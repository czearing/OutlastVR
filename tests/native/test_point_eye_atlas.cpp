#include "isolated_d3d9.h"
#include "sm3_runtime.h"
#include "d3d9_point_atlas.h"
#include <d3dcompiler.h>
#include <cmath>
#include <iomanip>
#include <iostream>

using namespace olvr;

int main(int argc,char** argv) {
    try {
        const std::string mode=argc==2?argv[1]:"";
        if (argc>2 || (!mode.empty() && mode!="--legacy" && mode!="--wrong-lookup" &&
                       mode!="--divergent-lod"))
            throw std::invalid_argument("Usage: test_point_eye_atlas.exe [--legacy | --wrong-lookup | --divergent-lod]");
        constexpr UINT width=64,height=8;
        IsolatedDevice isolated(width,height);
        auto& device=*isolated.device.get();
        const auto compile=[](const char* text,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto status=D3DCompile(text,std::strlen(text),"point-eye",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(status))
                throw std::runtime_error(errors.get()?std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                    "Point-eye fixture shader compilation failed");
        };
        Com<ID3DBlob> original_vertex,original_pixel,fixture_vertex;
        compile("float4 main(float2 p:POSITION):POSITION{return float4(p,0,1);}",
            "vs_3_0",original_vertex);
        compile(mode=="--divergent-lod"?
            "SamplerState Scene:register(s0);float4 main(float2 uv:TEXCOORD0):COLOR{"
            "[branch]if(uv.x>.5)return tex2Dlod(Scene,float4(uv.x,uv.y+.1,0,0));"
            "else return tex2Dlod(Scene,float4(uv.x,uv.y-.1,0,0));}":
            "SamplerState Scene:register(s0);float4 main(float2 uv:TEXCOORD0):COLOR{return tex2D(Scene,uv);}",
            "ps_3_0",original_pixel);
        compile("struct O{float4 p:POSITION;float2 uv:TEXCOORD0;float eye:TEXCOORD15;};"
            "O main(float2 p:POSITION,float2 uv:TEXCOORD0,float eye:TEXCOORD1)"
            "{O o;o.p=float4(p,0,1);o.uv=uv;o.eye=eye;return o;}",
            "vs_3_0",fixture_vertex);
        sm3::Words vs(original_vertex->GetBufferSize()/4),ps(original_pixel->GetBufferSize()/4);
        std::memcpy(vs.data(),original_vertex->GetBufferPointer(),original_vertex->GetBufferSize());
        std::memcpy(ps.data(),original_pixel->GetBufferPointer(),original_pixel->GetBufferSize());
        if (mode=="--divergent-lod") {
            bool branch=false;
            for (const auto& instruction:sm3::parse(ps))
                branch|=instruction.opcode==40 || instruction.opcode==41;
            if (!branch) throw std::runtime_error("Native divergent-LOD control was optimized out");
        }
        sm3::PixelOptions options{1u};
        options.point_atlas_samplers=mode=="--legacy"?0u:1u;
        const auto patched=sm3::patch_pair(vs,ps,options);
        if (patched.packed || patched.eye_semantic!=15 || patched.component ||
            patched.atlas_sampler_constants.size()!=1)
            throw std::runtime_error("Point-eye fixture varying layout changed");
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> native_pixel,atlas_pixel;
        const std::vector<DWORD> pixel_words(patched.pixel.begin(),patched.pixel.end());
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(fixture_vertex->GetBufferPointer()),
            vertex.put()),"Create point-eye vertex");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(original_pixel->GetBufferPointer()),
            native_pixel.put()),"Create unmodified native point shader");
        gpu_check(device.CreatePixelShader(pixel_words.data(),atlas_pixel.put()),
            "Create production atlas point shader");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind point-eye vertex");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,8,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
            {0,16,D3DDECLTYPE_FLOAT1,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create point-eye declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind point-eye declaration");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable point-eye culling");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable point-eye depth");
        for (const auto state:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            gpu_check(device.SetSamplerState(0,state,D3DTEXF_POINT),"Preserve native point filtering");
        gpu_check(device.SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable point-eye mipmaps");
        gpu_check(device.SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP),"Clamp point-eye U");
        gpu_check(device.SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP),"Clamp point-eye V");
        const float guard[]{float(width)/2-.5f,2,-1,0};
        gpu_check(device.SetPixelShaderConstantF(patched.guard,guard,1),"Bind production seam guard");
        Com<IDirect3DSurface9> output,readback;
        gpu_check(device.GetRenderTarget(0,output.put()),"Get point-eye output");
        gpu_check(device.CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create point-eye readback");
        struct Vertex {float x,y,u,v,eye;};
        const auto draw=[&](IDirect3DTexture9* texture,IDirect3DPixelShader9* shader,float u,float eye) {
            const Vertex vertices[]{
                {-1,-1,u,.5f,eye},{1,-1,u,.5f,eye},{1,1,u,.5f,eye},
                {-1,-1,u,.5f,eye},{1,1,u,.5f,eye},{-1,1,u,.5f,eye}};
            gpu_check(device.SetTexture(0,texture),"Bind point-eye source");
            gpu_check(device.SetPixelShader(shader),"Bind point-eye shader");
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0),"Clear prior point sample");
            gpu_check(device.BeginScene(),"Begin point-eye draw");
            gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,vertices,sizeof(Vertex)),
                "Draw unchanged native or production atlas shader");
            gpu_check(device.EndScene(),"End point-eye draw");
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read actual point sample");
            D3DLOCKED_RECT lock{};
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Lock point sample");
            const auto result=reinterpret_cast<const DWORD*>(
                static_cast<const char*>(lock.pBits)+height/2*lock.Pitch)[eye>.5f?width*3/4:width/4];
            gpu_check(readback->UnlockRect(),"Unlock point sample");
            return result;
        };
        unsigned checked=0,mismatches=0;
        D3D9PointAtlasLookup lookups;
        for (const UINT source_width:{1024u,1536u,1920u,2048u,2448u,2688u,3840u}) {
            Com<IDirect3DTexture9> native_texture,atlas_texture;
            const std::array<Com<IDirect3DTexture9>*,2> textures{{&native_texture,&atlas_texture}};
            for (unsigned index=0;index<textures.size();++index) {
                auto& texture=*textures[index];
                const auto extent=source_width*(index+1);
                gpu_check(device.CreateTexture(extent,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
                    texture.put(),nullptr),"Create native or paired point source");
                D3DLOCKED_RECT lock{};
                gpu_check(texture->LockRect(0,&lock,nullptr,0),"Lock native point source");
                for (UINT x=0;x<extent;++x)
                    static_cast<DWORD*>(lock.pBits)[x]=(x%source_width)%2?0xffe09010:0xff1090e0;
                gpu_check(texture->UnlockRect(0),"Unlock native point source");
            }
            const float texel=.5f/(source_width*2);
            const float binding[]{.5f,.5f,texel,.5f-texel};
            gpu_check(device.SetPixelShaderConstantF(patched.atlas_sampler_constants.front().second,binding,1),
                "Bind exact production atlas point constants");
            gpu_check(device.SetTexture(0,atlas_texture.get()),"Bind selected native point atlas");
            if (native_point_atlas_samplers(device,1u)!=1u)
                throw std::runtime_error("Native point filtering did not select the production lookup path");
            gpu_check(device.SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR),"Check native linear magnification");
            if (native_point_atlas_samplers(device,1u))
                throw std::runtime_error("Point lookup changed native linear filtering");
            gpu_check(device.SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT),"Restore native point filtering");
            if (mode=="--wrong-lookup")
                gpu_check(device.SetTexture(0,native_texture.get()),"Inject an incorrectly sized point selector");
            lookups.bind(device,patched.atlas_point_samplers);
            for (const UINT boundary:{1u,127u,511u,source_width/2,source_width-2}) {
                for (int subpixel=-2;subpixel<=2;++subpixel) for (int step=-3;step<=3;++step) {
                    float u=(float(boundary)+float(subpixel)/512)/source_width;
                    for (int i=0;i<std::abs(step);++i)
                        u=std::nextafter(u,step<0?-INFINITY:INFINITY);
                    const auto expected=draw(native_texture.get(),native_pixel.get(),u,0);
                    for (const unsigned eye:{0u,1u}) {
                        const auto actual=draw(atlas_texture.get(),atlas_pixel.get(),u,float(eye));
                        if (actual!=expected) {
                            if (!mismatches)
                                std::cout<<std::setprecision(12)<<"First native/atlas point mismatch: width="
                                    <<source_width<<" eye="<<eye<<" boundary="<<boundary
                                    <<" subpixel="<<subpixel<<" float_steps="<<step<<" uv="<<u
                                    <<" native="<<std::hex<<expected<<" atlas="<<actual<<std::dec<<'\n';
                            ++mismatches;
                        }
                        ++checked;
                    }
                }
            }
        }
        std::cout<<"Actual native/atlas point comparisons="<<checked<<" mismatches="<<mismatches<<'\n';
        if (mismatches) throw std::runtime_error("Atlas remapping changes native point samples at texel boundaries");
        std::cout<<"PASS: both discrete eyes preserve actual native point selection at near-texel boundaries\n";
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
