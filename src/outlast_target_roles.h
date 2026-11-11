#pragma once
#include "d3d9_resource.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace olvr {

enum class OutlastTargetRole : DWORD {
    shared, shadow, scene_color, scene_depth, lighting, filter, half_resolution,
    velocity, fog, gbuffer, post, ambient_occlusion, history
};

struct OutlastAllocationSite {
    std::uintptr_t return_rva;
    OutlastTargetRole role;
};

inline constexpr std::array<OutlastAllocationSite,54> outlast_allocation_sites{{
    {0x8da420,OutlastTargetRole::scene_depth}, {0x8da4f1,OutlastTargetRole::scene_depth},
    {0x8da5b3,OutlastTargetRole::scene_depth},
    {0x8da492,OutlastTargetRole::scene_depth}, {0x8da64b,OutlastTargetRole::scene_color},
    {0x8da7c1,OutlastTargetRole::lighting}, {0x8da889,OutlastTargetRole::lighting},
    {0x8da942,OutlastTargetRole::shared},
    {0x8daa9f,OutlastTargetRole::filter}, {0x8dab5d,OutlastTargetRole::filter},
    {0x8dac18,OutlastTargetRole::filter},
    {0x8dacd3,OutlastTargetRole::half_resolution}, {0x8dad2b,OutlastTargetRole::half_resolution},
    {0x8dae5e,OutlastTargetRole::post}, {0x8daee6,OutlastTargetRole::velocity},
    {0x8db0ce,OutlastTargetRole::shadow}, {0x8db19b,OutlastTargetRole::shadow},
    {0x8db268,OutlastTargetRole::shadow}, {0x8db335,OutlastTargetRole::shadow},
    {0x8db767,OutlastTargetRole::shadow}, {0x8db82d,OutlastTargetRole::shadow},
    {0x8db8f3,OutlastTargetRole::shadow}, {0x8db9b9,OutlastTargetRole::shadow},
    {0x8dba7f,OutlastTargetRole::shadow}, {0x8dbb45,OutlastTargetRole::shadow},
    {0x8dbcc6,OutlastTargetRole::shadow}, {0x8dbdc2,OutlastTargetRole::shadow},
    {0x8dbedb,OutlastTargetRole::shadow}, {0x8dbff1,OutlastTargetRole::shadow},
    {0x8dc107,OutlastTargetRole::shadow}, {0x8dc216,OutlastTargetRole::shadow},
    {0x8dc32f,OutlastTargetRole::shadow}, {0x8dc445,OutlastTargetRole::shadow},
    {0x8dc55b,OutlastTargetRole::shadow}, {0x8dc663,OutlastTargetRole::shadow},
    {0x8dc6ee,OutlastTargetRole::shadow}, {0x8dc772,OutlastTargetRole::shadow},
    {0x8dc7fa,OutlastTargetRole::shadow},
    {0x8dcadd,OutlastTargetRole::fog}, {0x8dcc9d,OutlastTargetRole::shared},
    {0x8dce2d,OutlastTargetRole::gbuffer}, {0x8dcf4d,OutlastTargetRole::gbuffer},
    {0x8dd07d,OutlastTargetRole::gbuffer}, {0x8dd1ae,OutlastTargetRole::gbuffer},
    {0x8dd392,OutlastTargetRole::gbuffer}, {0x8dd4b1,OutlastTargetRole::gbuffer},
    {0x8dd5d3,OutlastTargetRole::post}, {0x8dd6fc,OutlastTargetRole::post},
    {0x8dd828,OutlastTargetRole::gbuffer}, {0x8dd947,OutlastTargetRole::gbuffer},
    {0x8dda8e,OutlastTargetRole::post}, {0x8ddbab,OutlastTargetRole::post},
    {0x8d6dc8,OutlastTargetRole::ambient_occlusion}, {0x8d6eb1,OutlastTargetRole::history}
}};

inline bool stereo_target(OutlastTargetRole role) {
    return role!=OutlastTargetRole::shared && role!=OutlastTargetRole::shadow;
}

inline bool outlast_allocation_role(std::uintptr_t return_rva, OutlastTargetRole& role) {
    for (const auto& site : outlast_allocation_sites)
        if (site.return_rva==return_rva) { role=site.role; return true; }
    return false;
}

struct OutlastAtlasTag {
    DWORD version=1;
    OutlastTargetRole role;
    UINT logical_width, logical_height;
};
inline constexpr GUID outlast_atlas_guid{
    0x4716ea5f,0xe358,0x4a75,{0x9c,0x42,0x6b,0x6a,0x84,0xa9,0xc8,0x53}};

inline void tag_atlas_texture(IDirect3DTexture9& texture,OutlastTargetRole role,
                              UINT logical_width,UINT logical_height) {
    if (!stereo_target(role) || !logical_width || !logical_height ||
        logical_width>std::numeric_limits<UINT>::max()/2)
        throw std::invalid_argument("Invalid role-classified atlas extent");
    D3DSURFACE_DESC description{};
    gpu_check(texture.GetLevelDesc(0,&description),"Read tagged atlas extent");
    if (description.Width!=logical_width*2 || description.Height!=logical_height)
        throw std::invalid_argument("Native atlas allocation does not preserve its logical extent");
    const OutlastAtlasTag tag{1,role,logical_width,logical_height};
    gpu_check(texture.SetPrivateData(outlast_atlas_guid,&tag,sizeof(tag),0),"Tag native atlas texture role");
    Com<IDirect3DSurface9> surface;
    gpu_check(texture.GetSurfaceLevel(0,surface.put()),"Get role-classified atlas surface");
    gpu_check(surface->SetPrivateData(outlast_atlas_guid,&tag,sizeof(tag),0),"Tag native atlas surface role");
}

inline bool atlas_tag(IDirect3DResource9& resource,OutlastAtlasTag& tag) {
    DWORD bytes=sizeof(tag);
    const auto status=resource.GetPrivateData(outlast_atlas_guid,&tag,&bytes);
    if (status==D3DERR_NOTFOUND) return false;
    gpu_check(status,"Read native resource role tag");
    if (bytes!=sizeof(tag) || tag.version!=1 || !stereo_target(tag.role) ||
        static_cast<DWORD>(tag.role)>static_cast<DWORD>(OutlastTargetRole::history) ||
        !tag.logical_width || !tag.logical_height ||
        tag.logical_width>std::numeric_limits<UINT>::max()/2)
        throw std::runtime_error("Native resource has an invalid atlas role tag");
    return true;
}

inline unsigned atlas_sampler_mask(IDirect3DDevice9& device,unsigned declared=0xffffu) {
    if (declared>0xffffu) throw std::invalid_argument("Declared sampler mask exceeds SM3 range");
    unsigned mask=0;
    for (UINT sampler=0;sampler<16;++sampler) {
        if (!(declared&(1u<<sampler))) continue;
        Com<IDirect3DBaseTexture9> texture;
        gpu_check(device.GetTexture(sampler,texture.put()),"Get role-classified pixel sampler");
        OutlastAtlasTag tag{};
        if (!texture.get() || !atlas_tag(*texture.get(),tag)) continue;
        if (texture->GetType()!=D3DRTYPE_TEXTURE)
            throw std::runtime_error("Atlas sampler is not a native 2D texture");
        mask|=1u<<sampler;
    }
    return mask;
}

} // namespace olvr
