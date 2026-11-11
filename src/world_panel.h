#pragma once
#include "headlocked_ui.h"
#include "d3d9_shader_variants.h"
#include "d3d9_state_restore.h"
#include <d3dcompiler.h>
#include <cstring>
#include <optional>

namespace olvr {
struct WorldPanelHit {
    Vec3 point{};
    float u=0,v=0,distance=0;
};
class WorldPanel {
    Matrix4 anchor_{};
    bool placed_=false;
public:
    void reset() noexcept {placed_=false;}
    bool placed() const noexcept {return placed_;}
    std::optional<WorldPanelHit> intersect(EyePose aim) const {
        if (!placed_) return std::nullopt;
        const auto checked=checked_aim(aim);
        const auto inverse=matrix_inverse(anchor_);
        auto transform=[&](Vec3 v,float w) {
            return Vec3{inverse[0]*v.x+inverse[1]*v.y+inverse[2]*v.z+inverse[3]*w,
                inverse[4]*v.x+inverse[5]*v.y+inverse[6]*v.z+inverse[7]*w,
                inverse[8]*v.x+inverse[9]*v.y+inverse[10]*v.z+inverse[11]*w};
        };
        const auto direction=rotate(checked.orientation,{0,0,-1});
        const auto p=transform(checked.position,1),d=transform(direction,0);
        if (d.z>=-1e-6f) return std::nullopt;
        const float distance=-p.z/d.z;
        const auto hit=add(p,scale(d,distance));
        if (distance<=0 || std::abs(hit.x)>1 || std::abs(hit.y)>1) return std::nullopt;
        return WorldPanelHit{add(checked.position,scale(direction,distance)),
            (hit.x+1)*.5f,(1-hit.y)*.5f,distance};
    }
    StereoViewConstants locate(const std::array<EyePose,2>& eyes,
        const std::array<EyeFov,2>& fovs,float aspect) {
        if (!std::isfinite(aspect) || aspect<=0)
            throw std::invalid_argument("Invalid world panel aspect");
        const TrackingFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1},1,{0,0,0}};
        StereoViewConstants result{};
        for (unsigned eye=0;eye<2;++eye)
            result[eye]=eye_constants(frame,eyes[eye],fovs[eye],.01f,1000);
        if (!placed_) {
            auto upright=eyes;
            const auto center=scale(add(eyes[0].position,eyes[1].position),.5f);
            const auto q=binocular_orientation(eyes);
            const auto forward=rotate(q,{0,0,-1}),right=rotate(q,{1,0,0});
            const float yaw=forward.x*forward.x+forward.z*forward.z>1e-6f?
                std::atan2(-forward.x,-forward.z):std::atan2(-right.z,right.x);
            const Quaternion heading{0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
            const auto correction=multiply(heading,{-q.x,-q.y,-q.z,q.w});
            for (unsigned eye=0;eye<2;++eye)
                upright[eye]={add(center,rotate(correction,add(eyes[eye].position,scale(center,-1)))),
                    multiply(correction,eyes[eye].orientation)};
            const auto initial=headlocked_ui_constants(upright,fovs,aspect);
            const auto neutral=eye_constants(frame,upright[0],fovs[0],.01f,1000);
            Matrix4 projection{},panel{};
            std::copy_n(neutral.begin(),16,projection.begin());
            std::copy_n(initial[0].begin(),16,panel.begin());
            anchor_=matrix_product(matrix_inverse(projection),panel);
            const auto normal=rotate(heading,{0,0,1});
            anchor_[2]=normal.x;anchor_[6]=normal.y;anchor_[10]=normal.z;
            placed_=true;
        }
        for (auto& eye:result) {
            Matrix4 projection{};
            std::copy_n(eye.begin(),16,projection.begin());
            const auto transform=matrix_product(projection,anchor_);
            std::copy(transform.begin(),transform.end(),eye.begin());
        }
        return result;
    }
    StereoViewConstants locate_ui(const std::array<EyePose,2>& eyes,
        const std::array<EyeFov,2>& fovs,float aspect) {
        auto result=locate(eyes,fovs,aspect);
        // Retail UI depth orders flat elements; it is not distance from the ray plane.
        for (auto& eye:result)
            for (unsigned row=0;row<4;++row) eye[row*4+2]=0;
        return result;
    }
private:
    static EyePose checked_aim(EyePose aim) {
        const auto q=aim.orientation;
        if (!finite(aim.position) || !std::isfinite(q.x+q.y+q.z+q.w) ||
            std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1)>.0001f)
            throw std::invalid_argument("Invalid tracked menu aim");
        return aim;
    }
};

