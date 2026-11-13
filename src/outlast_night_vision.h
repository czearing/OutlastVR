#pragma once
#include "openxr_motion_input.h"
#include "outlast_camcorder.h"
#include "stereo_reconstruction.h"
#include <algorithm>
#include <optional>

namespace olvr {

struct OutlastNightLightPose {
    Vec3 translation;
    std::array<INT,3> rotation;
};
inline OutlastNightLightPose outlast_night_light_pose(const TrackingFrame& tracking,
    EyePose head,const Matrix4& actor_to_world,INT hero_yaw) {
    if (!std::isfinite(tracking.units_per_meter) || tracking.units_per_meter<=0)
        throw std::invalid_argument("Invalid native night-light world scale");
    const auto world=add(tracking.origin,
        scale(map_tracking(tracking,head.position),tracking.units_per_meter));
    const auto inverse=matrix_inverse(actor_to_world);
    const Vec3 local{
        world.x*inverse[0]+world.y*inverse[4]+world.z*inverse[8]+inverse[12],
        world.x*inverse[1]+world.y*inverse[5]+world.z*inverse[9]+inverse[13],
        world.x*inverse[2]+world.y*inverse[6]+world.z*inverse[10]+inverse[14]};
    if (!finite(local)) throw std::invalid_argument("Nonfinite native night-light translation");
    auto rotation=camcorder_native_rotation(tracking,head);
    rotation[1]=INT(std::uint32_t(rotation[1])-std::uint32_t(hero_yaw));
    rotation[2]=0;
    return {local,rotation};
}

class OutlastNightVision {
    bool enabled_=false,blocked_=true,down_=false;
    UINT64 clock_=0;
public:
    void synchronize(bool enabled) noexcept {enabled_=enabled;}
    bool update(const MotionFrame& motion,bool gameplay,UINT64 now,unsigned hand=1) {
        if (hand>=2) throw std::invalid_argument("Invalid night-vision hand");
        const bool current=motion.focused && motion.hands[hand].grip_tracked &&
            (!clock_ || (now>=clock_ && now-clock_<=250000));
        clock_=now;
        const bool pressed=motion.hands[hand].stick_click;
        if (!current || !gameplay) blocked_=true;
        else if (!pressed) blocked_=false;
        if (current && gameplay && !blocked_ && pressed && !down_) enabled_=!enabled_;
        down_=pressed;
        return enabled_;
    }
    std::optional<bool> update_native(const MotionFrame& motion,BYTE mode,bool gameplay,
        UINT64 now,unsigned hand=1) {
        if (mode>2) {
            // Retail scripted effects keep ownership; held input must not queue a toggle.
            update(motion,false,now,hand);
            return std::nullopt;
        }
        synchronize(mode==2);
        return update(motion,gameplay,now,hand);
    }
};

using OutlastNightEffect=void(WINAPI*)(void*,INT);
using OutlastCameraEffect=void(WINAPI*)(void*);
using OutlastNightLight=void(WINAPI*)(void*,UINT);
inline void outlast_native_night_lights(const std::array<void*,3>& lights,bool enabled,
    OutlastNightLight activate) {
    if (!activate || std::any_of(lights.begin(),lights.end(),[](void* light) {return !light;}))
        throw std::invalid_argument("Missing retail night-vision light binding");
    for (unsigned i=0;i<lights.size();++i)
        activate(lights[i],enabled?(i==0):(i==2));
}
inline void outlast_native_night_effect(void* effects,bool enabled,bool camera_raised,
    OutlastNightEffect night,OutlastCameraEffect normal,OutlastCameraEffect camera) {
    if (!effects || !night || !normal || !camera)
        throw std::invalid_argument("Missing retail night-vision effect binding");
    if (enabled) night(effects,1);
    else (camera_raised?camera:normal)(effects);
}

}
