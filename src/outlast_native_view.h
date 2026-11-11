#pragma once
#include "xr_view_constants.h"
#include <cstring>
#include <limits>

namespace olvr {

struct OutlastViewCamera {
    TrackingFrame tracking;
    float near_plane, far_plane, depth_scale;
    EyeFov original_fov;
};

inline TrackingFrame outlast_upright_tracking(TrackingFrame frame,Vec3 world_up) {
    constexpr float tolerance=.0001f;
    const float handedness=dot(cross(frame.right,frame.up),frame.backward);
    if (!finite(world_up) || std::abs(dot(world_up,world_up)-1)>tolerance ||
        !finite(frame.right) || !finite(frame.up) || !finite(frame.backward) ||
        !std::isfinite(handedness) || std::abs(std::abs(handedness)-1)>tolerance ||
        std::abs(dot(frame.right,frame.right)-1)>tolerance ||
        std::abs(dot(frame.up,frame.up)-1)>tolerance ||
        std::abs(dot(frame.backward,frame.backward)-1)>tolerance ||
        std::abs(dot(frame.right,frame.up))>tolerance ||
        std::abs(dot(frame.right,frame.backward))>tolerance ||
        std::abs(dot(frame.up,frame.backward))>tolerance)
        throw std::invalid_argument("Upright native tracking requires a unit gravity axis and orthonormal basis");
    const float sign=handedness<0?-1.f:1.f;
    auto backward=add(frame.backward,scale(world_up,-dot(frame.backward,world_up)));
    const float length=std::sqrt(dot(backward,backward));
    if (length<.00001f) {
        auto right=add(frame.right,scale(world_up,-dot(frame.right,world_up)));
        const float right_length=std::sqrt(dot(right,right));
        if (!std::isfinite(right_length) || right_length<.00001f)
            throw std::invalid_argument("Native vertical camera has no horizontal heading");
        right=scale(right,1/right_length);
        backward=scale(cross(right,world_up),sign);
    } else {
        backward=scale(backward,1/length);
    }
    frame.backward=backward;
    frame.right=scale(cross(world_up,backward),sign);
    frame.up=world_up;
    return frame;
}

inline bool outlast_orthographic_view(const std::array<float,16>& matrix) {
    for (const auto value:matrix)
        if (!std::isfinite(value))
            throw std::invalid_argument("Outlast view projection is nonfinite");
    if (std::abs(matrix[15]-1)>=.0001f) return false;
    const auto row=std::abs(matrix[12])+std::abs(matrix[13])+std::abs(matrix[14]);
    return row<.0001f;
}

inline std::array<float,16> outlast_view_matrix(const void* view, std::size_t bytes) {
    if (!view || bytes<0x240)
        throw std::invalid_argument("Invalid bounded native view matrix");
    std::array<float,16> native{},matrix{};
    std::memcpy(native.data(),static_cast<const unsigned char*>(view)+0x200,sizeof(native));
    for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column)
        matrix[row*4+column]=native[column*4+row];
    return matrix;
}

inline OutlastViewCamera outlast_view_camera(const void* view, std::size_t bytes,
                                            float units_per_meter) {
    if (!view || bytes < 0x3ac || !std::isfinite(units_per_meter) || units_per_meter <= 0)
        throw std::invalid_argument("Invalid bounded Outlast view or world scale");
    const auto* data = static_cast<const unsigned char*>(view);
    std::array<float,16> matrix{};
    OutlastViewCamera result{};
    matrix=outlast_view_matrix(view,bytes);
    std::memcpy(&result.tracking.origin, data+0x3a0, sizeof(Vec3));
    std::memcpy(&result.tracking.pretranslation, data+0x280, sizeof(Vec3));
    result.tracking.units_per_meter = units_per_meter;
    if (!finite(result.tracking.origin) || !finite(result.tracking.pretranslation))
        throw std::invalid_argument("Outlast camera origin is nonfinite");
    for (const auto value : matrix)
        if (!std::isfinite(value))
            throw std::invalid_argument("Outlast view projection is nonfinite");
    auto row = [&](unsigned index) {
        return Vec3{matrix[index*4],matrix[index*4+1],matrix[index*4+2]};
    };
    const auto w = row(3);
    const float normalization = std::sqrt(dot(w,w));
    if (!std::isfinite(normalization) || normalization < .000001f)
        throw std::invalid_argument("Outlast main view is not perspective");
    for (auto& value : matrix) value /= normalization;
    const auto forward = row(3);
    const float bias_x = dot(row(0),forward), bias_y = dot(row(1),forward);
    const auto right_scaled = add(row(0),scale(forward,-bias_x));
    const auto up_scaled = add(row(1),scale(forward,-bias_y));
    const float sx = std::sqrt(dot(right_scaled,right_scaled));
    const float sy = std::sqrt(dot(up_scaled,up_scaled));
    if (!std::isfinite(sx) || !std::isfinite(sy) || sx < .000001f || sy < .000001f)
        throw std::invalid_argument("Outlast perspective basis is degenerate");
    result.tracking.right = scale(right_scaled,1/sx);
    result.tracking.up = scale(up_scaled,1/sy);
    result.tracking.backward = scale(forward,-1);
    const float depth_scale = dot(row(2),forward);
    const auto depth_residual = add(row(2),scale(forward,-depth_scale));
    if (std::abs(dot(result.tracking.right,result.tracking.up)) > .0001f ||
        dot(depth_residual,depth_residual) > .000001f ||
        depth_scale < .998999f)
        throw std::invalid_argument("Outlast matrix is not a supported D3D perspective projection");
    const auto translated = add(result.tracking.origin,result.tracking.pretranslation);
    auto translation_tolerance = [&](unsigned index) {
        const auto axis=row(index);
        const auto origin=result.tracking.origin,pre=result.tracking.pretranslation;
        // UE3 concatenates the two large translations before their cancellation.
        const double magnitude=
            std::abs(double(axis.x)*origin.x)+std::abs(double(axis.x)*pre.x)+
            std::abs(double(axis.y)*origin.y)+std::abs(double(axis.y)*pre.y)+
            std::abs(double(axis.z)*origin.z)+std::abs(double(axis.z)*pre.z)+
            std::abs(double(matrix[index*4+3]));
        return .002+16*std::numeric_limits<float>::epsilon()*magnitude;
    };
    for (const unsigned index : {0u,1u,3u}) {
        const auto axis = row(index);
        const float value = dot(axis,translated)+matrix[index*4+3];
        if (!std::isfinite(value) || std::abs(value) > translation_tolerance(index))
            throw std::invalid_argument("Outlast matrix and translated world origin disagree");
    }
    result.near_plane = -(dot(row(2),translated)+matrix[11])/depth_scale;
    result.depth_scale=depth_scale;
    if (!std::isfinite(result.near_plane) || result.near_plane <= 0)
        throw std::invalid_argument("Outlast perspective near plane is invalid");
    result.far_plane = depth_scale > 1+.000001f ?
        result.near_plane*depth_scale/(depth_scale-1) : result.near_plane*100000000.f;
    result.original_fov = {std::atan((-1-bias_x)/sx),std::atan((1-bias_x)/sx),
                           std::atan((1-bias_y)/sy),std::atan((-1-bias_y)/sy)};
    // Rebuild all four rows to reject a coincidentally plausible, incorrectly oriented matrix.
    const auto reconstructed = eye_constants(result.tracking,{{0,0,0},{0,0,0,1}},
        result.original_fov,result.near_plane,result.far_plane,result.depth_scale);
    for (unsigned index=0; index<16; ++index)
        if (std::abs(reconstructed[index]-matrix[index]) >
            (index%4==3?translation_tolerance(index/4):
                .002+16*std::numeric_limits<float>::epsilon()*std::abs(matrix[index])))
            throw std::invalid_argument("Outlast perspective reconstruction differs from uploaded constants");
    return result;
}