class D3D9WorldPanel {
    IDirect3DDevice9& device_;
    D3D9ShaderVariants variants_;
    D3D9StateCache caller_state_;
    Com<IDirect3DVertexShader9> vertex_;
    Com<IDirect3DPixelShader9> pixel_;
    Com<IDirect3DVertexDeclaration9> declaration_;
public:
    std::uint64_t state_allocations() const noexcept {return caller_state_.allocations;}
    std::uint64_t state_captures() const noexcept {return caller_state_.captures;}
    explicit D3D9WorldPanel(IDirect3DDevice9& device):device_(device),variants_(device) {
        const char* vertex=R"(
            struct Output {float4 position:POSITION;float2 uv:TEXCOORD0;};
            Output main(float3 position:POSITION) {
                Output output;
                output.position=float4(position,1);
                output.uv=float2(position.x*.5+.5,.5-position.y*.5);
                return output;
            })";
        const char* pixel=R"(
            sampler Source:register(s0);
            sampler NativeHud:register(s1);
            float4 SourceScale:register(c0);
            float4 Camera:register(c1);
            float4 PointerColor:register(c2);
            float4 main(float2 uv:TEXCOORD0):COLOR {
                if (SourceScale.w < -1.5) return PointerColor;
                if (SourceScale.w<0) return float4(.075,.085,.095,1);
                float4 image=tex2D(Source,uv*SourceScale.xy);
                if (Camera.x>0.5) {
                    float luminance=dot(image.rgb,float3(.2126,.7152,.0722));
                    float green=pow(saturate(luminance*1.6),.65);
                    float vignette=saturate(1-.7*dot(uv-.5,uv-.5));
                    image=float4(float3(.12,1,.22)*green*vignette,1);
                }
                if (Camera.w>0.5) {
                    float4 hud=tex2D(NativeHud,uv);
                    return float4(hud.rgb+image.rgb*(1-hud.a),1);
                }
                if (Camera.y>0.5) {
                    float2 battery=abs(uv-float2(.88,.075));
                    bool outline=all(battery<float2(.075,.028)) && any(battery>float2(.069,.022));
                    bool terminal=uv.x>.955 && uv.x<.964 && uv.y>.063 && uv.y<.087;
                    bool charge=uv.x>.814 && uv.x<.814+.132*Camera.z && uv.y>.06 && uv.y<.09;
                    if (outline || terminal || charge) image=float4(1,1,1,1);
                    float2 recording=(uv-float2(.05,.075))*float2(1,1.77777778);
                    if (dot(recording,recording)<.00012) image=float4(1,.08,.04,1);
                }
                return image;
            })";
        auto compile=[](const char* source,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto status=D3DCompile(source,std::strlen(source),"world-menu-panel",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(status)) throw std::runtime_error(errors.get()?
                std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                "World panel shader compilation failed");
        };
        Com<ID3DBlob> vs,ps;
        compile(vertex,"vs_3_0",vs);compile(pixel,"ps_3_0",ps);
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),vertex_.put()),
            "Create world panel vertex shader");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),pixel_.put()),
            "Create world panel pixel shader");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        gpu_check(device.CreateVertexDeclaration(elements,declaration_.put()),"Create world panel declaration");
    }
    void draw(IDirect3DTexture9& source,IDirect3DSurface9& output,UINT width,UINT height,
        const StereoViewConstants& eyes) {
        compose(&source,output,nullptr,width,height,eyes,false,false,false);
    }
    void draw_pointer(IDirect3DSurface9& output,UINT width,UINT height,
        const StereoViewConstants& eyes,Vec3 origin,Vec3 end,bool hit) {
        const auto delta=add(end,scale(origin,-1));
        const float length=std::sqrt(dot(delta,delta));
        if (!finite(origin) || !finite(end) || length<.001f)
            throw std::invalid_argument("Invalid controller menu ray");
        const auto direction=scale(delta,1/length);
        auto a=cross(direction,std::abs(direction.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0});
        a=scale(a,.0015f/std::sqrt(dot(a,a)));
        const auto b=cross(direction,a);
        std::array<Vec3,12> vertices{};
        for (unsigned i=0;i<2;++i) {
            const auto side=i?b:a;
            const Vec3 quad[]{add(origin,side),add(origin,scale(side,-1)),
                add(end,side),add(end,side),add(origin,scale(side,-1)),add(end,scale(side,-1))};
            std::copy_n(quad,6,vertices.begin()+i*6);
        }
        compose(nullptr,output,nullptr,width,height,eyes,false,false,false,-1,nullptr,
            vertices.data(),UINT(vertices.size()),hit);
    }
    void draw_screen(IDirect3DTexture9& lens_image,IDirect3DSurface9& output,
        IDirect3DSurface9& world_depth,UINT width,UINT height,const StereoViewConstants& eyes,
        bool night_vision=false,float battery=-1,IDirect3DTexture9* native_hud=nullptr) {
        if (!std::isfinite(battery) || battery < -1 || battery>1)
            throw std::invalid_argument("Invalid physical camcorder battery state");
        validate_depth(output,world_depth,width,height);
        compose(&lens_image,output,&world_depth,width,height,eyes,true,false,night_vision,battery,native_hud);
    }
    void draw_housing(IDirect3DSurface9& output,IDirect3DSurface9& world_depth,
        UINT width,UINT height,const StereoViewConstants& eyes) {
        validate_depth(output,world_depth,width,height);
        compose(nullptr,output,&world_depth,width,height,eyes,true,true,false);
    }
