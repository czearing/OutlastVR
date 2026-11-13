#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include "stereo_reconstruction.h"
#include "test_d3d9_state_snapshot.h"
#ifdef OLVR_ISOLATED_9ON12
#include "isolated_d3d12_counter.h"
#endif
#include <chrono>
#include <iostream>

using namespace olvr;

#ifdef OLVR_ISOLATED_9ON12
class IndexUploadCounter {
    using Lock=HRESULT (WINAPI*)(IDirect3DIndexBuffer9*,UINT,UINT,void**,DWORD);
    Com<IDirect3DIndexBuffer9> example_;
    IsolatedVtableEntry entry_;
    Lock original_=nullptr;
    static inline IndexUploadCounter* active_=nullptr;
    static HRESULT WINAPI lock(IDirect3DIndexBuffer9* buffer,UINT offset,UINT size,void** data,DWORD flags) {
        ++active_->uploads;
        return active_->original_(buffer,offset,size,data,flags);
    }
public:
    UINT uploads=0;
    explicit IndexUploadCounter(IDirect3DDevice9& device) {
        if (active_) throw std::logic_error("Overlapping isolated index upload counters");
        gpu_check(device.CreateIndexBuffer(12,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,D3DFMT_INDEX32,
            D3DPOOL_DEFAULT,example_.put(),nullptr),"Create isolated index upload counter");
        void** table=nullptr;
        std::memcpy(&table,example_.get(),sizeof(table));
        original_=reinterpret_cast<Lock>(table[11]);
        entry_.install(&table[11],reinterpret_cast<void*>(&lock));
        active_=this;
    }
    ~IndexUploadCounter() {
        entry_.detach();
        active_=nullptr;
    }
};
#endif