inline Quaternion multiply(Quaternion a, Quaternion b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}

inline bool outlast_floor_reference_available(float height,bool position_tracked,bool orientation_tracked) {
    if (!std::isfinite(height)) throw std::invalid_argument("Nonfinite located floor height");
    return height>0 && position_tracked && orientation_tracked;
}

inline float outlast_floor_eye_height(float native_height,float native_standing,
                                     float measured_standing,float units,bool physical_crouch,bool airborne=false) {
    if (!std::isfinite(native_height) || !std::isfinite(native_standing) ||
        !std::isfinite(measured_standing) || !std::isfinite(units) || measured_standing<=0 || units<=0)
        throw std::invalid_argument("Invalid native or measured floor-height reference");
    return measured_standing*units+((physical_crouch || airborne)?0.f:std::min(0.f,native_height-native_standing));
}

class OutlastRecenter {
    Vec3 center_{};
    Quaternion inverse_yaw_{0,0,0,1};
    bool initialized_ = false;
    static void validate(EyePose pose) {
        const auto q = pose.orientation;
        const float norm = q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
        if (!finite(pose.position) || !std::isfinite(norm) || std::abs(norm-1) > .0001f)
            throw std::invalid_argument("Cannot recenter invalid located eyes");
    }
public:
    bool initialized() const noexcept { return initialized_; }
    float standing_height() const {
        if (!initialized_) throw std::logic_error("Outlast tracking has not been recentered");
        return center_.y;
    }
    void change_reference(EyePose new_origin_in_previous) {
        if (!initialized_) return;
        validate(new_origin_in_previous);
        const auto q = new_origin_in_previous.orientation;
        center_ = rotate({-q.x,-q.y,-q.z,q.w},add(center_,scale(new_origin_in_previous.position,-1)));
    }
    void recenter(const std::array<EyePose,2>& eyes,bool preserve_height=false) {
        for (const auto& eye : eyes) validate(eye);
        const auto a = eyes[0].orientation, b = eyes[1].orientation;
        const float sign = a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w < 0 ? -1.f : 1.f;
        Quaternion center{a.x+sign*b.x,a.y+sign*b.y,a.z+sign*b.z,a.w+sign*b.w};
        const float length = std::sqrt(center.x*center.x+center.y*center.y+
                                       center.z*center.z+center.w*center.w);
        if (!std::isfinite(length) || length < .000001f)
            throw std::invalid_argument("Located eye orientations have no recenter midpoint");
        center = {center.x/length,center.y/length,center.z/length,center.w/length};
        const auto direction = rotate(center,{0,0,-1});
        const auto right=rotate(center,{1,0,0});
        const float yaw = direction.x*direction.x+direction.z*direction.z >= .000001f?
            std::atan2(-direction.x,-direction.z):std::atan2(-right.z,right.x);
        const Quaternion inverse{0,-std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
        const auto position = scale(add(eyes[0].position,eyes[1].position),.5f);
        inverse_yaw_ = inverse;
        center_ = {position.x,preserve_height && initialized_?center_.y:position.y,position.z};
        initialized_ = true;
    }
    EyePose relative(EyePose eye) const {
        if (!initialized_) throw std::logic_error("Outlast tracking has not been recentered");
        validate(eye);
        return {rotate(inverse_yaw_,add(eye.position,scale(center_,-1))),
                multiply(inverse_yaw_,eye.orientation)};
    }
};

} // namespace olvr
