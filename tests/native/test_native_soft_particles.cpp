#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include "outlast_target_roles.h"
#include "stereo_reconstruction.h"
#ifdef OLVR_ISOLATED_9ON12
#include "isolated_d3d12_counter.h"
#endif
#include <iostream>

using namespace olvr;

int main(int argc,char** argv) {
    try {
        const bool shared_depth=argc==3 && std::string(argv[2])=="--fault-shared-depth";
        if (argc!=2 && !shared_depth) throw std::invalid_argument("Supply the retained native soft-particle pixel shader");
        constexpr UINT width=64,height=16;
        IsolatedDevice isolated(width*2,height);
        auto& device=*isolated.device.get();
        const auto native=read_shader(std::filesystem::path(argv[1]));
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.CreatePixelShader(native.data(),pixel.put()),"Create retained retail soft-particle shader");
        const char* source=R"(
            struct Output {
                float4 position:POSITION;
                float2 uv:TEXCOORD0;
                float4 color:TEXCOORD1;
                float4 fog:TEXCOORD4;
                float4 screen:TEXCOORD5;
            };
            Output main(float2 position:POSITION) {
                Output result;
                result.position=float4(position,.5,1);
                result.uv=position*float2(.5,-.5)+.5;
                result.color=float4(5,2,1,1);
                result.fog=float4(0,0,0,1);
                result.screen=float4(position*100,100,100);
                return result;
            })";
        Com<ID3DBlob> code,errors;
        const auto compiled=D3DCompile(source,std::strlen(source),"native-soft-particle-oracle",nullptr,nullptr,
            "main","vs_3_0",D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
        if (FAILED(compiled)) throw std::runtime_error(errors.get()?
            std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
            "Native soft-particle vertex fixture compilation failed");
        Com<IDirect3DVertexShader9> vertex;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),vertex.put()),
            "Create explicit particle interpolator fixture");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind particle interpolator fixture");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind retail soft-particle pixel shader");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create particle fixture declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind particle fixture declaration");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable unrelated fixture depth rejection");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable unrelated fixture culling");
        gpu_check(device.SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"Retain particle alpha blending");
        gpu_check(device.SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA),"Retain particle source alpha");
        gpu_check(device.SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA),"Retain particle destination alpha");
        Com<IDirect3DTexture9> depth,material;
        gpu_check(device.CreateTexture(width*2,height,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,
            depth.put(),nullptr),"Create explicit different-eye scene depth");
        D3DLOCKED_RECT lock{};
        gpu_check(depth->LockRect(0,&lock,nullptr,0),"Lock scene-depth fixture");
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
            auto* value=reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)+x*4;
            value[0]=value[1]=value[2]=0;value[3]=x<width || shared_depth?.02f:.005f;
        }
        gpu_check(depth->UnlockRect(0),"Unlock scene-depth fixture");
        tag_atlas_texture(*depth.get(),OutlastTargetRole::scene_color,width,height);
        gpu_check(device.CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
            material.put(),nullptr),"Create explicit particle material");
        gpu_check(material->LockRect(0,&lock,nullptr,0),"Lock particle material");
        *static_cast<DWORD*>(lock.pBits)=0xffffffff;
        gpu_check(material->UnlockRect(0),"Unlock particle material");
        gpu_check(device.SetTexture(0,depth.get()),"Bind scene depth in native HDR alpha");
        for (UINT sampler=0;sampler<3;++sampler) {
            if (sampler) gpu_check(device.SetTexture(sampler,material.get()),"Bind particle material");
            for (const auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
                gpu_check(device.SetSamplerState(sampler,filter,D3DTEXF_POINT),"Retain native point lookup");
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable fixture mips");
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,FALSE),"Keep explicit fixtures linear");
        }
        const float parameters[]{0,0,0,1,.5f,-.5f,.5f+.5f/height,.5f+.5f/width,
            -1.998f,.999f,-.5005005f,-.5f};
        gpu_check(device.SetPixelShaderConstantF(0,parameters,3),"Bind native authored depth-fade constants");
        D3D9ShaderVariants variants(device);
        auto& pipeline=variants.current(1,true,false,false,true,true);
        StereoViewConstants views{};
        for (auto& eye:views) std::copy_n(identity_matrix().begin(),16,eye.begin());
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        gpu_check(device.BeginScene(),"Begin native soft-particle fixture");
        gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0),"Clear explicit particle background");
#ifdef OLVR_ISOLATED_9ON12
        IsolatedD3D12DrawCounter counter(isolated.translation_device());
        counter.reset();
#endif
        const auto submitted=pipeline.draw_primitive_up(D3DPT_TRIANGLELIST,2,quad,sizeof(quad),sizeof(quad[0]),views);
        if (!submitted.succeeded()) throw std::runtime_error("Native particle fixture submission failed");
        gpu_check(device.EndScene(),"End native soft-particle fixture");
        Com<IDirect3DSurface9> target,readback;
        gpu_check(device.GetRenderTarget(0,target.put()),"Get rendered particle fixture");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create particle fixture readback");
        gpu_check(device.GetRenderTargetData(target.get(),readback.get()),"Read actual native particle pixels");
        gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Inspect actual native particle pixels");
        bool correct=true;
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
            const float depth_alpha=x<width?.02f:.005f;
            const float fade=std::clamp((1.998f/(depth_alpha-(1-.999f))-100)/105,0.f,1.f);
            const DWORD value=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
            for (unsigned channel=0;channel<3;++channel) {
                const float color=channel==0?.5f:channel==1?.2f:.1f;
                const int expected=int(std::lround(255*color*fade));
                const int actual=(value>>(16-channel*8))&255;
                if (std::abs(actual-expected)>2) {
                    if (correct) std::cerr<<"Native particle mismatch x="<<x<<" y="<<y
                        <<" actual="<<actual<<" expected="<<expected<<'\n';
                    correct=false;
                }
            }
        }
        gpu_check(readback->UnlockRect(),"Unlock actual native particle pixels");
        if (!correct) throw std::runtime_error("Retail soft-particle depth fade does not use its own eye's native HDR alpha");
#ifdef OLVR_ISOLATED_9ON12
        if (counter.indexed_calls!=1 || counter.plain_calls || counter.instances!=2)
            throw std::runtime_error("Native soft-particle fixture duplicated its scene submission");
#endif
        std::cout<<"PASS: retail_soft_particle_PS; independent_depth_fade_formula; different_native_HDR_alpha_each_eye;"
            " actual_UP_geometry; pixels=2048; one_draw_two_instances";
#ifdef OLVR_ISOLATED_9ON12
        std::cout<<"; native_d3d12_draws="<<counter.indexed_calls<<"; native_d3d12_instances="<<counter.instances;
#endif
        std::cout<<'\n';
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
