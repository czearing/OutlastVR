#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include "stereo_reconstruction.h"
#include <iostream>

using namespace olvr;
namespace {
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
sm3::Words compile(const char* source,const char* profile) {
    Com<ID3DBlob> code,errors;
    const auto status=D3DCompile(source,std::strlen(source),"hit-comfort-fixture",nullptr,nullptr,
        "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
    if (FAILED(status)) throw std::runtime_error(errors.get()?
        static_cast<const char*>(errors->GetBufferPointer()):"Hit fixture compilation failed");
    sm3::Words words(code->GetBufferSize()/4);
    std::memcpy(words.data(),code->GetBufferPointer(),code->GetBufferSize());
    return words;
}
sm3::Words bytecode(IDirect3DPixelShader9& shader) {
    UINT bytes=0;
    gpu_check(shader.GetFunction(nullptr,&bytes),"Read hit variant extent");
    sm3::Words words(bytes/4);
    gpu_check(shader.GetFunction(words.data(),&bytes),"Read hit variant");
    return words;
}
void check_scope() {
    const auto plain=compile("float4 main():COLOR {return float4(.1,.2,.3,1);}","ps_3_0");
    require(outlast_without_hit_distortion(plain)==plain,"Unrelated native effects were modified");
    const auto color_only=compile(R"(
        float4 HurtCircleParams;float2 HurtParams;
        float4 main(float2 uv:TEXCOORD0):COLOR {
            return lerp(HurtCircleParams,float4(uv,0,1),HurtParams.x);
        })","ps_3_0");
    require(outlast_without_hit_distortion(color_only)==color_only,"Non-distorting hurt tint was modified");
    const auto unknown=compile(R"(
        float4 HurtCircleParams;float2 HurtParams;
        float4 main(float2 uv:TEXCOORD0):COLOR {
            return float4(uv+HurtParams.y,HurtCircleParams.x,1);
        })","ps_3_0");
    bool rejected=false;
    try {outlast_without_hit_distortion(unknown);}
    catch (const std::invalid_argument&) {rejected=true;}
    require(rejected,"Unknown hit distortion was silently accepted");
}
}
int main(int argc,char** argv) {
    try {
        const bool retail=argc==3 && std::string(argv[1])=="--retail";
        const bool packed=argc==2 && std::string(argv[1])=="--packed";
        const bool fault=argc==2 && std::string(argv[1])=="--fault-hit-distortion";
        const bool cpu=argc==2 && std::string(argv[1])=="--cpu";
        require(argc==1 || retail || packed || fault || cpu,"Unknown hit shader fixture arguments");
        const auto vertex=compile(R"(
            float4x4 ViewProjectionMatrix:register(c0);
            struct Output {float4 position:POSITION;float2 uv:TEXCOORD0;};
            Output main(float2 p:POSITION) {
                Output o;o.position=mul(float4(p,0,1),ViewProjectionMatrix);
                o.uv=p*float2(.5,-.5)+.5+float2(.5/32,.5/32);return o;
            })","vs_3_0");
        const std::string pixel_source=std::string(packed?"#define PACKED_HIT 1\n":"")+R"(
            float4 HurtCircleParams:register(c13);
            float4 HurtColor:register(c14);
            float2 HurtParams:register(c15);
            float2 MovieOffset:register(c16);
            float4 main(float2 uv:TEXCOORD0):COLOR {
                float2 original=uv+MovieOffset;
                float radius=lerp(HurtCircleParams.x,HurtCircleParams.y,HurtParams.x);
                float hardness=lerp(HurtCircleParams.z,HurtCircleParams.w,HurtParams.x);
                float mask=saturate((1-length(original-.5)/radius)/(1-hardness));
                mask*=mask;
                float sine,cosine;sincos(HurtParams.y,sine,cosine);
                float2 v=(original-.5)*.75;
                float2 rotated=float2(v.x*cosine-v.y*sine,v.x*sine+v.y*cosine)+.5;
                float2 sampleUV=lerp(rotated,original,mask);
                #ifdef PACKED_HIT
                return float4(lerp(HurtColor.x,HurtColor.y,mask),sampleUV,1);
                #else
                return float4(sampleUV,lerp(HurtColor.x,HurtColor.y,mask),1);
                #endif
            })";
        const auto pixel=!retail?compile(pixel_source.c_str(),"ps_3_0"):[&] {
                std::ifstream input(argv[2],std::ios::binary|std::ios::ate);
                require(bool(input),"Cannot open selected retail shader");
                const auto bytes=input.tellg();
                require(bytes>=8 && bytes<=1048576 && bytes%4==0,"Invalid selected retail shader extent");
                sm3::Words words(static_cast<std::size_t>(bytes)/4);
                input.seekg(0);input.read(reinterpret_cast<char*>(words.data()),bytes);
                require(bool(input),"Cannot read complete selected retail shader");return words;
            }();
        const auto comfort=[&] {
            try {return outlast_without_hit_distortion(pixel);}
            catch (const std::invalid_argument&) {
                Com<ID3DBlob> assembly;
                gpu_check(D3DDisassemble(pixel.data(),pixel.size()*4,0,nullptr,assembly.put()),
                    "Disassemble rejected hit shader");
                std::cerr.write(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
                throw;
            }
        }();
        if (!retail) require(comfort.size()<pixel.size(),"Hit UV blend was not replaced selectively");
        if (!retail) check_scope();
        if (cpu) {
            const auto packed_source="#define PACKED_HIT 1\n"+pixel_source;
            const auto packed_program=compile(packed_source.c_str(),"ps_3_0");
            require(outlast_without_hit_distortion(packed_program)!=packed_program,
                "Packed hit UV blend was not isolated");
            std::cout<<"PASS: native hit shader scope; XY/YZ compiler forms; tint-only and unrelated shaders unchanged; unknown shape rejected\n";
            return 0;
        }
        IsolatedDevice isolated(32,32);
        auto& device=*isolated.device.get();
        Com<IDirect3DVertexShader9> vs;
        Com<IDirect3DPixelShader9> original,patched;
        const std::vector<DWORD> vertex_driver(vertex.begin(),vertex.end()),pixel_driver(pixel.begin(),pixel.end()),
            comfort_driver(comfort.begin(),comfort.end());
        gpu_check(device.CreateVertexShader(vertex_driver.data(),vs.put()),"Create hit fixture vertex");
        gpu_check(device.CreatePixelShader(pixel_driver.data(),original.put()),"Create original hit shader");
        gpu_check(device.CreatePixelShader(fault?pixel_driver.data():comfort_driver.data(),patched.put()),"Create comfort hit shader");
        gpu_check(device.SetVertexShader(vs.get()),"Bind hit fixture vertex");
        gpu_check(device.SetPixelShader(original.get()),"Bind native hit shader");
        for (const bool enabled:{false,true}) {
            D3D9ShaderVariants variants(device,enabled);
            auto& pipeline=variants.current(0,false,false);
            const auto expected=sm3::patch_pair(vertex,enabled?pixel:comfort,
                {0,false,false,false,false,false,false,0});
            require(bytecode(*pipeline.diagnostic_binding().pixel)==expected.pixel,
                "Production shader variant ignored hit_distortion configuration");
        }
        if (retail) {
            std::cout<<"PASS: actual retail hit shader accepted by GPU and both production policy variants; changed="
                <<(comfort!=pixel)<<'\n';
            return 0;
        }
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create hit declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind hit declaration");
        const auto identity=identity_matrix();
        gpu_check(device.SetVertexShaderConstantF(0,identity.data(),4),"Bind hit projection");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable hit fixture depth");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable hit fixture culling");
        const float circle[]{.6f,.4f,.1f,.3f},color[]{.2f,.9f,0,0},movie[]{.02f,-.01f,0,0};
        gpu_check(device.SetPixelShaderConstantF(13,circle,1),"Bind authored hurt mask");
        gpu_check(device.SetPixelShaderConstantF(14,color,1),"Bind damage color");
        gpu_check(device.SetPixelShaderConstantF(16,movie,1),"Bind independent scripted movie UV offset");
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        Com<IDirect3DSurface9> target,readback;
        gpu_check(device.GetRenderTarget(0,target.put()),"Get hit fixture output");
        gpu_check(device.CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create hit readback");
        unsigned different=0;
        for (const float angle:{0.f,.4f,-.8f}) {
            const float params[]{.7f,angle,0,0};
            gpu_check(device.SetPixelShaderConstantF(15,params,1),"Bind hit strength and pulsation");
            std::array<std::array<DWORD,1024>,2> images{};
            for (unsigned policy=0;policy<2;++policy) {
                gpu_check(device.SetPixelShader(policy?patched.get():original.get()),"Bind tested hit policy");
                gpu_check(device.BeginScene(),"Begin hit policy frame");
                gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,quad,sizeof(quad[0])),"Draw hit policy frame");
                gpu_check(device.EndScene(),"End hit policy frame");
                gpu_check(device.GetRenderTargetData(target.get(),readback.get()),"Read actual hit pixels");
                D3DLOCKED_RECT lock{};
                gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize hit pixels");
                for (unsigned y=0;y<32;++y) std::memcpy(images[policy].data()+y*32,
                    static_cast<BYTE*>(lock.pBits)+y*lock.Pitch,32*sizeof(DWORD));
                gpu_check(readback->UnlockRect(),"Finish hit pixels");
            }
            for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x) {
                const auto native=images[0][y*32+x],actual=images[1][y*32+x];
                const unsigned tint=packed?16:0,horizontal=packed?8:16,vertical=packed?0:8;
                require(((native>>tint)&255)==((actual>>tint)&255),"Comfort policy changed the actual damage mask/tint");
                require(std::abs(int((actual>>horizontal)&255)-((x+.5)/32+.02)*255)<2 &&
                    std::abs(int((actual>>vertical)&255)-((y+.5)/32-.01)*255)<2,
                    "Comfort policy retained hit UV distortion or removed scripted movie offsets");
                different+=native!=actual;
            }
        }
        require(different>300,"Hit comfort negative control did not expose native warping");
        std::cout<<"PASS: 3072 GPU pixels; hit rotation and zoom removed; damage mask/tint and movie UV preserved; opt-in retained\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
