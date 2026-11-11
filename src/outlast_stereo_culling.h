#pragma once
#include "stereo_frustum.h"
#include <cstring>

namespace olvr {

class OutlastStereoCulling {
    std::uintptr_t frustum_begin_;
    std::size_t count_;
    StereoFrustum stereo_;
public:
    OutlastStereoCulling(const void* views,std::size_t bytes,std::size_t count,
                         const StereoViewConstants& eyes)
        :frustum_begin_(reinterpret_cast<std::uintptr_t>(views)),count_(count),stereo_(eyes) {
        if (!views || !count || count>bytes/0x12c0 ||
            bytes>std::numeric_limits<std::uintptr_t>::max()-frustum_begin_)
            throw std::invalid_argument("Invalid owned native view array for stereo culling");
        frustum_begin_+=0x3b0;
    }
    bool owns(const void* frustum) const noexcept {
        const auto address=reinterpret_cast<std::uintptr_t>(frustum);
        if (address<frustum_begin_) return false;
        const auto offset=address-frustum_begin_;
        return offset%0x12c0==0 && offset/0x12c0<count_;
    }
    bool sphere(const void* frustum,const float* center,const float* radius,
                bool* fully_inside=nullptr) const {
        if (!owns(frustum) || !center || !radius)
            throw std::invalid_argument("Sphere is outside the owned stereo culling scope");
        Vec3 position{};float size=0;
        std::memcpy(&position,center,sizeof(position));std::memcpy(&size,radius,sizeof(size));
        return stereo_.native_sphere_margin(position,size,fully_inside);
    }
    bool box(const void* frustum,const float* center,const float* extent,
             bool* fully_inside=nullptr) const {
        if (!owns(frustum) || !center || !extent)
            throw std::invalid_argument("Box is outside the owned stereo culling scope");
        Vec3 position{},size{};
        std::memcpy(&position,center,sizeof(position));std::memcpy(&size,extent,sizeof(size));
        return stereo_.box(position,size,fully_inside);
    }
};

}