int main(int argc,char** argv) {
    try {
        const bool overwrite=argc==2 && std::string(argv[1])=="--burst-fault-overwrite";
        const bool shadow_fault=argc==2 && std::string(argv[1])=="--fault-shadow-projection";
        const bool shadow=shadow_fault || (argc==2 && std::string(argv[1])=="--shadow-projection");
        const bool upload_cache=argc==2 && std::string(argv[1])=="--upload-cache";
        const bool cache_invalidation=argc==2 && std::string(argv[1])=="--cache-invalidation";
        const bool declaration_cache=argc==2 && std::string(argv[1])=="--declaration-cache";
        const bool gpu_benchmark=argc==2 && std::string(argv[1])=="--gpu-benchmark";
        const bool benchmark=gpu_benchmark || (argc==2 && std::string(argv[1])=="--cpu-benchmark");
        const bool pixel_constants=argc==2 && std::string(argv[1])=="--pixel-constants";
        const bool register_pressure=argc==2 && std::string(argv[1])=="--pixel-register-pressure";
        const bool index_cache_fault=argc==2 && std::string(argv[1])=="--fault-index-cache-stale";
        const bool index_cache=index_cache_fault || (argc==2 && std::string(argv[1])=="--index-cache");
        const unsigned frames=index_cache?12:6;
        const bool caller_fault=argc==2 && std::string(argv[1])=="--fault-caller-state";
        const bool caller_state=caller_fault || (argc==2 && std::string(argv[1])=="--caller-state");
        const unsigned benchmark_draws=gpu_benchmark?64:3000;
        std::vector<double> submission_us(frames);
        std::vector<double> gpu_us(frames);
        const bool material_fault=argc==2 && std::string(argv[1])=="--fault-material-untracked";
        const bool camera_only_fault=argc==2 && std::string(argv[1])=="--fault-material-camera-only";
        const bool camera_projection=argc==2 && std::string(argv[1])=="--material-camera-only-projection";
        const bool camera_only=camera_only_fault || camera_projection ||
            (argc==2 && std::string(argv[1])=="--material-camera-only");
        const bool world_columns=argc==2 && std::string(argv[1])=="--material-world-position-native";
        const bool world_origin=argc==2 && std::string(argv[1])=="--material-world-position-large-origin";
        const bool alternate_world=camera_projection ||
            (argc==2 && std::string(argv[1])=="--material-alternate-varying");
        const bool full_samplers=argc==2 && std::string(argv[1])=="--material-full-samplers";
        const bool material_columns=world_columns || (argc==2 && std::string(argv[1])=="--material-native-columns");
        const bool material_origin=world_origin || (argc==2 && std::string(argv[1])=="--material-large-origin");
        const bool material_world=world_columns || world_origin || alternate_world || full_samplers ||
            (argc==2 && std::string(argv[1])=="--material-world-position");
        const bool material=material_fault || material_columns || material_origin || camera_only || material_world ||
            (argc==2 && std::string(argv[1])=="--material-projection");
        const bool motion_fault=argc==2 && std::string(argv[1])=="--fault-motion-policy";
        const bool motion=motion_fault || (argc==2 && std::string(argv[1])=="--motion-policy");
        const bool fault=shadow_fault || material_fault || camera_only_fault || motion_fault ||
            (argc==2 && std::string(argv[1])=="--fault-untracked");
        const bool burst=overwrite || (argc==2 && std::string(argv[1])=="--burst");
        const bool native_fault=argc==2 && std::string(argv[1])=="--fault-native-columns";
        const bool native_translated=argc==2 && std::string(argv[1])=="--native-translated";
        const bool native_columns=material_columns || material_origin || shadow || native_fault || native_translated || (argc==2 && std::string(argv[1])=="--native-columns");
        const bool context_change=argc==2 && std::string(argv[1])=="--context-change";
        const bool billboard_fault=argc==2 && std::string(argv[1])=="--fault-billboard-camera";
        const bool billboard_lock_fault=argc==2 && std::string(argv[1])=="--fault-billboard-locked";
        const bool billboard_locked=billboard_lock_fault || (argc==2 && std::string(argv[1])=="--billboard-locked");
        const bool billboard_policy=argc==2 && std::string(argv[1])=="--billboard-policy-cache";
        const bool billboard=billboard_fault || billboard_locked || billboard_policy ||
            (argc==2 && std::string(argv[1])=="--billboard-camera");
        if (argc!=1 && !fault && !burst && !native_columns && !context_change && !upload_cache &&
            !cache_invalidation && !declaration_cache && !benchmark && !index_cache && !pixel_constants && !register_pressure &&
            !material && !motion && !caller_state && !billboard)
            throw std::invalid_argument("Unknown tracked-uniform control");
        const UINT width=gpu_benchmark?512:64,height=gpu_benchmark?256:16;
        const UINT target_height=burst?height*6:height;
        IsolatedDevice isolated(width*2,target_height);
        auto& device=*isolated.device.get();
        if (caller_state) {
            D3DCAPS9 caps{};gpu_check(device.GetDeviceCaps(&caps),"Read caller-state register limits");
            std::vector<float> vertex(caps.MaxVertexShaderConst*4,.125f);
            std::array<float,224*4> pixel{};pixel.fill(.25f);
            std::array<BOOL,16> booleans{};booleans.fill(TRUE);
            std::array<int,64> integers{};integers.fill(17);
            gpu_check(device.SetVertexShaderConstantF(0,vertex.data(),caps.MaxVertexShaderConst),"Seed every caller vertex float register");
            gpu_check(device.SetPixelShaderConstantF(0,pixel.data(),224),"Seed every caller pixel float register");
            gpu_check(device.SetVertexShaderConstantI(0,integers.data(),16),"Seed every caller vertex integer register");
            gpu_check(device.SetPixelShaderConstantI(0,integers.data(),16),"Seed every caller pixel integer register");
            gpu_check(device.SetVertexShaderConstantB(0,booleans.data(),16),"Seed every caller vertex boolean register");
            gpu_check(device.SetPixelShaderConstantB(0,booleans.data(),16),"Seed every caller pixel boolean register");
        }
        const char* vertex_source=R"(
            MATRIX_STORAGE float4x4 ScreenToWorldOffset:register(c8);
            float4 CameraPositionVS:register(c4);
            float4 CameraWorldPosition:register(c17);
            float4 CameraRight:register(c18);
            float4 CameraUp:register(c19);
            float3 ParticleUpRightResultScalars:register(c20);
            sampler SourceData:register(s0);
            struct Output {float4 position:POSITION;float3 ray:TEXCOORD0;float4 materialWorld:MATERIAL_WORLD_SEMANTIC;
                           float4 billboard:TEXCOORD6;
#if ALTERNATE_WORLD
                           float4 unrelated:TEXCOORD5;
#endif
            };
            Output main(float2 position:POSITION) {
                Output output;
                output.position=float4(position,tex2Dlod(SourceData,float4(.5,.5,0,0)).r,1);
                output.ray=MUL_NATIVE(ScreenToWorldOffset,float4(position,1,0)).xyz;
                output.materialWorld=float4(output.ray*MATERIAL_DEPTH+CameraPositionVS.xyz,1);
#if ALTERNATE_WORLD
                output.unrelated=float4(0,0,0,output.ray.z*.003);
#endif
                output.billboard=float4(CameraRight.xyz*.25+CameraUp.xyz*.5+CameraWorldPosition.xyz*.01,
                    (CameraRight.w+CameraUp.w+CameraWorldPosition.w)/128+ParticleUpRightResultScalars.x);
                return output;
            })";
        const char* pixel_source=R"(
            float4 Parameters:register(c0);
            float BandOffset:register(c1);
            MATRIX_STORAGE float4x4 ScreenToWorldMatrix:register(c8);
            float4 FogCameraPosition:register(c12);
            MATRIX_STORAGE float3x3 ViewToWorldMatrix:register(c16);
            MATRIX_STORAGE float4x4 ScreenToShadowMatrix:register(c20);
            MATRIX_STORAGE float4x4 ScreenToLight:register(c24);
            float4 NativePretranslation:register(c2);
            MATRIX_STORAGE float4x4 ViewProjectionMatrix:register(c28);
            float4 CameraWorldPos:register(c32);
            float4 MotionBlurMaskScaleAndBias:register(c36);
            float4 DynamicVelocityParameters:register(c37);
            sampler MaterialTextures[16]:register(s0);
            float4 main(float3 ray:TEXCOORD0,float4 materialWorld:MATERIAL_WORLD_SEMANTIC,
#if ALTERNATE_WORLD
                float4 unrelated:TEXCOORD5,
#endif
                float4 billboard:TEXCOORD6,float2 pixel:VPOS):COLOR {
#if BILLBOARD_TEST
                return float4(billboard.xyz*.2+.5,billboard.w);
#endif
#if MOTION_TEST
                return float4((MotionBlurMaskScaleAndBias.xyz+DynamicVelocityParameters.xyz)*.01+.5,.5);
#endif
#if MATERIAL_CAMERA_ONLY
#if MATERIAL_CAMERA_PROJECTION
                float4 cameraProjected=MUL_NATIVE(ViewProjectionMatrix,materialWorld);
                return float4(CameraWorldPos.x*.0025+.5,cameraProjected.x/cameraProjected.w*.2+.5,
                    CameraWorldPos.z*.0025+.5,CameraWorldPos.w/128);
#endif
                return float4(CameraWorldPos.xyz*.0025+.5,CameraWorldPos.w/128);
#endif
#if MATERIAL_TEST
                float4 projected=MUL_NATIVE(ViewProjectionMatrix,materialWorld);
                float3 absolute=materialWorld.xyz+CameraWorldPos.xyz;
#if MATERIAL_WORLD_TEST
                float extra=0;
#if ALTERNATE_WORLD
                extra=unrelated.w*.000001;
#endif
#if FULL_SAMPLERS
                [unroll] for (int samplerIndex=0;samplerIndex<16;++samplerIndex)
                    extra+=tex2D(MaterialTextures[samplerIndex],pixel*.01).r*.000001;
#endif
                return float4(projected.x/projected.w*.2+.5,absolute.z*.005+.5,
                    materialWorld.x*.005+.5,CameraWorldPos.w/128+extra);
#endif
                return float4(projected.xy/projected.w*.2+.5,absolute.z*.005+.5,CameraWorldPos.w/128);
#endif
                float2 clip=float2(pixel.x*Parameters.x*2-1,1-(pixel.y-BandOffset)*Parameters.y*2);
                float3 world=MUL_NATIVE(ScreenToWorldMatrix,float4(clip*Parameters.z,Parameters.z,1)).xyz;
#if TRANSLATED_NATIVE
                world-=NativePretranslation.xyz;
#endif
                float3 fromVertex=ray*Parameters.z+FogCameraPosition.xyz;
                float3 rotation=MUL_NATIVE(ViewToWorldMatrix,float3(clip,-1));
#if SHADOW_TEST
                float4 shadow=MUL_NATIVE(ScreenToShadowMatrix,float4(clip*Parameters.z,Parameters.z,1));
                float4 light=MUL_NATIVE(ScreenToLight,float4(clip*Parameters.z,Parameters.z,1));
                return float4(shadow.xyz/shadow.w*.0025+light.xyz/light.w*.0025+.5, .5);
#endif
                return float4((world+fromVertex)*.0025+.5+rotation*.02,FogCameraPosition.w/128);
            })";
        auto compile=[](const char* source,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto status=D3DCompile(source,std::strlen(source),"tracked-native-uniforms",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(status)) throw std::runtime_error(errors.get()?
                std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                "Tracked uniform fixture compile failed");
        };
        Com<ID3DBlob> vertex_code,pixel_code;
        const std::string prefix=std::string("#define MUL_NATIVE(m,v) mul(m,v)\n#define MATRIX_STORAGE ")+
            (native_columns?"column_major\n":"row_major\n")+
            (alternate_world?"#define MATERIAL_WORLD_SEMANTIC TEXCOORD7\n#define ALTERNATE_WORLD 1\n":
                "#define MATERIAL_WORLD_SEMANTIC TEXCOORD5\n#define ALTERNATE_WORLD 0\n")+
            (full_samplers?"#define FULL_SAMPLERS 1\n":"#define FULL_SAMPLERS 0\n")+
            (camera_projection?"#define MATERIAL_CAMERA_PROJECTION 1\n":"#define MATERIAL_CAMERA_PROJECTION 0\n")+
            (material_origin?"#define ScreenToWorldOffset ScreenToTranslatedWorld\n":"")+
            (material_origin?"#define MATERIAL_DEPTH .25\n":"#define MATERIAL_DEPTH 25\n")+
            (native_translated?"#define TRANSLATED_NATIVE 1\n":"#define TRANSLATED_NATIVE 0\n")+
            (material?"#define MATERIAL_TEST 1\n":"#define MATERIAL_TEST 0\n")+
            (material_world?"#define MATERIAL_WORLD_TEST 1\n":"#define MATERIAL_WORLD_TEST 0\n")+
            (camera_only?"#define MATERIAL_CAMERA_ONLY 1\n":"#define MATERIAL_CAMERA_ONLY 0\n")+
            (motion?"#define MOTION_TEST 1\n":"#define MOTION_TEST 0\n")+
            (billboard?"#define BILLBOARD_TEST 1\n":"#define BILLBOARD_TEST 0\n")+
            (shadow?"#define SHADOW_TEST 1\n":"#define SHADOW_TEST 0\n");
        compile((prefix+vertex_source).c_str(),"vs_3_0",vertex_code);
        compile((prefix+pixel_source).c_str(),"ps_3_0",pixel_code);
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vertex_code->GetBufferPointer()),vertex.put()),"Create tracked fixture vertex");
        const auto* original_pixel=static_cast<const std::uint32_t*>(pixel_code->GetBufferPointer());
        sm3::Words selected_pixel(original_pixel,original_pixel+pixel_code->GetBufferSize()/4);
        if (register_pressure) {
            const auto instructions=sm3::parse(selected_pixel);
            const auto free=sm3::inspect(instructions).free(2,224);
            auto [headers,body]=sm3::split(instructions);
            for (std::size_t index=0;index+8<free.size();++index)
                sm3::op(headers,81,{sm3::destination(2,free[index]),0,0,0,0});
            selected_pixel={0xffff0300u};
            sm3::append(selected_pixel,sm3::canonical(headers));sm3::append(selected_pixel,body);
            selected_pixel.push_back(65535);
        }
        const std::vector<DWORD> selected_code(selected_pixel.begin(),selected_pixel.end());
        gpu_check(device.CreatePixelShader(selected_code.data(),pixel.put()),"Create tracked fixture pixel");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind original tracked vertex");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind original tracked pixel");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create reconstruction declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind reconstruction declaration");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable reconstruction culling");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable reconstruction depth");
        Com<IDirect3DTexture9> source_data;
        gpu_check(device.CreateTexture(1,1,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,source_data.put(),nullptr),"Create original VTF sentinel");
        D3DLOCKED_RECT lock{};
        gpu_check(source_data->LockRect(0,&lock,nullptr,0),"Lock original VTF sentinel");
        std::memset(lock.pBits,0,16);
        gpu_check(source_data->UnlockRect(0),"Unlock original VTF sentinel");
        gpu_check(device.SetTexture(D3DVERTEXTEXTURESAMPLER0,source_data.get()),"Bind original VTF sentinel");
        if (full_samplers)
            for (unsigned sampler=0;sampler<16;++sampler) {
                gpu_check(device.SetTexture(sampler,source_data.get()),"Bind all sixteen native material samplers");
                gpu_check(device.SetSamplerState(sampler,D3DSAMP_MINFILTER,D3DTEXF_POINT),"Set native material minification");
                gpu_check(device.SetSamplerState(sampler,D3DSAMP_MAGFILTER,D3DTEXF_POINT),"Set native material magnification");
                gpu_check(device.SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable native material mips");
            }
        for (const auto state:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            gpu_check(device.SetSamplerState(D3DVERTEXTEXTURESAMPLER0,state,D3DTEXF_POINT),"Point sample original VTF");
        gpu_check(device.SetSamplerState(D3DVERTEXTEXTURESAMPLER0,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable original VTF mips");
        TrackingFrame frame{{30,-5,7},{0,1,0},{0,0,1},{-1,0,0},100,{-20,2,-3}};
        if (material_origin) {
            frame.origin={16777216,-16777216,16777216};
            frame.pretranslation={-16777216,16777216,-16777216};
        }
        const EyeFov mono{-.7f,.8f,.65f,-.6f};
        const auto original=eye_constants(frame,{{0,0,0},{0,0,0,1}},mono,.5f,2000);
        StereoReconstructionContext context{};
        std::copy_n(original.begin(),16,context.original_projection.begin());
        context.original_camera=frame;
        const float billboard_camera[]{frame.origin.x,frame.origin.y,frame.origin.z,16};
        const float billboard_right[]{-.25f,.5f,-.75f,16};
        const float billboard_up[]{.125f,-.875f,.25f,32};
        if (billboard) {
            gpu_check(device.SetVertexShaderConstantF(17,billboard_camera,1),"Bind native particle camera with independent W");
            gpu_check(device.SetVertexShaderConstantF(18,billboard_right,1),"Bind authored noncanonical billboard right");
            gpu_check(device.SetVertexShaderConstantF(19,billboard_up,1),"Bind authored noncanonical billboard up");
        }
        const float motion_mask[]{80,45,5,0},motion_velocity[]{.0125f,.0222222f,.1f,.05625f};
        if (motion) {
            gpu_check(device.SetPixelShaderConstantF(36,motion_mask,1),"Bind active native motion mask");
            gpu_check(device.SetPixelShaderConstantF(37,motion_velocity,1),"Bind active native motion velocity");
        }
        const float camera_offset[]{-frame.pretranslation.x,-frame.pretranslation.y,-frame.pretranslation.z,64};
        Matrix4 material_projection{};
        if (material) {
            const auto translated=add(frame.origin,frame.pretranslation);
            float camera_translated[]{translated.x,translated.y,translated.z,1};
            material_projection=matrix_product(context.original_projection,translation_matrix(translated));
            if (native_columns)
                for (unsigned row=0;row<4;++row) for (unsigned column=row+1;column<4;++column)
                    std::swap(material_projection[row*4+column],material_projection[column*4+row]);
            gpu_check(device.SetVertexShaderConstantF(4,camera_translated,1),"Bind material translated camera");
            gpu_check(device.SetPixelShaderConstantF(28,material_projection.data(),4),"Bind native material projection");
            gpu_check(device.SetPixelShaderConstantF(32,camera_offset,1),"Bind original material world offset");
        }
        auto screen=identity_matrix(),basis=identity_matrix();
        const auto component=[](Vec3 value,unsigned index) {return index==0?value.x:index==1?value.y:value.z;};
        const Vec3 depth_axis=add(scale(frame.backward,-1),add(
            scale(frame.right,(std::tan(mono.right)+std::tan(mono.left))*.5f),
            scale(frame.up,(std::tan(mono.up)+std::tan(mono.down))*.5f)));
        const Vec3 columns[]{
            scale(frame.right,(std::tan(mono.right)-std::tan(mono.left))*.5f),
            scale(frame.up,(std::tan(mono.up)-std::tan(mono.down))*.5f),depth_axis,frame.origin};
        const Vec3 axes[]{frame.right,frame.up,frame.backward};
        for (unsigned row=0;row<3;++row) {
            for (unsigned column=0;column<4;++column) screen[row*4+column]=component(columns[column],row);
            for (unsigned column=0;column<3;++column) basis[row*4+column]=component(axes[column],row);
        }
        const Matrix4 light_space{.8f,.2f,-.1f,3, -.1f,.7f,.3f,-2,
            .2f,-.3f,.9f,1, .001f,-.002f,.001f,1};
        const Matrix4 shadow_space{.6f,-.3f,.2f,-4, .2f,.9f,-.1f,5,
            -.3f,.1f,.7f,2, -.002f,.001f,.002f,1};
        auto shadow_screen=matrix_product(shadow_space,screen);
        auto light_screen=matrix_product(light_space,screen);
        if (shadow) {
            for (auto* matrix:{&shadow_screen,&light_screen})
                for (unsigned row=0;row<4;++row) for (unsigned column=row+1;column<4;++column)
                    std::swap((*matrix)[row*4+column],(*matrix)[column*4+row]);
            gpu_check(device.SetPixelShaderConstantF(20,shadow_screen.data(),4),"Bind original native screen-to-shadow");
            gpu_check(device.SetPixelShaderConstantF(24,light_screen.data(),4),"Bind original native screen-to-light");
        }
        if (native_translated || material_origin) {
            screen[3]+=frame.pretranslation.x;
            screen[7]+=frame.pretranslation.y;
            screen[11]+=frame.pretranslation.z;
            const float translation[]{frame.pretranslation.x,frame.pretranslation.y,frame.pretranslation.z,0};
            gpu_check(device.SetPixelShaderConstantF(2,translation,1),"Bind native translated-world reconstruction contract");
        }
        if (native_columns) {
            for (unsigned row=0;row<4;++row) for (unsigned column=row+1;column<4;++column)
                std::swap(screen[row*4+column],screen[column*4+row]);
            for (unsigned row=0;row<3;++row) for (unsigned column=row+1;column<3;++column)
                std::swap(basis[row*4+column],basis[column*4+row]);
        }
        gpu_check(device.SetVertexShaderConstantF(8,screen.data(),4),"Bind original screen ray matrix");
        gpu_check(device.SetPixelShaderConstantF(8,screen.data(),4),"Bind original world reconstruction");
        gpu_check(device.SetPixelShaderConstantF(16,basis.data(),3),"Bind original view-to-world rotation");
        float camera[]{frame.origin.x,frame.origin.y,frame.origin.z,64};
        gpu_check(device.SetPixelShaderConstantF(12,camera,1),"Bind original fog camera and distance");
        const float parameters[]{1.f/width,1.f/height,25,0};
        gpu_check(device.SetPixelShaderConstantF(0,parameters,1),"Bind reconstruction fixture dimensions");
        D3D9ShaderVariants variants(device);
        auto& pipeline=variants.current(0,false,false,false,false,!fault && !billboard_fault);
        if (pixel_constants && (pipeline.diagnostic_binding().pixel_constant_rows.empty() ||
            pipeline.diagnostic_binding().pixel_view_sampler>=0))
            throw std::runtime_error("Available pixel constants still use per-pixel camera texture reads");
        if (register_pressure && (!pipeline.diagnostic_binding().pixel_constant_rows.empty() ||
            pipeline.diagnostic_binding().pixel_view_sampler<0))
            throw std::runtime_error("Constant-register pressure did not retain the texture fallback");
        pipeline.set_reconstruction_context(context);
        pipeline.set_camera_directions_view_dependent(!billboard_locked || billboard_lock_fault);
        pipeline.set_native_view_columns(native_columns && !native_fault);
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        std::array<std::array<float,2>,18> repeated_quad{};
        for (unsigned index=0;index<repeated_quad.size();++index)
            std::copy_n(quad[index%6],2,repeated_quad[index].begin());
        Com<IDirect3DVertexBuffer9> indexed_vertices;
        Com<IDirect3DVertexBuffer9> fvf_vertices;
        Com<IDirect3DIndexBuffer9> indexed_indices;
        Com<IDirect3DPixelShader9> alternate_caller_pixel;
        Com<IDirect3DVertexDeclaration9> alternate_declaration,conflicting_declaration;
        if (declaration_cache || caller_state || index_cache) {
            const auto patched=sm3::patch_pair(
                sm3::Words(static_cast<const std::uint32_t*>(vertex_code->GetBufferPointer()),
                    static_cast<const std::uint32_t*>(vertex_code->GetBufferPointer())+vertex_code->GetBufferSize()/4),
                sm3::Words(static_cast<const std::uint32_t*>(pixel_code->GetBufferPointer()),
                    static_cast<const std::uint32_t*>(pixel_code->GetBufferPointer())+pixel_code->GetBufferSize()/4),
                {0,false,false,false,false,true,false});
            const D3DVERTEXELEMENT9 alternate[]{
                elements[0],{1,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,0},D3DDECL_END()};
            const D3DVERTEXELEMENT9 conflicting[]{
                elements[0],{0,0,D3DDECLTYPE_FLOAT1,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,
                    static_cast<BYTE>(patched.input_semantic)},D3DDECL_END()};
            gpu_check(device.CreateVertexDeclaration(alternate,alternate_declaration.put()),"Create spare-stream change fixture");
            gpu_check(device.CreateVertexDeclaration(conflicting,conflicting_declaration.put()),"Create eye-semantic conflict fixture");
            gpu_check(device.CreateVertexBuffer(sizeof(quad),D3DUSAGE_WRITEONLY,0,D3DPOOL_MANAGED,
                indexed_vertices.put(),nullptr),"Create declaration-cache geometry");
            const WORD indices[]{0,1,2,3,4,5};
            gpu_check(device.CreateIndexBuffer(sizeof(indices),D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,
                D3DPOOL_MANAGED,indexed_indices.put(),nullptr),"Create declaration-cache indices");
            void* pointer=nullptr;
            gpu_check(indexed_vertices->Lock(0,sizeof(quad),&pointer,0),"Lock declaration-cache geometry");
            std::memcpy(pointer,quad,sizeof(quad));
            gpu_check(indexed_vertices->Unlock(),"Unlock declaration-cache geometry");
            gpu_check(indexed_indices->Lock(0,sizeof(indices),&pointer,0),"Lock declaration-cache indices");
            std::memcpy(pointer,indices,sizeof(indices));
            gpu_check(indexed_indices->Unlock(),"Unlock declaration-cache indices");
        }
        if (caller_state) {
            float positions[6][4]{};
            for (unsigned i=0;i<6;++i) {positions[i][0]=quad[i][0];positions[i][1]=quad[i][1];positions[i][3]=1;}
            gpu_check(device.CreateVertexBuffer(sizeof(positions),D3DUSAGE_WRITEONLY,0,D3DPOOL_MANAGED,
                fvf_vertices.put(),nullptr),"Create native FVF geometry");
            void* pointer=nullptr;
            gpu_check(fvf_vertices->Lock(0,sizeof(positions),&pointer,0),"Lock native FVF geometry");
            std::memcpy(pointer,positions,sizeof(positions));
            gpu_check(fvf_vertices->Unlock(),"Unlock native FVF geometry");
            const auto* begin=static_cast<const std::uint32_t*>(pixel_code->GetBufferPointer());
            sm3::Words changed(begin,begin+pixel_code->GetBufferSize()/4);
            const std::uint32_t literal[]{0x05000051,0xa00f00c9,0x3f800000,0x40000000,0x40400000,0x40800000};
            changed.insert(changed.begin()+1,std::begin(literal),std::end(literal));
            gpu_check(device.CreatePixelShader(reinterpret_cast<const DWORD*>(changed.data()),alternate_caller_pixel.put()),
                "Create changed native literal-register caller");
        }
        Com<IDirect3DSurface9> backbuffer,readback;
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),"Get reconstruction backbuffer");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,target_height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create reconstruction readback");
#ifdef OLVR_ISOLATED_9ON12
        IsolatedD3D12DrawCounter counter(isolated.translation_device());
        counter.reset();
        std::unique_ptr<IndexUploadCounter> index_uploads;
        if (index_cache || benchmark) index_uploads=std::make_unique<IndexUploadCounter>(device);
#endif
        UINT pixels=0;
        if (burst) {
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear reconstruction burst");
            gpu_check(device.BeginScene(),"Begin unfenced reconstruction burst");
        }
        for (unsigned serial=0;serial<frames;++serial) {
            if (index_cache && serial==9) pipeline.release_device_resources();
            if (billboard_policy) pipeline.set_camera_directions_view_dependent(serial%2==0);
            if (cache_invalidation) {
                if (serial==2) {
                    context.original_camera.units_per_meter+=1;
                    pipeline.set_reconstruction_context(context);
                }
                pipeline.set_native_view_columns(serial==3);
                if (serial==5) pipeline.release_device_resources();
            }
            if (upload_cache) {
                if (serial==4) pipeline.release_device_resources();
                camera[3]=serial>=3?32.f:64.f;
                gpu_check(device.SetPixelShaderConstantF(12,camera,1),"Exercise changed source uniform with unchanged eye records");
            }
            if (context_change) {
                frame.origin.x+=.5f;
                frame.pretranslation.x-=.5f;
                const auto projection=eye_constants(frame,{{0,0,0},{0,0,0,1}},mono,.5f,2000);
                std::copy_n(projection.begin(),16,context.original_projection.begin());
                context.original_camera=frame;
                pipeline.set_reconstruction_context(context);
                screen[3]=camera[0]=frame.origin.x;
                gpu_check(device.SetVertexShaderConstantF(8,screen.data(),4),"Update changed native vertex context");
                gpu_check(device.SetPixelShaderConstantF(8,screen.data(),4),"Update changed native pixel context");
                gpu_check(device.SetPixelShaderConstantF(12,camera,1),"Update changed native fog context");
            }
            const float yaw=material?(int(serial)-2)*1.57079632679f:
                (int(overwrite?5:cache_invalidation || benchmark || billboard_policy?0:upload_cache?serial/2:serial)-2)*.2f;
            std::array<EyePose,2> poses{};
            const std::array<EyeFov,2> fovs{
                EyeFov{-.9f,.65f,.75f,-.7f},EyeFov{-.6f,1.f,.8f,-.65f}};
            StereoViewConstants views{};
            for (unsigned eye=0;eye<2;++eye) {
                poses[eye]={{eye?.132f:.068f,-.1f,.15f},{0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)}};
                views[eye]=eye_constants(frame,poses[eye],fovs[eye],.5f,2000);
            }
#ifdef OLVR_ISOLATED_9ON12
            if (!burst) counter.reset();
#endif
            const float band_offset[]{burst?float(serial*height):0,0,0,0};
            gpu_check(device.SetPixelShaderConstantF(1,band_offset,1),"Set reconstruction band origin");
            const D3DVIEWPORT9 viewport{0,burst?serial*height:0,width*2,height,0,1};
            gpu_check(device.SetViewport(&viewport),"Locate reconstruction band");
            if (!burst) {
                gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear reconstruction fixture");
                gpu_check(device.BeginScene(),"Begin reconstruction frame");
            }
            if (declaration_cache || caller_state) {
                if (serial==2) {
                    gpu_check(device.SetVertexDeclaration(conflicting_declaration.get()),"Bind conflicting cached declaration");
                    bool rejected=false;
                    try {
                        pipeline.draw_indexed({D3DPT_TRIANGLELIST,0,0,6,0,2},views);
                    } catch (const std::invalid_argument& error) {
                        if (std::string(error.what())!="Original declaration conflicts with the paired eye input semantic") throw;
                        rejected=true;
                    }
                    if (!rejected) throw std::runtime_error("Declaration cache accepted a changed eye-semantic conflict");
                }
                const bool alternate=serial==2 || serial==3 || serial==5;
                gpu_check(device.SetVertexDeclaration(alternate?alternate_declaration.get():declaration.get()),
                    "Switch immutable native declaration");
                for (UINT stream=0;stream<(alternate?2u:1u);++stream)
                    gpu_check(device.SetStreamSource(stream,indexed_vertices.get(),0,sizeof(quad[0])),
                        "Bind declaration-cache geometry stream");
                gpu_check(device.SetIndices(indexed_indices.get()),"Bind declaration-cache indices");
                gpu_check(device.SetStreamSourceFreq(0,serial==3?D3DSTREAMSOURCE_INDEXEDDATA|1:1),
                    "Change native object count without changing declaration");
                gpu_check(device.SetStreamSourceFreq(1,serial==3?D3DSTREAMSOURCE_INSTANCEDATA|1:1),
                    "Change native stream divisor without changing declaration");
            }
            std::unique_ptr<TestD3D9StateSnapshot> caller_before;
            if (caller_state) {
                if (serial%2) {
                    gpu_check(device.SetFVF(D3DFVF_XYZW),"Bind actual native FVF declaration");
                    gpu_check(device.SetStreamSource(0,fvf_vertices.get(),0,4*sizeof(float)),"Bind native FVF stream");
                }
                gpu_check(device.SetPixelShader(serial>=2 && serial<4?alternate_caller_pixel.get():pixel.get()),
                    "Change native caller identity and literal-register mask");
                const float sentinel[]{float(serial),.125f,-.25f,1};
                gpu_check(device.SetVertexShaderConstantF(200,sentinel,1),"Change unused caller vertex constants");
                gpu_check(device.SetPixelShaderConstantF(200,sentinel,1),"Change unused caller pixel constants");
                gpu_check(device.SetSamplerState(15,D3DSAMP_ADDRESSU,serial%2?D3DTADDRESS_WRAP:D3DTADDRESS_MIRROR),
                    "Change untouched caller sampler");
                const RECT scissor{0,0,LONG(width*2),LONG(height)};
                gpu_check(device.SetScissorRect(&scissor),"Change caller scissor");
                caller_before=std::make_unique<TestD3D9StateSnapshot>(device);
            }
            if (pixel_constants || register_pressure) {
                if (serial==4) pipeline.release_device_resources();
                caller_before=std::make_unique<TestD3D9StateSnapshot>(device);
            }
            if (full_samplers) {
                if (pipeline.diagnostic_binding().pixel_constant_rows.empty() ||
                    pipeline.diagnostic_binding().pixel_view_sampler>=0)
                    throw std::runtime_error("Sampler-saturated material did not use paired constant rows");
                caller_before=std::make_unique<TestD3D9StateSnapshot>(device);
            }
            Com<IDirect3DQuery9> gpu_start,gpu_end,gpu_frequency,gpu_disjoint;
            if (gpu_benchmark) {
                gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMP,gpu_start.put()),"Create benchmark GPU start");
                gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMP,gpu_end.put()),"Create benchmark GPU end");
                gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,gpu_frequency.put()),"Create benchmark GPU frequency");
                gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,gpu_disjoint.put()),"Create benchmark GPU disjoint");
                gpu_check(gpu_disjoint->Issue(D3DISSUE_BEGIN),"Begin benchmark GPU timing");
                gpu_check(gpu_start->Issue(D3DISSUE_END),"Start benchmark GPU workload");
            }
            const auto begin=std::chrono::steady_clock::now();
            for (unsigned repeat=0;repeat<(benchmark?benchmark_draws:1u);++repeat) {
                SubmissionResult submission{};
                if (index_cache) {
                    if (serial==2 || serial==7 || (index_cache_fault && serial==3)) {
                        const std::array<DWORD,6> indices32{};
                        const std::array<WORD,6> indices16{};
                        submission=pipeline.draw_indexed_up({D3DPT_TRIANGLELIST,0,0,6,0,2},
                            serial==7?static_cast<const void*>(indices16.data()):indices32.data(),
                            serial==7?sizeof(indices16):sizeof(indices32),
                            serial==7?D3DFMT_INDEX16:D3DFMT_INDEX32,quad,sizeof(quad),sizeof(quad[0]),views);
                    } else if (serial==10) {
                        gpu_check(device.SetStreamSource(0,indexed_vertices.get(),0,sizeof(quad[0])),
                            "Bind native non-indexed cache fixture");
                        submission=pipeline.draw_non_indexed(D3DPT_TRIANGLELIST,0,2,views);
                    } else {
                        const unsigned vertices=serial==0 || serial==4?12:serial==6?18:6;
                        submission=pipeline.draw_primitive_up(D3DPT_TRIANGLELIST,vertices/3,
                            repeated_quad.data(),vertices*sizeof(quad[0]),sizeof(quad[0]),views);
                    }
                } else submission=declaration_cache || caller_state?
                    pipeline.draw_indexed({D3DPT_TRIANGLELIST,0,0,6,0,2},views):
                    pipeline.draw_primitive_up(D3DPT_TRIANGLELIST,2,quad,sizeof(quad),sizeof(quad[0]),views);
                if (!submission.succeeded())
                    throw std::runtime_error("Tracked native uniform dispatch/restoration failed");
            }
            submission_us[serial]=std::chrono::duration<double,std::micro>(
                std::chrono::steady_clock::now()-begin).count()/(benchmark?benchmark_draws:1u);
            if (caller_state || full_samplers || pixel_constants || register_pressure) {
                if (caller_fault) {
                    const float corrupt[]{123,456,789,1};
                    gpu_check(device.SetPixelShaderConstantF(12,corrupt,1),"Inject caller-state corruption");
                }
                const TestD3D9StateSnapshot after(device);
                if (!(*caller_before==after))
                    throw std::runtime_error("Precise caller-state snapshot detected state corruption: "+
                        caller_before->difference(after));
            }
            if (declaration_cache) {
                for (UINT stream=0;stream<2;++stream) {
                    UINT frequency=0;
                    gpu_check(device.GetStreamSourceFreq(stream,&frequency),"Read freshly restored native stream frequency");
                    const auto expected=serial==3?(stream?D3DSTREAMSOURCE_INSTANCEDATA|1:D3DSTREAMSOURCE_INDEXEDDATA|1):1;
                    if (frequency!=expected) throw std::runtime_error("Declaration reuse retained stale stream frequency");
                }
            }
            if (burst && serial<5) continue;
            if (gpu_benchmark) {
                gpu_check(gpu_end->Issue(D3DISSUE_END),"End benchmark GPU workload");
                gpu_check(gpu_frequency->Issue(D3DISSUE_END),"Issue benchmark GPU frequency");
                gpu_check(gpu_disjoint->Issue(D3DISSUE_END),"End benchmark GPU timing");
            }
            gpu_check(device.EndScene(),"End reconstruction frame");
            if (gpu_benchmark) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                const auto wait=[&](IDirect3DQuery9& query,void* value,UINT bytes) {
                    for (;;) {
                        const auto result=query.GetData(value,bytes,D3DGETDATA_FLUSH);
                        gpu_check(result,"Read isolated benchmark GPU timer");
                        if (result==S_OK) return;
                        if (std::chrono::steady_clock::now()>deadline)
                            throw std::runtime_error("Isolated GPU benchmark timed out");
                        Sleep(1);
                    }
                };
                UINT64 start=0,end=0,frequency=0;BOOL disjoint=FALSE;
                wait(*gpu_end.get(),&end,sizeof(end));wait(*gpu_start.get(),&start,sizeof(start));
                wait(*gpu_frequency.get(),&frequency,sizeof(frequency));
                wait(*gpu_disjoint.get(),&disjoint,sizeof(disjoint));
                if (disjoint || !frequency || end<start)
                    throw std::runtime_error("Invalid isolated GPU benchmark interval");
                gpu_us[serial]=double(end-start)*1000000/double(frequency)/benchmark_draws;
            }
            gpu_check(device.GetRenderTargetData(backbuffer.get(),readback.get()),"Read reconstructed eye images");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize reconstruction pixels");
            bool correct=true;
            for (UINT y=0;y<target_height;++y) for (UINT x=0;x<width*2;++x) {
                const auto eye=x/width;
                const float nx=2.f*(x%width)/width-1,ny=1-2.f*(y%height)/height;
                auto pose=poses[eye];
                if (burst) {
                    const float band_yaw=(int(y/height)-2)*.2f;
                    pose.orientation={0,std::sin(band_yaw*.5f),0,std::cos(band_yaw*.5f)};
                }
                const auto fov=fovs[eye];
                const float rx=((std::tan(fov.right)-std::tan(fov.left))*nx+
                    std::tan(fov.right)+std::tan(fov.left))*.5f;
                const float ry=((std::tan(fov.up)-std::tan(fov.down))*ny+
                    std::tan(fov.up)+std::tan(fov.down))*.5f;
                const auto direction=map_tracking(frame,rotate(pose.orientation,{rx,ry,-1}));
                const auto origin=add(frame.origin,scale(map_tracking(frame,pose.position),100));
                const auto world=add(origin,scale(direction,material_origin?.25f:25.f));
                const auto rotation=map_tracking(frame,rotate(pose.orientation,{nx,ny,-1}));
                const auto rotated_billboard=[&](const float* native) {
                    const Vec3 value{native[0],native[1],native[2]};
                    if (billboard_locked || (billboard_policy && serial%2)) return value;
                    const Vec3 local{dot(value,frame.right),dot(value,frame.up),dot(value,frame.backward)};
                    return map_tracking(frame,rotate(pose.orientation,local));
                };
                const auto billboard_value=add(add(scale(rotated_billboard(billboard_right),.25f),
                    scale(rotated_billboard(billboard_up),.5f)),scale(origin,.01f));
                const auto color=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                if (index_cache && (serial==2 || serial==7)) {
                    if (correct && color!=0xff000000)
                        std::cerr<<"Index overwrite mismatch frame="<<serial<<" x="<<x<<" y="<<y
                            <<" color="<<color<<'\n';
                    correct &= color==0xff000000;
                    ++pixels;
                    continue;
                }
                for (unsigned axis=0;axis<3;++axis) {
                    auto project=[&](const Matrix4& matrix,unsigned row) {
                        const float v[]{world.x,world.y,world.z,1};
                        float numerator=0,denominator=0;
                        for (unsigned column=0;column<4;++column) {
                            numerator+=matrix[row*4+column]*v[column];
                            denominator+=matrix[12+column]*v[column];
                        }
                        return numerator/denominator;
                    };
                    const float value=billboard?component(billboard_value,axis)*.2f+.5f:
                        motion?.5f:camera_only?(camera_projection && axis==1?nx*.2f+.5f:
                            component(origin,axis)*.0025f+.5f):
                        material_world?(axis==0?nx*.2f+.5f:axis==1?world.z*.005f+.5f:
                            (world.x+frame.pretranslation.x)*.005f+.5f):material?(axis==0?nx*.2f+.5f:
                        axis==1?ny*.2f+.5f:world.z*.005f+.5f):shadow?
                        .5f+.0025f*(project(shadow_space,axis)+project(light_space,axis)):
                        component(world,axis)*.005f+.5f+component(rotation,axis)*.02f;
                    const int expected=int(std::lround(std::clamp(value,0.f,1.f)*255));
                    const int actual=(color>>(16-axis*8))&255;
                    if (std::abs(actual-expected)>2) {
                        if (correct) std::cerr<<"Reconstruction mismatch frame="<<serial<<" x="<<x<<" y="<<y
                            <<" axis="<<axis<<" actual="<<actual<<" expected="<<expected<<'\n';
                        correct=false;
                    }
                }
                correct &= std::abs(int(color>>24)-(upload_cache && serial>=3?64:128))<=1;
                ++pixels;
            }
            gpu_check(readback->UnlockRect(),"Unlock reconstructed eye images");
            if (!correct) throw std::runtime_error("Tracked inverse matrices, fog camera or view rotation content mismatch; frame="+
                std::to_string(serial));
