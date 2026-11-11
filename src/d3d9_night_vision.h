#pragma once
#include "d3d9_state_restore.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>

namespace olvr {

class D3D9NightVision {
    IDirect3DDevice9& device_;
    D3D9StateCache state_;
    Com<IDirect3DPixelShader9> shader_;
    Com<IDirect3DTexture9> source_;
    Com<IDirect3DSurface9> surface_;
    UINT width_=0,height_=0;
    D3DFORMAT format_=D3DFMT_UNKNOWN;
public:
    explicit D3D9NightVision(IDirect3DDevice9& device):device_(device) {
        const char* text=R"(
            sampler Source:register(s0);
            float4 Options:register(c0);
            float4 main(float2 uv:TEXCOORD0):COLOR {
                float4 image=tex2D(Source,uv);
                float luminance=dot(image.rgb,float3(.2126,.7152,.0722));
                float green=pow(saturate(luminance*1.6),.65);
                if (Options.x>.5) image.rgb=float3(.12,1,.22)*green;
                float2 eye=float2(frac(uv.x*2),uv.y)*2-1;
                image.rgb*=1-Options.y*smoothstep(.55,1,length(eye));
                return image;
            })";
        Com<ID3DBlob> code,errors;
        const auto status=D3DCompile(text,std::strlen(text),"night-vision-filter",nullptr,nullptr,
            "main","ps_3_0",D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
        if (FAILED(status)) throw std::runtime_error(errors.get()?
            std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
            "Cannot compile night-vision filter");
        gpu_check(device_.CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),shader_.put()),
            "Create night-vision filter");
    }
    void draw(IDirect3DSurface9& output,bool night_vision=true,float vignette=0) {
        if (!std::isfinite(vignette) || vignette<0 || vignette>1)
            throw std::invalid_argument("Vignette strength must be 0..1");
        D3DSURFACE_DESC description{};
        gpu_check(output.GetDesc(&description),"Read night-vision target");
        if (!description.Width || description.Width%2 || !description.Height ||
            description.MultiSampleType!=D3DMULTISAMPLE_NONE)
            throw std::invalid_argument("Night-vision requires a non-MSAA two-eye atlas");
        if (width_!=description.Width || height_!=description.Height || format_!=description.Format) {
            surface_.reset();source_.reset();
            gpu_check(device_.CreateTexture(description.Width,description.Height,1,D3DUSAGE_RENDERTARGET,
                description.Format,D3DPOOL_DEFAULT,source_.put(),nullptr),"Create reusable filter input");
            gpu_check(source_->GetSurfaceLevel(0,surface_.put()),"Get filter input surface");
            width_=description.Width;height_=description.Height;format_=description.Format;
        }
        StateRestore state(device_,state_);
        std::array<Com<IDirect3DSurface9>,4> targets;
        Com<IDirect3DSurface9> depth;
        D3DCAPS9 caps{};
        gpu_check(device_.GetDeviceCaps(&caps),"Read filter target slots");
        const auto count=std::min<UINT>(caps.NumSimultaneousRTs,targets.size());
        for (UINT i=0;i<count;++i) {
            const auto status=device_.GetRenderTarget(i,targets[i].put());
            if (status!=D3DERR_NOTFOUND) gpu_check(status,"Save filter caller target");
        }
        const auto status=device_.GetDepthStencilSurface(depth.put());
        if (status!=D3DERR_NOTFOUND) gpu_check(status,"Save filter caller depth");
        auto restore=[&] {
            gpu_check(device_.SetRenderTarget(0,targets[0].get()),"Restore filter caller target");
            for (UINT i=1;i<count;++i)
                gpu_check(device_.SetRenderTarget(i,targets[i].get()),"Restore filter secondary target");
            gpu_check(device_.SetDepthStencilSurface(depth.get()),"Restore filter caller depth");
            gpu_check(state.apply(),"Restore filter caller state");
        };
        try {
            gpu_check(device_.StretchRect(&output,nullptr,surface_.get(),nullptr,D3DTEXF_NONE),
                "Copy completed stereo atlas without scene replay");
            gpu_check(device_.SetDepthStencilSurface(nullptr),"Detach filter depth");
            for (UINT i=1;i<count;++i) gpu_check(device_.SetRenderTarget(i,nullptr),"Detach filter MRT");
            gpu_check(device_.SetRenderTarget(0,&output),"Bind filter output");
            const D3DVIEWPORT9 viewport{0,0,width_,height_,0,1};
            gpu_check(device_.SetViewport(&viewport),"Set full stereo filter viewport");
            for (const auto& setting:std::initializer_list<std::pair<D3DRENDERSTATETYPE,DWORD>>{
                {D3DRS_ZENABLE,FALSE},{D3DRS_ZWRITEENABLE,FALSE},{D3DRS_STENCILENABLE,FALSE},
                {D3DRS_ALPHABLENDENABLE,FALSE},{D3DRS_ALPHATESTENABLE,FALSE},{D3DRS_FOGENABLE,FALSE},
                {D3DRS_SCISSORTESTENABLE,FALSE},{D3DRS_CLIPPLANEENABLE,0},{D3DRS_CULLMODE,D3DCULL_NONE},
                {D3DRS_FILLMODE,D3DFILL_SOLID},{D3DRS_COLORWRITEENABLE,15},{D3DRS_SRGBWRITEENABLE,FALSE}})
                gpu_check(device_.SetRenderState(setting.first,setting.second),"Set isolated filter state");
            gpu_check(device_.SetVertexShader(nullptr),"Use transformed filter quad");
            gpu_check(device_.SetPixelShader(shader_.get()),"Bind night-vision filter");
            const float options[]{night_vision?1.f:0.f,vignette,0,0};
            gpu_check(device_.SetPixelShaderConstantF(0,options,1),"Set stereo comfort filter options");
            gpu_check(device_.SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1),"Set filter vertex format");
            gpu_check(device_.SetTexture(0,source_.get()),"Bind completed stereo input");
            for (const auto& setting:std::initializer_list<std::pair<D3DSAMPLERSTATETYPE,DWORD>>{
                {D3DSAMP_MINFILTER,D3DTEXF_POINT},{D3DSAMP_MAGFILTER,D3DTEXF_POINT},
                {D3DSAMP_MIPFILTER,D3DTEXF_NONE},{D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP},
                {D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP},{D3DSAMP_SRGBTEXTURE,FALSE}})
                gpu_check(device_.SetSamplerState(0,setting.first,setting.second),"Set exact filter sampling");
            for (UINT i=0;i<caps.MaxStreams;++i)
                gpu_check(device_.SetStreamSourceFreq(i,1),"Disable inherited scene instancing for atlas filter");
            struct Vertex {float x,y,z,w,u,v;};
            const Vertex quad[]{{-.5f,-.5f,0,1,0,0},{float(width_)-.5f,-.5f,0,1,1,0},
                {-.5f,float(height_)-.5f,0,1,0,1},{float(width_)-.5f,float(height_)-.5f,0,1,1,1}};
            gpu_check(device_.BeginScene(),"Begin stereo atlas filter");
            const auto drawn=device_.DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex));
            const auto ended=device_.EndScene();
            gpu_check(drawn,"Filter both eyes in one fullscreen draw");
            gpu_check(ended,"End stereo atlas filter");
        } catch (...) {restore();throw;}
        restore();
    }
};

}
