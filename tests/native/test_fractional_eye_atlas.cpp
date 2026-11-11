#include "isolated_d3d9.h"
#include "sm3_runtime.h"
#include <d3dcompiler.h>
#include <iostream>
#include <cmath>

using namespace olvr;

int main() {
    try {
        constexpr UINT width=64,height=8,source_width=1024;
        IsolatedDevice isolated(width,height);
        auto& device=*isolated.device.get();
        const auto compile=[](const char* text,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto status=D3DCompile(text,std::strlen(text),"fractional-eye",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(status))
                throw std::runtime_error(errors.get()?std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                    "Fractional-eye fixture shader compilation failed");
        };
        Com<ID3DBlob> original_vertex,original_pixel,fixture_vertex;
        compile("float4 main(float2 p:POSITION):POSITION{return float4(p,0,1);}",
            "vs_3_0",original_vertex);
        compile(
            "SamplerState Scene:register(s0);float4 main(float2 uv:TEXCOORD0):COLOR{return tex2D(Scene,uv);}",
            "ps_3_0",original_pixel);
        compile(
            "struct O{float4 p:POSITION;float2 uv:TEXCOORD0;float eye:TEXCOORD15;};"
            "O main(float2 p:POSITION,float2 uv:TEXCOORD0,float eye:TEXCOORD1)"
            "{O o;o.p=float4(p,0,1);o.uv=uv;o.eye=eye;return o;}","vs_3_0",fixture_vertex);
        sm3::Words vs(original_vertex->GetBufferSize()/4),ps(original_pixel->GetBufferSize()/4);
        std::memcpy(vs.data(),original_vertex->GetBufferPointer(),original_vertex->GetBufferSize());
        std::memcpy(ps.data(),original_pixel->GetBufferPointer(),original_pixel->GetBufferSize());
        const auto patched=sm3::patch_pair(vs,ps,{1u});
        if (patched.packed || patched.eye_semantic!=15 || patched.component ||
            patched.atlas_sampler_constants.size()!=1)
            throw std::runtime_error("Fractional-eye fixture varying layout changed");
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        const std::vector<DWORD> pixel_words(patched.pixel.begin(),patched.pixel.end());
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(fixture_vertex->GetBufferPointer()),
            vertex.put()),"Create fractional-eye fixture vertex");
        gpu_check(device.CreatePixelShader(pixel_words.data(),pixel.put()),"Create production atlas pixel shader");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind isolated fractional-eye input");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind production atlas sampling rewrite");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,8,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
            {0,16,D3DDECLTYPE_FLOAT1,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create fractional-eye declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind fractional-eye declaration");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable fixture culling");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable fixture depth");
        Com<IDirect3DTexture9> texture;
        gpu_check(device.CreateTexture(source_width*2,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
            texture.put(),nullptr),"Create independent eye source colors");
        D3DLOCKED_RECT lock{};
        gpu_check(texture->LockRect(0,&lock,nullptr,0),"Lock source eye texels");
        constexpr DWORD left=0xff204060,even=0xff1090e0,odd=0xffe09010;
        for (UINT x=0;x<source_width*2;++x)
            static_cast<DWORD*>(lock.pBits)[x]=x<source_width?left:(x%2?odd:even);
        gpu_check(texture->UnlockRect(0),"Unlock source eye texels");
        gpu_check(device.SetTexture(0,texture.get()),"Bind eye atlas source");
        for (const auto state:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            gpu_check(device.SetSamplerState(0,state,D3DTEXF_POINT),"Preserve native point sampling");
        gpu_check(device.SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable fixture mipmaps");
        gpu_check(device.SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP),"Clamp fixture U");
        gpu_check(device.SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP),"Clamp fixture V");
        const float guard[]{float(width)/2-.5f,2,-1,0};
        const float texel=.5f/(source_width*2);
        const float binding[]{.5f,.5f,texel,.5f-texel};
        gpu_check(device.SetPixelShaderConstantF(patched.guard,guard,1),"Bind actual atlas seam guard");
        gpu_check(device.SetPixelShaderConstantF(patched.atlas_sampler_constants.front().second,binding,1),
            "Bind unchanged production atlas sampling constants");
        Com<IDirect3DSurface9> output,readback;
        gpu_check(device.GetRenderTarget(0,output.put()),"Get fixture color output");
        gpu_check(device.CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create fractional-eye readback");
        struct Vertex {float x,y,u,v,eye;};
        unsigned checked=0;
        for (unsigned test=0;test<3;++test) {
            const float eye=test==1?.99975f:test==2?1.00025f:1.f;
            const float u=(512.f+(test==2?-.03125f:.03125f))/source_width;
            std::vector<Vertex> vertices;
            for (unsigned half=0;half<2;++half) {
                const float a=half?0.f:-1.f,b=half?1.f:0.f,e=half?eye:0.f;
                for (const auto& p:std::array<std::array<float,2>,6>{{
                        {a,-1},{b,-1},{b,1},{a,-1},{b,1},{a,1}}})
                    vertices.push_back({p[0],p[1],u,.5f,e});
            }
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear fractional-eye output");
            gpu_check(device.BeginScene(),"Begin fractional-eye test");
            gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,4,vertices.data(),sizeof(Vertex)),
                "Execute isolated production pixel routing once");
            gpu_check(device.EndScene(),"End fractional-eye test");
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read actual eye routing");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize routing pixels");
            unsigned mismatches=0;
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
                const auto actual=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                const auto expected=x<width/2?left:test==2?odd:even;
                if (actual!=expected) ++mismatches;
                ++checked;
            }
            gpu_check(readback->UnlockRect(),"Unlock routing readback");
            if (mismatches)
                throw std::runtime_error("Fractional eye changes native point-sampled texels: case="+
                    std::to_string(test)+" eye="+std::to_string(eye)+
                    " mismatched_pixels="+std::to_string(mismatches));
        }
        std::cout<<"PASS: "<<checked<<" GPU pixels; exact and both fractional eye labels preserve point texels\n";
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
