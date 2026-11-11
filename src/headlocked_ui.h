#pragma once
#include "outlast_native_view.h"
#include "stereo_reconstruction.h"

namespace olvr {
inline Quaternion binocular_orientation(const std::array<EyePose,2>& eyes) {
    const auto a=eyes[0].orientation,b=eyes[1].orientation;
    const float sign=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w<0?-1.f:1.f;
    Quaternion orientation{a.x+sign*b.x,a.y+sign*b.y,a.z+sign*b.z,a.w+sign*b.w};
    const float norm=std::sqrt(orientation.x*orientation.x+orientation.y*orientation.y+
        orientation.z*orientation.z+orientation.w*orientation.w);
    if (!std::isfinite(norm) || norm<.000001f) throw std::invalid_argument("Invalid UI head orientation");
    return {orientation.x/norm,orientation.y/norm,orientation.z/norm,orientation.w/norm};
}

inline StereoViewConstants headlocked_ui_constants(const std::array<EyePose,2>& eyes,
    const std::array<EyeFov,2>& fovs,float aspect,float distance=2.f) {
    if (!std::isfinite(aspect) || aspect<=0 || !std::isfinite(distance) || distance<=.01f)
        throw std::invalid_argument("Invalid headlocked UI plane");
    const auto orientation=binocular_orientation(eyes);
    const auto right=rotate(orientation,{1,0,0}),up=rotate(orientation,{0,1,0});
    const auto center=add(scale(add(eyes[0].position,eyes[1].position),.5f),
        scale(rotate(orientation,{0,0,-1}),distance));
    const TrackingFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1},1,{0,0,0}};
    StereoViewConstants result{};
    float half_height=std::numeric_limits<float>::infinity();
    for (unsigned eye=0;eye<2;++eye) {
        result[eye]=eye_constants(frame,eyes[eye],fovs[eye],.01f,1000);
        for (unsigned axis=0;axis<2;++axis) for (float side:{-1.f,1.f}) {
            const auto& m=result[eye];
            const Vec3 normal{m[12]+side*m[axis*4],m[13]+side*m[axis*4+1],m[14]+side*m[axis*4+2]};
            const float available=dot(normal,center)+m[15]+side*m[axis*4+3];
            const float extent=aspect*std::abs(dot(normal,right))+std::abs(dot(normal,up));
            if (available<=0 || extent<=0) throw std::invalid_argument("UI plane is outside a located eye frustum");
            half_height=std::min(half_height,available/extent);
        }
    }
    Matrix4 plane{};
    for (unsigned row=0;row<3;++row) {
        const auto component=[row](Vec3 v){return row==0?v.x:row==1?v.y:v.z;};
        plane[row*4]=component(right)*aspect*half_height;
        plane[row*4+1]=component(up)*half_height;
        plane[row*4+3]=component(center);
    }
    plane[15]=1;
    for (auto& eye:result) {
        Matrix4 projection{};std::copy_n(eye.begin(),16,projection.begin());
        const auto transform=matrix_product(projection,plane);
        std::copy(transform.begin(),transform.end(),eye.begin());
    }
    return result;
}
}
