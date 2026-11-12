#pragma once
#include "openxr_motion_input.h"
#include "xr_view_constants.h"
#include <cstring>
#include <optional>

namespace olvr {

struct OutlastUseRay {Vec3 origin,direction;};
struct OutlastNativeUseContext {UINT64 clock=0;unsigned basic=0,special=0;};

using OutlastPickupComplete=void(WINAPI*)(void*);
inline bool outlast_complete_controller_pickup(void* hero,INT action,UINT64 result,
                                               bool controller,OutlastPickupComplete complete,bool enabled=true) {
    if (!enabled || !controller || !action || !result) return false;
    if (!hero || !complete) throw std::invalid_argument("Missing native pickup completion binding");
    const auto* bytes=static_cast<const unsigned char*>(hero);
    if (bytes[0x7bc]!=2 || bytes[0x7bd]!=0x31) return false;
    void* selected=nullptr;
    std::memcpy(&selected,bytes+0x3648,sizeof(selected));
    if (!selected) throw std::runtime_error("Admitted native pickup lost its selected item");
    // Normal completion owns inventory/events, animation teardown and movement restoration.
    complete(hero);
    return true;
}

inline bool outlast_native_use_action_ready(bool body_ready,const OutlastNativeUseContext& context,UINT64 now) {
    return body_ready || (now>=context.clock && now-context.clock<=250000 && context.basic==2 &&
        (context.special==0x31 || context.special==0x32));
}

inline std::optional<OutlastUseRay> outlast_controller_use_ray(
    const TrackingFrame& tracking,const MotionFrame& motion,UINT64 sampled,UINT64 now,bool allowed,unsigned hand=1) {
    if (hand>=2) throw std::invalid_argument("Invalid native interaction hand");
    if (!allowed || !motion.focused || !motion.hands[hand].aim_tracked ||
        now<sampled || now-sampled>250000) return {};
    const auto& aim=motion.hands[hand].aim;
    const Vec3 position{aim.position.x,aim.position.y,aim.position.z};
    const Quaternion q{aim.orientation.x,aim.orientation.y,aim.orientation.z,aim.orientation.w};
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if (!finite(position) || !finite(tracking.origin) || !finite(tracking.right) ||
        !finite(tracking.up) || !finite(tracking.backward) ||
        !std::isfinite(tracking.units_per_meter) || tracking.units_per_meter<=0 ||
        !std::isfinite(norm) || std::abs(norm-1)>.0001f ||
        std::abs(dot(tracking.right,tracking.right)-1)>.0001f ||
        std::abs(dot(tracking.up,tracking.up)-1)>.0001f ||
        std::abs(dot(tracking.backward,tracking.backward)-1)>.0001f ||
        std::abs(dot(tracking.right,tracking.up))>.0001f ||
        std::abs(dot(tracking.right,tracking.backward))>.0001f ||
        std::abs(dot(tracking.up,tracking.backward))>.0001f)
        throw std::invalid_argument("Invalid controller-directed native use pose");
    const OutlastUseRay ray{
        add(tracking.origin,scale(map_tracking(tracking,position),tracking.units_per_meter)),
        map_tracking(tracking,rotate(q,{0,0,-1}))};
    if (!finite(ray.origin) || !finite(ray.direction))
        throw std::invalid_argument("Controller-directed native use pose overflowed");
    return ray;
}

class OutlastNativeUsePose {
    unsigned char* hero_;
    std::array<unsigned char,sizeof(Vec3)> origin_{},direction_{};
public:
    OutlastNativeUsePose(void* hero,const OutlastUseRay& ray):hero_(static_cast<unsigned char*>(hero)) {
        if (!hero_ || !finite(ray.origin) || !finite(ray.direction) ||
            std::abs(dot(ray.direction,ray.direction)-1)>.0001f)
            throw std::invalid_argument("Invalid native use-pose scope");
        std::memcpy(origin_.data(),hero_+0x30e4,origin_.size());
        std::memcpy(direction_.data(),hero_+0x2bd8,direction_.size());
        std::memcpy(hero_+0x30e4,&ray.origin,sizeof(ray.origin));
        std::memcpy(hero_+0x2bd8,&ray.direction,sizeof(ray.direction));
    }
    OutlastNativeUsePose(const OutlastNativeUsePose&)=delete;
    OutlastNativeUsePose& operator=(const OutlastNativeUsePose&)=delete;
    ~OutlastNativeUsePose() {
        std::memcpy(hero_+0x30e4,origin_.data(),origin_.size());
        std::memcpy(hero_+0x2bd8,direction_.data(),direction_.size());
    }
};

}
