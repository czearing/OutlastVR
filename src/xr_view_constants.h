#pragma once
#include "d3d9_stereo_pipeline.h"
#include <cmath>

namespace olvr {

struct Vec3 { float x, y, z; };
struct Quaternion { float x, y, z, w; };
struct EyePose { Vec3 position; Quaternion orientation; };
struct EyeFov { float left, right, up, down; };
struct TrackingFrame {
    Vec3 origin;
    Vec3 right, up, backward;
    float units_per_meter;
    Vec3 pretranslation;
};

inline float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 scale(Vec3 v, float s) { return {v.x*s, v.y*s, v.z*s}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
inline bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline Vec3 rotate(Quaternion q, Vec3 v) {
    const Vec3 xyz{q.x, q.y, q.z};
    const auto t = scale(cross(xyz, v), 2);
    return add(v, add(scale(t, q.w), cross(xyz, t)));
}
inline Vec3 map_tracking(const TrackingFrame& frame, Vec3 v) {
    return add(add(scale(frame.right, v.x), scale(frame.up, v.y)), scale(frame.backward, v.z));
}

inline std::array<float, 32> eye_constants(const TrackingFrame& frame, EyePose pose, EyeFov fov,
                                          float near_plane, float far_plane, float native_depth_scale=0) {
    if (!finite(frame.origin) || !finite(frame.pretranslation) || !finite(frame.right) ||
        !finite(frame.up) || !finite(frame.backward) || !finite(pose.position) ||
        !std::isfinite(frame.units_per_meter) || frame.units_per_meter <= 0 ||
        !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
        near_plane <= 0 || far_plane <= near_plane)
        throw std::invalid_argument("Invalid tracking frame, world scale or clipping planes");
    constexpr float tolerance = .0001f, half_pi = 1.57079632679f;
    if (std::abs(dot(frame.right, frame.right)-1) > tolerance ||
        std::abs(dot(frame.up, frame.up)-1) > tolerance ||
        std::abs(dot(frame.backward, frame.backward)-1) > tolerance ||
        std::abs(dot(frame.right, frame.up)) > tolerance ||
        std::abs(dot(frame.right, frame.backward)) > tolerance ||
        std::abs(dot(frame.up, frame.backward)) > tolerance)
        throw std::invalid_argument("Tracking-to-engine basis must be orthonormal");
    const auto q = pose.orientation;
    const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!std::isfinite(norm) || std::abs(norm-1) > tolerance)
        throw std::invalid_argument("Eye quaternion must be normalized");
    if (!std::isfinite(fov.left) || !std::isfinite(fov.right) || !std::isfinite(fov.up) ||
        !std::isfinite(fov.down) || fov.left <= -half_pi || fov.right >= half_pi ||
        fov.down <= -half_pi || fov.up >= half_pi || fov.left >= fov.right || fov.down >= fov.up)
        throw std::invalid_argument("Eye FOV is invalid");
    const float left = std::tan(fov.left), right = std::tan(fov.right);
    const float up = std::tan(fov.up), down = std::tan(fov.down);
    const float sx = 2/(right-left), sy = 2/(up-down);
    const float bx = -(right+left)/(right-left), by = -(up+down)/(up-down);
    const auto eye_world = add(frame.origin, scale(map_tracking(frame, pose.position), frame.units_per_meter));
    const auto eye_translated = add(eye_world, frame.pretranslation);
    const auto eye_right = map_tracking(frame, rotate(q, {1,0,0}));
    const auto eye_up = map_tracking(frame, rotate(q, {0,1,0}));
    const auto eye_forward = map_tracking(frame, rotate(q, {0,0,-1}));
    if (!std::isfinite(native_depth_scale) || (native_depth_scale!=0 && native_depth_scale<.998999f))
        throw std::invalid_argument("Unsupported native depth projection scale");
    const float depth_scale = native_depth_scale!=0?native_depth_scale:far_plane/(far_plane-near_plane);
    const Vec3 rows[]{add(scale(eye_right, sx), scale(eye_forward, bx)),
                      add(scale(eye_up, sy), scale(eye_forward, by)),
                      scale(eye_forward, depth_scale), eye_forward};
    std::array<float, 32> constants{};
    for (UINT row = 0; row < 4; ++row) {
        constants[row*4] = rows[row].x;
        constants[row*4+1] = rows[row].y;
        constants[row*4+2] = rows[row].z;
        constants[row*4+3] = -dot(rows[row], eye_translated);
    }
    constants[11] -= near_plane*depth_scale;
    constants[16] = eye_translated.x;
    constants[17] = eye_translated.y;
    constants[18] = eye_translated.z;
    constants[19] = 1;
    constants[20] = frame.pretranslation.x;
    constants[21] = frame.pretranslation.y;
    constants[22] = frame.pretranslation.z;
    constants[23] = 1;
    for (const auto value : constants)
        if (!std::isfinite(value)) throw std::overflow_error("Eye transform exceeded finite engine coordinates");
    return constants;
}

} // namespace olvr