#ifdef OLVR_ISOLATED_9ON12
            const auto draws=benchmark?benchmark_draws:burst?6u:1u;
            if (counter.indexed_calls!=draws || counter.plain_calls || counter.instances!=draws*2)
                throw std::runtime_error("Reconstruction emitted more than one physical geometry draw");
#endif
            Com<IDirect3DBaseTexture9> restored_texture;
            gpu_check(device.GetTexture(D3DVERTEXTEXTURESAMPLER0,restored_texture.put()),"Read original VTF restoration");
            if (restored_texture.get()!=source_data.get()) throw std::runtime_error("Reconstruction replaced the original VTF sampler");
            float restored[16]{};
            gpu_check(device.GetPixelShaderConstantF(8,restored,4),"Read original reconstruction matrix restoration");
            if (std::memcmp(restored,screen.data(),sizeof(restored))) throw std::runtime_error("Reconstruction changed original native matrix");
            gpu_check(device.GetPixelShaderConstantF(12,restored,1),"Read original fog distance restoration");
            if (std::memcmp(restored,camera,sizeof(camera))) throw std::runtime_error("Reconstruction changed original fog parameters");
            if (material) {
                gpu_check(device.GetPixelShaderConstantF(28,restored,4),"Read original material projection restoration");
                if (std::memcmp(restored,material_projection.data(),sizeof(restored)))
                    throw std::runtime_error("Reconstruction changed original material projection");
                gpu_check(device.GetPixelShaderConstantF(32,restored,1),"Read original material camera restoration");
                if (std::memcmp(restored,camera_offset,sizeof(camera_offset)))
                    throw std::runtime_error("Reconstruction changed original material camera or its fourth component");
            }
            if (motion) {
                gpu_check(device.GetPixelShaderConstantF(36,restored,1),"Read original motion mask restoration");
                if (std::memcmp(restored,motion_mask,sizeof(motion_mask)))
                    throw std::runtime_error("Shader-local policy changed the native motion mask");
                gpu_check(device.GetPixelShaderConstantF(37,restored,1),"Read original motion velocity restoration");
                if (std::memcmp(restored,motion_velocity,sizeof(motion_velocity)))
                    throw std::runtime_error("Shader-local policy changed native motion velocity");
            }
            if (billboard) {
                for (unsigned index=0;index<3;++index) {
                    const float* expected=index==0?billboard_camera:index==1?billboard_right:billboard_up;
                    gpu_check(device.GetVertexShaderConstantF(17+index,restored,1),"Read native particle caller constants");
                    if (std::memcmp(restored,expected,4*sizeof(float)))
                        throw std::runtime_error("Stereo particle binding changed authored XYZ or W in caller constants");
                }
            }
        }
        if (upload_cache && pipeline.view_uploads()!=4)
            throw std::runtime_error("View upload cache missed identical records, changed uniforms or resource release; uploads="+
                std::to_string(pipeline.view_uploads()));
