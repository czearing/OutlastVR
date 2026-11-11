#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include "isolated_d3d12_counter.h"
#include "stereo_reconstruction.h"
#include <iostream>

using namespace olvr;

int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-scissor";
        if (argc>2 || (argc==2 && !fault)) throw std::invalid_argument("Unknown world-scissor GPU control");
        constexpr UINT width=128,height=96;
        IsolatedDevice isolated(width*2,height);
        auto& device=*isolated.device.get();
        const char* vertex=R"(
            #pragma pack_matrix(row_major)
            float4x4 ViewProjectionMatrix:register(c0);
            float4 main(float3 p:POSITION):POSITION {return mul(ViewProjectionMatrix,float4(p,1));}
        )";
        const char* pixel="float4 main():COLOR {return float4(.8,.4,.2,1);}";
        const auto compile=[](const char* source,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto result=D3DCompile(source,std::strlen(source),"world-scissor",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(result)) throw std::runtime_error(errors.get()?
                std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                "World-scissor fixture shader compile failed");
        };
        Com<ID3DBlob> vertex_code,pixel_code;
        compile(vertex,"vs_3_0",vertex_code);compile(pixel,"ps_3_0",pixel_code);
        Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vertex_code->GetBufferPointer()),vs.put()),"Create world-scissor VS");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(pixel_code->GetBufferPointer()),ps.put()),"Create world-scissor PS");
        gpu_check(device.SetVertexShader(vs.get()),"Bind world-scissor native VS");
        gpu_check(device.SetPixelShader(ps.get()),"Bind world-scissor native PS");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create world-scissor declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind world-scissor declaration");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable world-scissor fixture depth");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable world-scissor fixture winding cull");
        Com<IDirect3DSurface9> backbuffer,readback;
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),"Get world-scissor atlas");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create world-scissor readback");
        const TrackingFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,-1},1,{0,0,0}};
        const auto original=eye_constants(frame,{{0,0,0},{0,0,0,1}},{-.35f,.35f,.35f,-.35f},.125f,1000,.999f);
        gpu_check(device.SetVertexShaderConstantF(0,original.data(),4),"Upload original mono light projection");
        StereoReconstructionContext context{};
        context.original_camera=frame;
        std::copy_n(original.begin(),16,context.original_projection.begin());
        const float quad[][3]{{-.35f,-.25f,3},{.35f,-.25f,3},{.35f,.25f,3},
            {-.35f,-.25f,3},{.35f,.25f,3},{-.35f,.25f,3}};
        D3D9ShaderVariants variants(device);
        IsolatedD3D12DrawCounter counter(isolated.translation_device(),true,isolated.translation_queue());
        UINT draws=0,pixels=0;
        for (unsigned sample=0;sample<12;++sample) {
            const float yaw=sample==0 || sample==7?-.5f:sample==1 || sample==6?.5f:sample==2?.25f:0;
            const float pitch=sample==8?.5f:sample==9?-.5f:0;
            const Quaternion orientation{std::sin(pitch*.5f)*std::cos(yaw*.5f),
                std::cos(pitch*.5f)*std::sin(yaw*.5f),-std::sin(pitch*.5f)*std::sin(yaw*.5f),
                std::cos(pitch*.5f)*std::cos(yaw*.5f)};
            StereoViewConstants views{};
            for (unsigned eye=0;eye<2;++eye)
                views[eye]=eye_constants(frame,{{eye?.032f:-.032f,0,0},
                    orientation},
                    eye?EyeFov{-.7f,.8f,.7f,-.7f}:EyeFov{-.8f,.7f,.7f,-.7f},.125f,1000,.999f);
            const RECT source=sample==3 || sample==6?RECT{0,28,88,68}:
                sample==4 || sample==8?RECT{40,0,88,68}:
                sample==5 || sample==7?RECT{40,28,128,68}:
                sample==9?RECT{40,28,88,96}:sample==10?RECT{40,28,40,28}:
                sample==11?RECT{30,32,98,64}:RECT{40,28,88,68};
            float geometry[6][3]{};
            std::memcpy(geometry,quad,sizeof(quad));
            for (auto& point:geometry) {
                if (sample==11) {
                    point[0]*=.02f/.35f;point[1]*=.01f/.25f;point[2]=.13f;
                }
                if (sample==6) point[0]-=3;
                if (sample==7) point[0]+=3;
                if (sample==8) point[1]+=3;
                if (sample==9) point[1]-=3;
            }
            std::vector<DWORD> reference;
            for (unsigned filtered=0;filtered<2;++filtered) {
                const D3DVIEWPORT9 viewport{0,0,width*2,height,0,1};
                gpu_check(device.SetViewport(&viewport),"Set world-scissor atlas viewport");
                gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE),"Clear complete world-scissor atlas");
                gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear world-scissor test image");
                gpu_check(device.SetScissorRect(&source),"Set original mono world-light bounds");
                gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,filtered),"Select world-scissor control");
                auto& pipeline=variants.current(0,false,true,filtered!=0,true,!fault);
                pipeline.set_reconstruction_context(context);
                counter.reset();
                gpu_check(device.BeginScene(),"Begin world-scissor GPU case");
                const auto result=pipeline.draw_primitive_up(D3DPT_TRIANGLELIST,2,geometry,sizeof(geometry),sizeof(geometry[0]),views);
                if (!result.succeeded()) throw std::runtime_error("World-scissor paired draw or caller restoration failed");
                gpu_check(device.EndScene(),"End world-scissor GPU case");
                gpu_check(device.GetRenderTargetData(backbuffer.get(),readback.get()),"Read world-scissor GPU output");
                D3DLOCKED_RECT locked{};
                gpu_check(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Wait for completed world-scissor pixels");
                std::vector<DWORD> image(width*2*height);
                for (UINT y=0;y<height;++y) std::memcpy(image.data()+y*width*2,
                    static_cast<const char*>(locked.pBits)+y*locked.Pitch,width*2*4);
                gpu_check(readback->UnlockRect(),"Unlock world-scissor GPU output");
                const auto executed=counter.events();
                if (counter.indexed_calls!=1 || counter.plain_calls || counter.instances!=2 ||
                    executed.size()!=1 || !executed[0].indexed || executed[0].instances!=2)
                    throw std::runtime_error("World scissor introduced duplicated or non-instanced geometry");
                RECT restored{};DWORD enabled=0;
                gpu_check(device.GetScissorRect(&restored),"Check original world scissor restoration");
                gpu_check(device.GetRenderState(D3DRS_SCISSORTESTENABLE,&enabled),"Check original world scissor enable");
                if (std::memcmp(&source,&restored,sizeof(source)) || enabled!=filtered)
                    throw std::runtime_error("Tracked world scissor changed caller clipping state");
                if (!filtered) {
                    reference=image;
                    if (std::count_if(reference.begin(),reference.end(),[](DWORD p){return (p&0xffffff)!=0;})<32)
                        throw std::runtime_error("Unclipped reference light is not measurable");
                } else {
                    if (sample==10) std::fill(reference.begin(),reference.end(),0xff000000);
                    if (image!=reference)
                        throw std::runtime_error("Mono-derived scissor removed visible tracked-eye light pixels");
                    pixels+=UINT(image.size());
                }
                ++draws;
            }
        }
        std::cout<<"PASS: world-light scissors match independently unscissored eye images; pixel_checks="<<pixels
            <<"; physical_draws="<<draws<<"; physical_instances="<<draws*2
            <<"; queue_executed_draws="<<draws<<"; tracked yaw/IPD/asymmetric FOV; distinct near-eye clips;"
            <<" truncated bounds; empty clip; caller restored\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