private:
    static void validate_depth(IDirect3DSurface9& output,IDirect3DSurface9& world_depth,
        UINT width,UINT height) {
        D3DSURFACE_DESC target{},depth{};
        gpu_check(output.GetDesc(&target),"Inspect physical screen output");
        gpu_check(world_depth.GetDesc(&depth),"Inspect physical screen world depth");
        if (!width || !height || width>std::numeric_limits<UINT>::max()/2 ||
            target.Width!=width*2 || target.Height!=height || depth.Width!=target.Width ||
            depth.Height!=target.Height || depth.MultiSampleType!=target.MultiSampleType ||
            depth.MultiSampleQuality!=target.MultiSampleQuality)
            throw std::invalid_argument("Physical screen requires matching eye-atlas world depth");
    }
    void compose(IDirect3DTexture9* source,IDirect3DSurface9& output,IDirect3DSurface9* world_depth,
        UINT width,UINT height,const StereoViewConstants& eyes,bool screen,bool housing,bool night_vision,
        float battery=-1,IDirect3DTexture9* native_hud=nullptr,
        const Vec3* pointer=nullptr,UINT pointer_count=0,bool pointer_hit=false) {
        StateRestore state(device_,caller_state_);
        D3DCAPS9 caps{};
        gpu_check(device_.GetDeviceCaps(&caps),"Inspect panel target slots");
        std::array<Com<IDirect3DSurface9>,4> targets;
        Com<IDirect3DSurface9> depth;
        const auto count=std::min<UINT>(caps.NumSimultaneousRTs,targets.size());
        for (UINT slot=0;slot<count;++slot) {
            const auto status=device_.GetRenderTarget(slot,targets[slot].put());
            if (status!=D3DERR_NOTFOUND) gpu_check(status,"Capture panel caller render targets");
        }
        const auto depth_status=device_.GetDepthStencilSurface(depth.put());
        if (depth_status!=D3DERR_NOTFOUND) gpu_check(depth_status,"Capture panel caller depth");
        const auto restore=[&] {
            gpu_check(device_.SetRenderTarget(0,targets[0].get()),"Restore panel caller primary target");
            for (UINT slot=1;slot<count;++slot)
                gpu_check(device_.SetRenderTarget(slot,targets[slot].get()),"Restore panel caller secondary target");
            gpu_check(device_.SetDepthStencilSurface(depth.get()),"Restore panel caller depth");
            gpu_check(state.apply(),"Restore panel caller graphics state");
        };
        try {
            gpu_check(device_.SetDepthStencilSurface(nullptr),"Detach caller depth for spatial panel");
            for (UINT slot=1;slot<count;++slot)
                gpu_check(device_.SetRenderTarget(slot,nullptr),"Detach secondary panel targets");
            gpu_check(device_.SetRenderTarget(0,&output),"Bind spatial panel output");
            gpu_check(device_.SetDepthStencilSurface(world_depth),"Bind spatial panel world depth");
            const D3DVIEWPORT9 viewport{0,0,width*2,height,0,1};
            gpu_check(device_.SetViewport(&viewport),"Set exact panel XR viewport");
            for (auto setting:std::initializer_list<std::pair<D3DRENDERSTATETYPE,DWORD>>{
                {D3DRS_ZENABLE,screen?D3DZB_TRUE:D3DZB_FALSE},{D3DRS_ZWRITEENABLE,FALSE},
                {D3DRS_ZFUNC,D3DCMP_LESSEQUAL},{D3DRS_STENCILENABLE,FALSE},
                {D3DRS_ALPHABLENDENABLE,FALSE},{D3DRS_ALPHATESTENABLE,FALSE},{D3DRS_FOGENABLE,FALSE},
                {D3DRS_SCISSORTESTENABLE,FALSE},{D3DRS_CLIPPLANEENABLE,0},{D3DRS_WRAP0,0},
                {D3DRS_CULLMODE,D3DCULL_NONE},{D3DRS_FILLMODE,D3DFILL_SOLID},
                {D3DRS_COLORWRITEENABLE,15},{D3DRS_SRGBWRITEENABLE,FALSE}})
                gpu_check(device_.SetRenderState(setting.first,setting.second),"Set isolated panel raster state");
            if (!screen && !pointer)
                gpu_check(device_.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear spatial panel surroundings");
            gpu_check(device_.SetVertexShader(vertex_.get()),"Bind spatial panel vertex");
            gpu_check(device_.SetPixelShader(pixel_.get()),"Bind spatial panel pixel");
            gpu_check(device_.SetVertexDeclaration(declaration_.get()),"Bind spatial panel geometry");
            gpu_check(device_.SetTexture(0,source),"Bind mono menu image");
            gpu_check(device_.SetTexture(1,native_hud),"Bind actual native camera movie image");
            const float source_scale[]{screen?1.f:.5f,1,0,pointer?-2.f:housing?-1.f:0.f};
            gpu_check(device_.SetPixelShaderConstantF(0,source_scale,1),"Set explicit mono/atlas panel source layout");
            const float camera[]{night_vision?1.f:0.f,battery>=0?1.f:0.f,battery,native_hud?1.f:0.f};
            gpu_check(device_.SetPixelShaderConstantF(1,camera,1),"Set feed-only night vision");
            const float color[]{pointer_hit?.1f:.55f,pointer_hit?1.f:.7f,1,1};
            gpu_check(device_.SetPixelShaderConstantF(2,color,1),"Set controller menu ray color");
            for (auto setting:std::initializer_list<std::pair<D3DSAMPLERSTATETYPE,DWORD>>{
                {D3DSAMP_MINFILTER,D3DTEXF_LINEAR},{D3DSAMP_MAGFILTER,D3DTEXF_LINEAR},
                {D3DSAMP_MIPFILTER,D3DTEXF_NONE},{D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP},
                {D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP},{D3DSAMP_SRGBTEXTURE,FALSE}})
                for (UINT sampler=0;sampler<(native_hud?2u:1u);++sampler)
                    gpu_check(device_.SetSamplerState(sampler,setting.first,setting.second),"Set spatial panel image sampling");
            auto& pipeline=variants_.current(0,false,false,false,false,false,true);
            const float quad[][3]{{-1,-1,0},{1,-1,0},{1,1,0},{-1,-1,0},{1,1,0},{-1,1,0}};
            std::vector<std::array<float,3>> body;
            if (housing) {
                auto box=[&](float x,float y,float z,float hx,float hy,float hz) {
                    const std::array<std::array<float,3>,8> p{{
                        {x-hx,y-hy,z-hz},{x+hx,y-hy,z-hz},{x+hx,y+hy,z-hz},{x-hx,y+hy,z-hz},
                        {x-hx,y-hy,z+hz},{x+hx,y-hy,z+hz},{x+hx,y+hy,z+hz},{x-hx,y+hy,z+hz}}};
                    constexpr unsigned indices[]{0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,
                        3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
                    for (unsigned index:indices) body.push_back(p[index]);
                };
                box(0,0,-.025f,.035f,.025f,.055f);
                box(-.075f,.015f,.010f,.050f,.0303125f,.004f);
                box(0,.015f,-.085f,.027f,.022f,.014f);
            }
            gpu_check(device_.BeginScene(),"Begin spatial panel composition");
            SubmissionResult result{};
            try {
                result=pipeline.draw_primitive_up(D3DPT_TRIANGLELIST,
                    pointer?pointer_count/3:housing?UINT(body.size()/3):2,
                    pointer?static_cast<const void*>(pointer):housing?body.data():static_cast<const void*>(quad),
                    pointer?pointer_count*sizeof(Vec3):housing?body.size()*sizeof(body[0]):sizeof(quad),
                    sizeof(quad[0]),eyes);
            } catch (...) {
                gpu_check(device_.EndScene(),"End failed spatial panel composition");
                throw;
            }
            const auto end=device_.EndScene();
            if (!result.succeeded()) throw std::runtime_error("Spatial panel instanced draw/restoration failed");
            gpu_check(end,"End spatial panel composition");
        } catch (...) {
            restore();
            throw;
        }
        restore();
    }
};
}