#ifndef OLVR_BASELINE_BENCHMARK
        if (upload_cache && (pipeline.view_record_builds()!=4 || pipeline.view_record_cache_hits()!=2 ||
            pipeline.declaration_builds()!=2))
            throw std::runtime_error("CPU view/declaration cache missed unchanged inputs, uniform changes or reset");
        if (cache_invalidation && (pipeline.view_record_builds()!=5 || pipeline.view_record_cache_hits()!=1 ||
            pipeline.declaration_builds()!=2 || pipeline.view_uploads()!=4))
            throw std::runtime_error("Unchanged-input cache failed context, column-convention or reset invalidation");
        if (declaration_cache && pipeline.declaration_builds()!=4)
            throw std::runtime_error("Declaration cache failed switching, spare-stream changes or conflict recovery");
        if (benchmark && (pipeline.view_record_builds()!=1 ||
            pipeline.view_record_cache_hits()!=6*benchmark_draws-1 || pipeline.declaration_builds()!=1))
            throw std::runtime_error("Hot-path benchmark did not exercise unchanged-input CPU caches");
#ifdef OLVR_ISOLATED_9ON12
        if (index_cache && index_uploads->uploads!=8)
            throw std::runtime_error("Generated-index reuse missed prefix, overwrite, growth, format or reset invalidation; uploads="+
                std::to_string(index_uploads->uploads));
        if (benchmark && index_uploads->uploads!=1)
            throw std::runtime_error("Unchanged generated indices were uploaded more than once");
