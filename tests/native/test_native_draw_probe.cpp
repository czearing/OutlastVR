#include "isolated_d3d9.h"
#include "native_draw_probe.h"
#include <d3dcompiler.h>
#include <iostream>

using namespace olvr;
namespace {
void compile(const char* source,const char* profile,Com<ID3DBlob>& code) {
    Com<ID3DBlob> errors;
    const auto result=D3DCompile(source,std::strlen(source),"material-probe-fixture",nullptr,nullptr,
        "main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),errors.put());
    if (FAILED(result) && errors.get())
        throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize()));
    gpu_check(result,"Compile diagnostic fixture shader");
}
}
int main(int argc,char** argv) {
    try {
        const bool frozen=argc==3 && std::string(argv[2])=="--fault-frozen";
        if ((argc!=2 && !frozen) || !std::filesystem::path(argv[1]).is_absolute())
            throw std::invalid_argument("Expected a new absolute probe-evidence directory");
        IsolatedDevice isolated(512,256);
        auto& device=*isolated.device.get();
        Com<IDirect3DTexture9> target,texture;
        gpu_check(device.CreateTexture(512,256,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,target.put(),nullptr),"Create diagnostic atlas fixture");
        tag_atlas_texture(*target.get(),OutlastTargetRole::scene_color,256,256);
        Com<IDirect3DSurface9> surface;
        gpu_check(target->GetSurfaceLevel(0,surface.put()),"Get diagnostic atlas fixture");
        gpu_check(device.SetRenderTarget(0,surface.get()),"Bind diagnostic atlas fixture");
        gpu_check(device.SetDepthStencilSurface(nullptr),"Detach fixture depth");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable fixture depth test");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable fixture face culling");
        gpu_check(device.SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"Disable fixture blending");
        gpu_check(device.SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE),"Select linear fixture output");
        gpu_check(device.CreateTexture(16,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
            texture.put(),nullptr),"Create diagnostic material fixture");
        D3DLOCKED_RECT locked{};
        gpu_check(texture->LockRect(0,&locked,nullptr,0),"Lock material fixture");
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<16;++x)
            reinterpret_cast<DWORD*>(static_cast<char*>(locked.pBits)+y*locked.Pitch)[x]=0xffffffff;
        gpu_check(texture->UnlockRect(0),"Unlock material fixture");
        gpu_check(device.SetTexture(0,texture.get()),"Bind material fixture texture");
        Com<IDirect3DCubeTexture9> cube;
        Com<IDirect3DVolumeTexture9> volume;
        gpu_check(device.CreateCubeTexture(32,2,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
            cube.put(),nullptr),"Create diagnostic cube fixture");
        gpu_check(device.CreateVolumeTexture(16,8,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
            volume.put(),nullptr),"Create diagnostic volume fixture");
        gpu_check(device.SetTexture(1,cube.get()),"Bind diagnostic cube fixture");
        gpu_check(device.SetTexture(2,volume.get()),"Bind diagnostic volume fixture");
        Com<ID3DBlob> vs,ps;
        compile("float4 VertexOffset:register(c7);"
            "float4 main(float4 p:POSITION):POSITION{return p+float4(VertexOffset.xy,0,0);}","vs_3_0",vs);
        compile("float4 TestTint:register(c5); sampler2D Material:register(s0);"
            "float4 main(float2 p:VPOS):COLOR{return float4(TestTint.rgb*tex2D(Material,p/512).rgb,1);}","ps_3_0",ps);
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),vertex.put()),
            "Create diagnostic fixture vertex shader");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),pixel.put()),
            "Create diagnostic fixture pixel shader");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind diagnostic vertex fixture");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind diagnostic pixel fixture");
        gpu_check(device.SetFVF(D3DFVF_XYZ|D3DFVF_TEX1),"Bind diagnostic vertex format");
        struct Vertex {float x,y,z,u,v;};
        const Vertex triangle[]{{-1,-1,.5f,0,0},{-1,3,.5f,0,0},{3,-1,.5f,0,0}};
        NativeDrawProbe probe(std::filesystem::path(argv[1]),20);
        gpu_check(device.SetVertexShader(nullptr),"Select fixed-function vertex provenance control");
        probe.retain_bound_shaders(device,19);
        probe.retain_bound_shaders(device,20);
        gpu_check(device.SetPixelShader(nullptr),"Select fully fixed-function provenance control");
        probe.retain_bound_shaders(device,20);
        gpu_check(device.SetVertexShader(vertex.get()),"Select pixel-only fixed-function provenance control");
        probe.retain_bound_shaders(device,20);
        bool refused_missing=false;
        try {probe.retain_bound_shaders(device);} catch (const std::runtime_error&) {refused_missing=true;}
        if (!refused_missing) throw std::runtime_error("Refused native pair accepted a missing pixel source");
        gpu_check(device.SetPixelShader(pixel.get()),"Restore programmable diagnostic pixel fixture");
        const float colors[2][4]{{1,.5f,.25f,1},{.125f,.75f,.5f,std::numeric_limits<float>::quiet_NaN()}};
        for (unsigned frame=0;frame<2;++frame) {
            const float offset[]{0,0,float(frame+1),float(frame+2)};
            gpu_check(device.SetVertexShaderConstantF(7,offset,1),"Set changing native vertex fixture");
            gpu_check(device.SetPixelShaderConstantF(5,colors[frozen?0:frame],1),
                "Set changing native material fixture");
            gpu_check(device.SetSamplerState(0,D3DSAMP_ADDRESSU,frame?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP),
                "Set changing fixture sampler address");
            gpu_check(device.SetSamplerState(0,D3DSAMP_SRGBTEXTURE,frame),"Set changing fixture sampler color space");
            gpu_check(device.SetRenderState(D3DRS_ALPHATESTENABLE,frame),"Set changing fixture alpha test");
            gpu_check(device.SetRenderState(D3DRS_ALPHAREF,128),"Set fixture alpha reference");
            gpu_check(device.SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER),"Set fixture alpha comparison");
            const float bias=frame?.00025f:0;
            DWORD bits=0;std::memcpy(&bits,&bias,sizeof(bits));
            gpu_check(device.SetRenderState(D3DRS_DEPTHBIAS,bits),"Set changing fixture depth bias");
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0),"Clear fixture output");
            gpu_check(device.BeginScene(),"Begin material fixture draw");
            gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex)),
                "Draw material diagnostic fixture");
            gpu_check(device.EndScene(),"End material fixture draw");
            probe.capture(device,20);
            float current[4]{};
            gpu_check(device.GetPixelShaderConstantF(5,current,1),"Verify diagnostic caller constants");
            if (std::memcmp(current,colors[frozen?0:frame],sizeof(current)))
                throw std::runtime_error("Diagnostic probe mutated native material constants");
            DWORD current_bits=0;
            gpu_check(device.GetRenderState(D3DRS_DEPTHBIAS,&current_bits),"Verify diagnostic caller raster state");
            if (current_bits!=bits) throw std::runtime_error("Diagnostic probe mutated native raster state");
        }
        Com<IDirect3DTexture9> hdr_target;
        Com<IDirect3DSurface9> hdr_surface;
        gpu_check(device.CreateTexture(512,256,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,
            D3DPOOL_DEFAULT,hdr_target.put(),nullptr),"Create exact HDR diagnostic fixture");
        tag_atlas_texture(*hdr_target.get(),OutlastTargetRole::scene_color,256,256);
        gpu_check(hdr_target->GetSurfaceLevel(0,hdr_surface.put()),"Get HDR diagnostic surface");
        gpu_check(device.SetRenderTarget(0,hdr_surface.get()),"Bind HDR diagnostic fixture");
        gpu_check(device.SetRenderState(D3DRS_ALPHATESTENABLE,FALSE),"Disable HDR fixture alpha test");
        const float hdr_color[]{.0009765625f,2.5f,-.25f,1};
        gpu_check(device.SetPixelShaderConstantF(5,hdr_color,1),"Set sub-byte, HDR and negative fixture channels");
        gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0),"Clear HDR diagnostic fixture");
        gpu_check(device.BeginScene(),"Begin HDR fixture draw");
        gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex)),"Draw HDR fixture");
        gpu_check(device.EndScene(),"End HDR fixture draw");
        NativeDrawProbe hdr_probe(std::filesystem::path(argv[1]),21,L"hdr-draw-probe",true);
        hdr_probe.capture(device,21);
        const auto raw_path=std::filesystem::path(argv[1])/L"hdr-draw-probe"/L"draw_1.rgba16f";
        std::ifstream raw(raw_path,std::ios::binary|std::ios::ate);
        if (!raw || raw.tellg()!=512*256*8)
            throw std::runtime_error("Exact HDR diagnostic has an invalid sample extent");
        raw.seekg(0);
        std::vector<std::uint16_t> samples(512*256*4);
        raw.read(reinterpret_cast<char*>(samples.data()),samples.size()*sizeof(samples[0]));
        if (!raw) throw std::runtime_error("Cannot independently read exact HDR diagnostic");
        constexpr std::uint16_t expected[]{0x1400,0x4100,0xb400,0x3c00};
        for (std::size_t i=0;i<samples.size();++i)
            if (samples[i]!=expected[i%4])
                throw std::runtime_error("HDR diagnostic clipped, quantized or reordered a native sample");
        float current_hdr[4]{};
        gpu_check(device.GetPixelShaderConstantF(5,current_hdr,1),"Verify HDR caller constants");
        if (std::memcmp(current_hdr,hdr_color,sizeof(hdr_color)))
            throw std::runtime_error("HDR diagnostic mutated native caller constants");
        std::cout<<"CAPTURED: two changing native material states; three texture kinds; nonfinite bits; "
            "partial-stage native shader provenance; refused missing source rejection; caller state retained; "
            "524288 exact HDR channels including sub-byte, above-one and negative values\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
