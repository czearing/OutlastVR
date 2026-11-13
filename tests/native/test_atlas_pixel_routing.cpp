#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include "outlast_target_roles.h"
#include "test_d3d9_state_snapshot.h"
#include "isolated_vtable_entry.h"
#ifdef OLVR_ISOLATED_9ON12
#include "isolated_d3d12_counter.h"
#endif
#include <chrono>
#include <iostream>

using namespace olvr;
namespace {
using PixelRead=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DPixelShader9**);
PixelRead original_pixel_read=nullptr;
unsigned pixel_reads=0;
HRESULT WINAPI count_pixel_read(IDirect3DDevice9* device,IDirect3DPixelShader9** pixel) {
    ++pixel_reads;
    return original_pixel_read(device,pixel);
}
}

int main(int argc,char** argv) {
    try {
        const std::string mode=argc>1?argv[1]:"";
        if (argc>2 || (!mode.empty() && mode!="--fault-atlas" && mode!="--fault-camera" &&
                       mode!="--fault-material" && mode!="--fault-scissor" && mode!="--fault-lookup-state" &&
                       mode!="--hit-effects" && mode!="--fault-hit-effects" &&
                       mode!="--overdraw-benchmark" && mode!="--hit-benchmark"))
            throw std::invalid_argument("Unknown atlas-routing control");
        const bool fault_atlas=mode=="--fault-atlas",fault_camera=mode=="--fault-camera";
        const bool fault_material=mode=="--fault-material";
        const bool fault_scissor=mode=="--fault-scissor";
        const bool fault_lookup_state=mode=="--fault-lookup-state";
        const bool benchmark=mode=="--overdraw-benchmark" || mode=="--hit-benchmark";
        const bool hit_effects=mode=="--hit-effects" || mode=="--fault-hit-effects" || mode=="--hit-benchmark";
        const bool fault_hit=mode=="--fault-hit-effects";
        const UINT eye_width=benchmark?1024:64,height=benchmark?512:16;
        const UINT repeats=benchmark?64:1,frames=benchmark?6:12;
        IsolatedDevice isolated(eye_width*2,height);
        auto& device=*isolated.device.get();
        const char* vertex_source=R"(
            float4x4 ViewProjectionMatrix:register(c0);
            struct Output {float4 position:POSITION;float2 uv:TEXCOORD0;};
            Output main(float2 position:POSITION) {
                Output output;output.position=mul(float4(position,0,1),ViewProjectionMatrix);
                output.uv=position*float2(.5,-.5)+.5+float2(.5/EYE_WIDTH,.5/EYE_HEIGHT);return output;
            })";
        const char* pixel_source=R"(
            SamplerState Scene:register(s0);
            SamplerState Material:register(s1);
            float4 Params:register(c0);
            float4 VisibleEyes:register(c1);
            float4 HitEffect:register(c2);
            float4 HitColor:register(c3);
            float4 CameraPositionPS:register(c6);
            float4 main(float2 uv:TEXCOORD0,float2 pixel:VPOS):COLOR {
                clip(lerp(VisibleEyes.x,VisibleEyes.y,step(1.5,CameraPositionPS.x)));
                float2 sampleUV=lerp(uv,pixel*Params.xy,Params.z);
                float2 centered=sampleUV-.5;
                sampleUV+=centered*dot(centered,centered)*HitEffect.x+HitEffect.yz;
                float4 scene=tex2D(Scene,sampleUV+float2(Params.w,0));
                float material=tex2D(Material,uv).r;
                float4 color=lerp(float4(scene.rg*material,CameraPositionPS.x*.25,1),HitColor,HitEffect.w);
                #ifdef TRANSPARENT_BENCHMARK
                color.a=.5;
                #endif
                return color;
            })";
        const auto width_text=std::to_string(eye_width),height_text=std::to_string(height);
        const D3D_SHADER_MACRO defines[]{
            {"EYE_WIDTH",width_text.c_str()},{"EYE_HEIGHT",height_text.c_str()},
            {benchmark?"TRANSPARENT_BENCHMARK":nullptr,benchmark?"1":nullptr},{nullptr,nullptr}};
        auto compile=[&](const char* source,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto status=D3DCompile(source,std::strlen(source),"atlas-routing",defines,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(status)) {
                const std::string message=errors.get()?std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"No diagnostics";
                throw std::runtime_error("Atlas fixture compile failed: "+message);
            }
        };
        Com<ID3DBlob> vertex_code,pixel_code;
        compile(vertex_source,"vs_3_0",vertex_code);
        compile(pixel_source,"ps_3_0",pixel_code);
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vertex_code->GetBufferPointer()),vertex.put()),"Create atlas vertex");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(pixel_code->GetBufferPointer()),pixel.put()),"Create atlas pixel");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind original atlas vertex");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind original atlas pixel");
        const float screen_projection[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        gpu_check(device.SetVertexShaderConstantF(0,screen_projection,4),"Bind original screen projection");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create atlas declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind original atlas declaration");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable atlas culling");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable atlas depth");
        if (benchmark) {
            gpu_check(device.SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"Enable transparent overdraw benchmark");
            gpu_check(device.SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA),"Preserve transparent source alpha");
            gpu_check(device.SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA),"Preserve transparent destination blend");
        }
        gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE),"Enable original caller scissor");
        Com<IDirect3DTexture9> scene,material;
        gpu_check(device.CreateTexture(eye_width*2,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,scene.put(),nullptr),"Create sampled scene atlas");
        gpu_check(device.CreateTexture(eye_width*2,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,material.put(),nullptr),"Create unsplit material with matching atlas dimensions");
        D3DLOCKED_RECT locked{};
        gpu_check(scene->LockRect(0,&locked,nullptr,0),"Lock sampled scene atlas");
        for (UINT y=0;y<height;++y) for (UINT x=0;x<eye_width*2;++x) {
            const DWORD value=benchmark?96+(x%eye_width)*126/eye_width+y*30/height:
                96+(x%eye_width)*2+y*2;
            reinterpret_cast<DWORD*>(static_cast<char*>(locked.pBits)+y*locked.Pitch)[x]=
                0xff000000|(x<eye_width?value<<16:value<<8);
        }
        gpu_check(scene->UnlockRect(0),"Unlock sampled scene atlas");
        gpu_check(material->LockRect(0,&locked,nullptr,0),"Lock material ramp");
        for (UINT y=0;y<height;++y) for (UINT x=0;x<eye_width*2;++x) {
            const DWORD value=benchmark?64+(x/2)*128/eye_width:64+(x/2)*2;
            reinterpret_cast<DWORD*>(static_cast<char*>(locked.pBits)+y*locked.Pitch)[x]=0xff000000|value*0x010101;
        }
        gpu_check(material->UnlockRect(0),"Unlock material ramp");
        gpu_check(device.SetTexture(0,scene.get()),"Bind role-selected scene atlas");
        gpu_check(device.SetTexture(1,material.get()),"Bind unselected material");
        tag_atlas_texture(*scene.get(),OutlastTargetRole::scene_color,eye_width,height);
        if (atlas_sampler_mask(device)!=1)
            throw std::runtime_error("Atlas role classification depended on dimensions rather than native ownership");
        for (UINT sampler=0;sampler<2;++sampler) {
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_MINFILTER,D3DTEXF_POINT),"Set point sampling");
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_MAGFILTER,D3DTEXF_POINT),"Set point sampling");
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable sampled mips");
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP),"Clamp source U");
            gpu_check(device.SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP),"Clamp source V");
        }
        D3D9ShaderVariants variants(device);
        gpu_check(device.SetTexture(15,scene.get()),"Seed unused bound atlas sampler");
        if (atlas_sampler_mask(device)!=(1u|(1u<<15)) ||
            atlas_sampler_mask(device,3)!=1 || variants.bound_atlas_samplers()!=1)
            throw std::runtime_error("Declared sampler inventory included an unused atlas binding");
        Com<ID3DBlob> cube_code;
        compile("SamplerState Cube:register(s0); float4 main(float3 ray:TEXCOORD0):COLOR {return texCUBE(Cube,ray);}",
            "ps_3_0",cube_code);
        Com<IDirect3DPixelShader9> cube;
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(cube_code->GetBufferPointer()),cube.put()),
            "Create declared cube sampler control");
        gpu_check(device.SetPixelShader(cube.get()),"Bind incompatible atlas/cube control");
        bool cube_rejected=false;
        try {variants.bound_atlas_samplers();}
        catch (const std::invalid_argument&) {cube_rejected=true;}
        if (!cube_rejected) throw std::runtime_error("Declared non-2D sampler silently accepted a bound native atlas");
        gpu_check(device.SetPixelShader(pixel.get()),"Restore original declared sampler inventory");
        sm3::Words reference_vertex(vertex_code->GetBufferSize()/4),reference_pixel(pixel_code->GetBufferSize()/4);
        std::memcpy(reference_vertex.data(),vertex_code->GetBufferPointer(),vertex_code->GetBufferSize());
        std::memcpy(reference_pixel.data(),pixel_code->GetBufferPointer(),pixel_code->GetBufferSize());
        const auto patched=sm3::patch_pair(reference_vertex,reference_pixel,
            {fault_atlas?0u:fault_material?3u:1u,!fault_camera,false,!fault_scissor,true});
        if (patched.view_texture)
            throw std::runtime_error("Screen-space projection was replaced with tracked world projection");
        const std::vector<DWORD> inspected_code(patched.pixel.begin(),patched.pixel.end());
        Com<IDirect3DPixelShader9> inspected;
        const auto inspection=device.CreatePixelShader(inspected_code.data(),inspected.put());
        if (FAILED(inspection)) {
            Com<ID3DBlob> assembly;
            gpu_check(D3DDisassemble(patched.pixel.data(),patched.pixel.size()*4,0,nullptr,assembly.put()),
                "Disassemble refused eye-aware pixel program");
            std::cerr.write(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
            gpu_check(inspection,"Driver refused eye-aware pixel program");
        }
        auto& world_pipeline=variants.current(fault_atlas?0:fault_material?3:1,!fault_camera,true,!fault_scissor,true);
        auto& pipeline=variants.current(fault_atlas?0:fault_material?3:1,!fault_camera,false,!fault_scissor,true);
        if (&world_pipeline==&pipeline || variants.size()!=2)
            throw std::runtime_error("Screen and world projection contexts shared a shader cache variant");
        for (const auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            gpu_check(device.SetSamplerState(0,filter,D3DTEXF_LINEAR),"Select native linear-filter variant");
        auto& linear_pipeline=variants.current(fault_atlas?0:fault_material?3:1,!fault_camera,false,!fault_scissor,true);
        for (const auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            gpu_check(device.SetSamplerState(0,filter,D3DTEXF_POINT),"Restore native point-filter variant");
        if (!fault_atlas && (&linear_pipeline==&pipeline ||
            &variants.current(fault_material?3:1,!fault_camera,false,!fault_scissor,true)!=&pipeline))
            throw std::runtime_error("Native point/linear filtering shared or lost its shader cache variant");
        if (mode.empty()) {
            auto** table=*reinterpret_cast<void***>(&device);
            original_pixel_read=reinterpret_cast<PixelRead>(table[108]);
            IsolatedVtableEntry reads;
            reads.install(&table[108],reinterpret_cast<void*>(&count_pixel_read));
            pixel_reads=0;
            Com<IDirect3DPixelShader9> selected;
            const auto mask=variants.bound_atlas_samplers(&selected);
            auto& reused=variants.current(mask,true,false,true,true,false,false,selected.get());
            if (pixel_reads!=1 || &reused!=&pipeline || selected.get()!=pixel.get())
                throw std::runtime_error("Draw selection repeated a captured pixel-shader query or changed its variant");
            auto& queried=variants.current(mask,true,false,true,true);
            if (pixel_reads!=2 || &queried!=&reused)
                throw std::runtime_error("Uncaptured draw selection skipped its required bound-shader query");
        }
        for (UINT slot=2;slot<15;++slot) {
            gpu_check(device.SetTexture(slot,material.get()),"Seed unused caller lookup-slot texture");
            for (const auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
                gpu_check(device.SetSamplerState(slot,filter,D3DTEXF_LINEAR),"Seed caller lookup-slot filtering");
            gpu_check(device.SetSamplerState(slot,D3DSAMP_SRGBTEXTURE,TRUE),"Seed caller lookup-slot gamma state");
            gpu_check(device.SetSamplerState(slot,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP),"Seed caller lookup-slot addressing");
        }
        StereoViewConstants views{};
        views[0][16]=8;views[1][16]=9;views[0][20]=views[1][20]=7;
        const float original_camera[]{9,0,0,1};
        gpu_check(device.SetPixelShaderConstantF(6,original_camera,1),"Seed original pixel camera");
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        Com<IDirect3DSurface9> backbuffer,readback;
        Com<IDirect3DQuery9> occlusion;
        Com<IDirect3DQuery9> timer_start,timer_end,timer_frequency,timer_disjoint;
        if (benchmark) {
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMP,timer_start.put()),"Create overdraw GPU start");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMP,timer_end.put()),"Create overdraw GPU end");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,timer_frequency.put()),"Create overdraw frequency");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,timer_disjoint.put()),"Create overdraw disjoint query");
        }
        gpu_check(device.CreateQuery(D3DQUERYTYPE_OCCLUSION,occlusion.put()),"Create stereo sample-count oracle");
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),"Get atlas routing backbuffer");
        gpu_check(device.CreateOffscreenPlainSurface(eye_width*2,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create routing readback");
#ifdef OLVR_ISOLATED_9ON12
        IsolatedD3D12DrawCounter counter(isolated.translation_device());
#endif
        UINT checked=0;
        for (UINT frame=0;frame<frames;++frame) {
            const UINT sample_frame=frame%6;
            const D3DVIEWPORT9 full_viewport{0,0,eye_width*2,height,0,1};
            const D3DVIEWPORT9 caller_viewport=frame<6?full_viewport:D3DVIEWPORT9{8,2,96,12,0,1};
            const float parameters[]{1.f/eye_width,1.f/height,float(sample_frame%2),float(int(sample_frame/2)-1)};
            const RECT scissor=frame==11?RECT{10,3,10,3}:
                frame<6?RECT{0,0,LONG(eye_width),LONG(height)}:RECT{10,3,55,13};
            const float visible_eyes[]{frame==10?-1.f:1.f,frame==9?-1.f:1.f,0,0};
            const bool hit_active=hit_effects && sample_frame>=1 && sample_frame<=4;
            const float hit[]{hit_active?.35f:0,hit_active?.125f:0,
                hit_active?-.1875f:0,hit_active?.15f*sample_frame:0};
            const float neutral_hit[4]{},hit_color[]{1,.125f,.0625f,1};
            gpu_check(device.SetScissorRect(&scissor),"Set half-open logical caller scissor");
            gpu_check(device.SetPixelShaderConstantF(0,parameters,1),"Bind UV/VPOS and border control");
            gpu_check(device.SetPixelShaderConstantF(1,visible_eyes,1),"Bind independent one-eye visibility controls");
            gpu_check(device.SetPixelShaderConstantF(2,fault_hit?neutral_hit:hit,1),"Bind transient hit-style distortion and flash");
            gpu_check(device.SetPixelShaderConstantF(3,hit_color,1),"Bind transient hit-style tint");
#ifdef OLVR_ISOLATED_9ON12
            counter.reset();
#endif
            gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE),"Disable caller scissor for complete fixture clear");
            gpu_check(device.SetViewport(&full_viewport),"Clear complete fixture viewport");
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear routing target");
            gpu_check(device.SetViewport(&caller_viewport),"Set offset native-style caller viewport");
            gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE),"Restore original caller scissor mode");
            const TestD3D9StateSnapshot caller_before(device);
            gpu_check(device.BeginScene(),"Begin routing frame");
            gpu_check(occlusion->Issue(D3DISSUE_BEGIN),"Begin native query around the paired stereo draw");
            if (benchmark) {
                gpu_check(timer_disjoint->Issue(D3DISSUE_BEGIN),"Begin overdraw timestamp interval");
                gpu_check(timer_start->Issue(D3DISSUE_END),"Start overdraw GPU workload");
            }
            const auto begin=std::chrono::steady_clock::now();
            for (UINT repeat=0;repeat<repeats;++repeat) {
                const auto result=pipeline.draw_primitive_up(D3DPT_TRIANGLELIST,2,quad,sizeof(quad),sizeof(quad[0]),views);
                if (!result.succeeded()) throw std::runtime_error("Single-pass pixel routing or caller restoration failed");
            }
            const auto cpu_us=std::chrono::duration<double,std::micro>(
                std::chrono::steady_clock::now()-begin).count()/repeats;
            if (benchmark) {
                gpu_check(timer_end->Issue(D3DISSUE_END),"End overdraw GPU workload");
                gpu_check(timer_frequency->Issue(D3DISSUE_END),"Issue overdraw GPU frequency");
                gpu_check(timer_disjoint->Issue(D3DISSUE_END),"End overdraw timestamp interval");
            }
            gpu_check(occlusion->Issue(D3DISSUE_END),"End native query around the paired stereo draw");
            gpu_check(device.EndScene(),"End routing frame");
            if (benchmark) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                auto wait=[&](IDirect3DQuery9& query,void* value,UINT bytes) {
                    for (;;) {
                        const auto result=query.GetData(value,bytes,D3DGETDATA_FLUSH);
                        gpu_check(result,"Read isolated overdraw GPU interval");
                        if (result==S_OK) return;
                        if (std::chrono::steady_clock::now()>=deadline)
                            throw std::runtime_error("Overdraw GPU timer timed out");
                        Sleep(1);
                    }
                };
                UINT64 start=0,end=0,frequency=0;BOOL disjoint=FALSE;
                wait(*timer_end.get(),&end,sizeof(end));wait(*timer_start.get(),&start,sizeof(start));
                wait(*timer_frequency.get(),&frequency,sizeof(frequency));
                wait(*timer_disjoint.get(),&disjoint,sizeof(disjoint));
                if (disjoint || !frequency || end<start)
                    throw std::runtime_error("Invalid overdraw GPU timing interval");
                std::cout<<"BENCHMARK frame="<<frame<<"; warmup="<<(frame==0?1:0)
                    <<"; eye_width="<<eye_width<<"; eye_height="<<height<<"; draws="<<repeats
                    <<"; cpu_us_per_draw="<<cpu_us<<"; gpu_us_per_draw="
                    <<double(end-start)*1000000/double(frequency)/repeats<<'\n';
            }
            if (fault_lookup_state)
                gpu_check(device.SetSamplerState(2,D3DSAMP_SRGBTEXTURE,FALSE),"Inject missing lookup-state restoration");
            const TestD3D9StateSnapshot caller_after(device);
            if (!(caller_before==caller_after))
                throw std::runtime_error("Point-atlas caller state changed: "+caller_before.difference(caller_after));
            gpu_check(device.GetRenderTargetData(backbuffer.get(),readback.get()),"Read routed pixels");
            float restored_camera[4]{};
            gpu_check(device.GetPixelShaderConstantF(6,restored_camera,1),"Read restored original camera");
            if (std::memcmp(restored_camera,original_camera,sizeof(original_camera)))
                throw std::runtime_error("Pixel routing changed original caller camera");
            float restored_projection[16]{};
            gpu_check(device.GetVertexShaderConstantF(0,restored_projection,4),"Read restored screen projection");
            if (std::memcmp(restored_projection,screen_projection,sizeof(screen_projection)))
                throw std::runtime_error("Pixel routing changed the original screen projection");
            RECT restored_scissor{};
            DWORD restored_enabled=0;
            gpu_check(device.GetScissorRect(&restored_scissor),"Read restored caller scissor");
            gpu_check(device.GetRenderState(D3DRS_SCISSORTESTENABLE,&restored_enabled),"Read restored caller scissor mode");
            if (std::memcmp(&scissor,&restored_scissor,sizeof(scissor)) || !restored_enabled)
                throw std::runtime_error("Pixel routing changed the caller scissor state");
            D3DVIEWPORT9 restored_viewport{};
            gpu_check(device.GetViewport(&restored_viewport),"Read restored caller viewport");
            if (std::memcmp(&restored_viewport,&caller_viewport,sizeof(caller_viewport)))
                throw std::runtime_error("Pixel routing changed the caller viewport");
            gpu_check(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Lock routed pixels");
            bool correct=true;
            DWORD expected_samples=0;
            for (UINT y=0;y<height;++y) for (UINT x=0;x<eye_width*2;++x) {
                const auto color=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(locked.pBits)+y*locked.Pitch)[x];
                const int eye=x/eye_width;
                const UINT logical_x=x%eye_width;
                const UINT viewport_x=caller_viewport.X/2,viewport_width=caller_viewport.Width/2;
                const bool in_viewport=logical_x>=viewport_x && logical_x<viewport_x+viewport_width &&
                    y>=caller_viewport.Y && y<caller_viewport.Y+caller_viewport.Height;
                const UINT uv_x=in_viewport?((logical_x-viewport_x)*eye_width*2+viewport_width)/(viewport_width*2):0;
                const UINT uv_y=in_viewport?((y-caller_viewport.Y)*height*2+caller_viewport.Height)/(caller_viewport.Height*2):0;
                const int gray=benchmark?64+uv_x*128/eye_width:64+uv_x*2;
                UINT source_x=sample_frame<2?0:sample_frame>=4?eye_width-1:
                    sample_frame%2?logical_x:uv_x;
                UINT source_y=sample_frame%2?y:uv_y;
                if (hit_active && in_viewport) {
                    const double u=sample_frame%2?double(logical_x)/eye_width:
                        double(logical_x-viewport_x)/viewport_width+.5/eye_width;
                    const double v=sample_frame%2?double(y)/height:
                        double(y-caller_viewport.Y)/caller_viewport.Height+.5/height;
                    const double radial=((u-.5)*(u-.5)+(v-.5)*(v-.5))*hit[0];
                    const double warped_u=u+(u-.5)*radial+hit[1]+parameters[3];
                    const double warped_v=v+(v-.5)*radial+hit[2];
                    source_x=UINT(std::clamp(std::floor(warped_u*eye_width),0.,double(eye_width-1)));
                    source_y=UINT(std::clamp(std::floor(warped_v*height),0.,double(height-1)));
                }
                const auto source_value=benchmark?96+source_x*126/eye_width+source_y*30/height:
                    96+source_x*2+source_y*2;
                const int intensity=(source_value*gray+127)/255;
                const bool clipped=int(x%eye_width)<scissor.left || int(x%eye_width)>=scissor.right ||
                    int(y)<scissor.top || int(y)>=scissor.bottom || !in_viewport ||
                    (frame==9 && eye==1) || (frame==10 && eye==0);
                if (!clipped) ++expected_samples;
                const int red=(color>>16)&255,green=(color>>8)&255,blue=color&255;
                const auto expected=[&](int original,unsigned channel) {
                    return clipped?0:int(std::lround(original*(1-hit[3])+255*hit_color[channel]*hit[3]));
                };
                const bool matches=std::abs(red-expected(eye?0:intensity,0))<=2 &&
                    std::abs(green-expected(!eye?0:intensity,1))<=2 &&
                    std::abs(blue-expected(eye?128:64,2))<=((hit_effects || benchmark)?2:1);
                if (!matches && correct)
                    std::cerr<<"Mismatch frame="<<frame<<" x="<<x<<" y="<<y
                        <<" rgb="<<red<<','<<green<<','<<blue<<" expected="
                        <<expected(eye?0:intensity,0)<<','<<expected(!eye?0:intensity,1)<<','
                        <<expected(eye?128:64,2)<<'\n';
                correct &= matches;
                ++checked;
            }
            gpu_check(readback->UnlockRect(),"Unlock routed pixels");
            if (!correct) throw std::runtime_error(hit_effects?
                "Hit-effect stereo UV, tint or recovery content mismatch":
                "Cross-eye atlas, material UV or tracked pixel camera content mismatch");
            DWORD samples=UINT_MAX;
            const auto query_result=occlusion->GetData(&samples,sizeof(samples),D3DGETDATA_FLUSH);
            if (query_result!=S_OK || samples!=expected_samples*repeats)
                throw std::runtime_error("Native occlusion did not count the exact two-eye pixel union: status="+
                    std::to_string(query_result)+" actual="+std::to_string(samples)+
                    " expected="+std::to_string(expected_samples*repeats));