#endif
#endif
        if (fault) throw std::runtime_error("Untracked inverse reconstruction incorrectly passed");
        if (caller_state) std::cout<<"PASS: precise_caller_state_checks=6; exact_caller_state_preserved; every_float_integer_boolean_shader_constant; all_20_samplers_and_streams; declaration_FVF_switches; changed_caller_literal_mask; viewport_scissor_render_state; ";
        std::cout<<"PASS: per-eye vertex/pixel inverse reconstruction, fog camera, view-to-world rotation, original VTF and constant restoration; frames="<<frames<<"; pixels="
            <<pixels<<(benchmark?"; one draw/two instances per submission":"; one draw/two instances per frame");
#ifdef OLVR_ISOLATED_9ON12
        std::cout<<"; native_d3d12_draws="<<(benchmark?6*benchmark_draws:frames)
            <<"; native_d3d12_instances="<<(benchmark?12*benchmark_draws:frames*2);
#endif
        if (burst) std::cout<<"; unfenced_draw_burst=6; gpu_readbacks=1";
        if (shadow) std::cout<<"; screen_to_shadow_and_light; native_columns; independent_world_projector_oracle";
        if (billboard) std::cout<<"; particle_camera_right_up_world_position; noncanonical_authored_axes_and_W; independent_quaternion_oracle";
        if (billboard_locked) std::cout<<"; authored_world_locked_axes_preserved";
        if (billboard_policy) std::cout<<"; axis_policy_switches=6; unchanged_eye_records; invalidated_camera_axis_cache";
        if (upload_cache) std::cout<<"; view_uploads=4; identical_records_reused; changed_uniform_and_reset_invalidated";
        if (upload_cache) std::cout<<"; CPU_view_builds=4; CPU_view_cache_hits=2; declaration_builds=2";
        if (cache_invalidation) std::cout<<"; identical_eye_and_uniform_inputs; context_columns_reset_invalidated; CPU_view_builds=5; CPU_view_cache_hits=1; declaration_builds=2; view_uploads=4";
        if (declaration_cache) std::cout<<"; declaration_builds=4; spare_stream_changed; fresh_stream_frequencies; conflicting_declaration_rejected";
        if (index_cache) std::cout<<"; cached_prefix_and_native_nonindexed; supplied_indices_growth_format_reset_invalidated";
