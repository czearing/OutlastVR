#pragma once
#include "d3d9_resource.h"
#include <unordered_map>
#include <vector>
#include <memory>

namespace olvr {

inline unsigned native_point_atlas_samplers(IDirect3DDevice9& device,unsigned mask) {
    unsigned selected=0;
    for (unsigned sampler=0;sampler<16;++sampler) if (mask&(1u<<sampler)) {
        DWORD minimum=0,maximum=0;
        gpu_check(device.GetSamplerState(sampler,D3DSAMP_MINFILTER,&minimum),"Read native atlas minification");
        gpu_check(device.GetSamplerState(sampler,D3DSAMP_MAGFILTER,&maximum),"Read native atlas magnification");
        if (minimum!=D3DTEXF_POINT || maximum!=D3DTEXF_POINT) continue;
        Com<IDirect3DBaseTexture9> texture;
        gpu_check(device.GetTexture(sampler,texture.put()),"Read native point atlas texture");
        if (!texture.get() || texture->GetType()!=D3DRTYPE_TEXTURE)
            throw std::runtime_error("Native point atlas lacks a bound 2D texture");
        if (texture->GetLevelCount()==1) selected|=1u<<sampler;
    }
    return selected;
}

class D3D9PointAtlasLookup {
    std::unordered_map<UINT,std::unique_ptr<Com<IDirect3DTexture9>>> textures_;
public:
    void clear() noexcept {textures_.clear();}
    void bind(IDirect3DDevice9& device,const std::vector<std::pair<unsigned,unsigned>>& samplers) {
        for (const auto& entry:samplers) {
            Com<IDirect3DBaseTexture9> source;
            gpu_check(device.GetTexture(entry.first,source.put()),"Read point selector's native source");
            if (!source.get() || source->GetType()!=D3DRTYPE_TEXTURE || source->GetLevelCount()!=1)
                throw std::runtime_error("Native point selector requires its single-level 2D atlas");
            D3DSURFACE_DESC description{};
            gpu_check(static_cast<IDirect3DTexture9*>(source.get())->GetLevelDesc(0,&description),
                "Read native point selector width");
            if (description.Width<2 || description.Width%2)
                throw std::runtime_error("Native point selector lacks equal-width atlas eyes");
            const auto width=description.Width/2;
            auto found=textures_.find(width);
            if (found==textures_.end()) {
                auto created=std::make_unique<Com<IDirect3DTexture9>>();
                auto& texture=*created;
                gpu_check(device.CreateTexture(width,1,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,
                    texture.put(),nullptr),"Create native-width point selector");
                D3DLOCKED_RECT lock{};
                gpu_check(texture->LockRect(0,&lock,nullptr,0),"Lock native point selector");
                for (UINT x=0;x<width;++x)
                    static_cast<float*>(lock.pBits)[x]=(float(x)+.5f)/float(width);
                gpu_check(texture->UnlockRect(0),"Unlock native point selector");
                found=textures_.emplace(width,std::move(created)).first;
            }
            gpu_check(device.SetTexture(entry.second,found->second->get()),"Bind native-width point selector");
            for (const auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
                gpu_check(device.SetSamplerState(entry.second,filter,D3DTEXF_POINT),"Preserve native point selection");
            gpu_check(device.SetSamplerState(entry.second,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable selector mipmaps");
            for (const auto address:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})
                gpu_check(device.SetSamplerState(entry.second,address,D3DTADDRESS_CLAMP),"Clamp native point selector");
            gpu_check(device.SetSamplerState(entry.second,D3DSAMP_SRGBTEXTURE,FALSE),"Keep selector coordinates linear");
        }
    }
};

} // namespace olvr