#ifdef OLVR_ISOLATED_9ON12
            // LockRect waits for translated GPU work; GetRenderTargetData alone can return before emission.
            if (counter.indexed_calls!=repeats || counter.plain_calls || counter.instances!=repeats*2)
                throw std::runtime_error("Correct stereo pixels but inconclusive physical draw instrumentation: indexed="+
                    std::to_string(counter.indexed_calls.load())+" plain="+
                    std::to_string(counter.plain_calls.load())+" instances="+
                    std::to_string(counter.instances.load()));
#endif
        }
        if (!mode.empty() && mode!="--hit-effects" && !benchmark)
            throw std::runtime_error("Pixel routing incorrectly accepted a negative control");
        Com<IDirect3DBaseTexture9> unused;
        gpu_check(device.GetTexture(15,unused.put()),"Check unused atlas sampler restoration");
        if (unused.get()!=scene.get()) throw std::runtime_error("Declared-sampler optimization changed an unused binding");
        std::cout<<"PASS: eye-local UV and VPOS, atlas border isolation, unchanged same-size material UV, tracked per-eye pixel camera, half-open stereo scissor and disjoint offset native viewports; frames="<<frames<<"; pixels="
                 <<checked<<(benchmark?"; one draw/two instances per submission":"; one draw/two instances per frame")
                 <<"; caller restored; original screen projection preserved"
                 <<"; declared_sampler_scan; unused_atlas_untouched; cube_conflict_rejected; exact_stereo_occlusion_samples"
                 <<"; point_lookup_complete_caller_state; native_point_linear_variant_separation"
                 <<"; left_only_query; right_only_query; occluded_both_query";
        if (hit_effects) std::cout<<"; hit_style_radial_distortion_XY_offsets_and_flash"
            <<(benchmark?"; one_transient_cycle":"; two_transient_cycles")<<"; neutral_recovery";
#ifdef OLVR_ISOLATED_9ON12
        std::cout<<"; native_d3d12_draws="<<frames*repeats<<"; native_d3d12_instances="<<frames*repeats*2;
#endif
        if (benchmark) std::cout<<"; synthetic_fullscreen_alpha_overdraw; not_retail_water_or_headset_FPS";
        std::cout<<'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