#ifdef OLVR_ISOLATED_9ON12
        if (index_uploads) std::cout<<"; index_buffer_locks="<<index_uploads->uploads;
#endif
        if (material && !camera_only) std::cout<<"; material_camera_rebase; pixel_forward_projection; large_head_turns";
        if (material_origin) std::cout<<"; large_world_origin; translated_material_precision";
        if (camera_only) std::cout<<"; material_camera_only; no_world_position_varying; original_camera_preserved; large_head_turns";
        if (motion) std::cout<<"; native_motion_policy_zeroed_without_mutating_caller_constants";
        if (pixel_constants) std::cout<<"; paired_pixel_constants_no_texture_reads; full_caller_state_preserved; reset_recovered";
        if (register_pressure) std::cout<<"; pixel_register_pressure_texture_fallback; full_caller_state_preserved; reset_recovered";
        if (benchmark) {
            std::cout<<"; benchmark_submission_us=[";
            for (unsigned index=0;index<submission_us.size();++index)
                std::cout<<(index?",":"")<<submission_us[index];
            std::cout<<"]; includes_driver_submission_and_state_restoration; excludes_readbacks; not_headset_FPS";
        }
        if (gpu_benchmark) {
            std::cout<<"; benchmark_gpu_us=[";
            for (unsigned index=0;index<gpu_us.size();++index) std::cout<<(index?",":"")<<gpu_us[index];
            std::cout<<"]; timestamp_disjoint_checked; per_draw_pixels="<<width*2*height;
        }
        std::cout<<'\n';return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
