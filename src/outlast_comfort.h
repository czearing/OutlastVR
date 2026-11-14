#pragma once
#include "outlast_config.h"
#include "outlast_native_view.h"
#include "openxr_motion_input.h"
#include <optional>

namespace olvr {
class OutlastBodyFollow {
    float yaw_=0;
    bool following_=false;
    UINT64 clock_=0;
    std::uintptr_t hero_=0;
public:
    void reset() {*this=OutlastBodyFollow{};}
    float update(bool enabled,float deadzone,Quaternion head,std::uintptr_t hero,UINT64 now) {
        if (!std::isfinite(deadzone) || deadzone<10 || deadzone>90)
            throw std::invalid_argument("Invalid body-follow deadzone");
        const auto forward=rotate(head,{0,0,-1});
        if (!finite(forward)) throw std::invalid_argument("Nonfinite body-follow orientation");
        if (!enabled || !hero) {reset();return 0;}
        if (hero!=hero_ || !clock_ || now<clock_ || now-clock_>250000) {
            reset();hero_=hero;clock_=now;return 0;
        }
        const float elapsed=float(now-clock_)/1000000;
        clock_=now;
        // Looking straight up/down has no stable horizontal heading.
        if (forward.x*forward.x+forward.z*forward.z<.000001f) return yaw_;
        const float desired=std::atan2(-forward.x,-forward.z);
        const float delta=std::remainder(desired-yaw_,6.28318530718f);
        if (std::abs(delta)>deadzone*.01745329252f) following_=true;
        if (following_) {
            const float step=std::clamp(delta,-6.28318530718f*elapsed,6.28318530718f*elapsed);
            yaw_=std::remainder(yaw_+step,6.28318530718f);
            if (std::abs(delta-step)<.0001f) following_=false;
        }
        return yaw_;
    }
};
class OutlastComfort {
    float yaw_=0,head_y_=0;
    Vec3 offset_{};
    bool armed_=false;
    UINT64 clock_=0;
    static Quaternion rotation(float yaw) {return {0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};}
    static float heading(Quaternion q) {
        const auto forward=rotate(q,{0,0,-1}),right=rotate(q,{1,0,0});
        return forward.x*forward.x+forward.z*forward.z>.000001f?
            std::atan2(-forward.x,-forward.z):std::atan2(-right.z,right.x);
    }
public:
    void reset() {*this=OutlastComfort{};}
    void update(const OutlastComfortSettings& options,const MotionFrame& motion,
                EyePose head,bool gameplay,UINT64 now) {
        head_y_=options.seated?head.position.y:0;
        const bool turn_active=options.left_handed?motion.move_active:motion.turn_active;
        const bool current=motion.focused && turn_active && (!clock_ || (now>=clock_ && now-clock_<=250000));
        clock_=now;
        if (!current || !gameplay) {armed_=false;return;}
        const float turn=options.left_handed?motion.move.x:motion.turn.x;
        if (std::abs(turn)<.2f) armed_=true;
        if (!options.snap_turn_degrees || std::abs(turn)<.7f || !armed_) return;
        const auto pivot=add(rotate(rotation(yaw_),head.position),offset_);
        yaw_=std::remainder(yaw_-(turn>0?1.f:-1.f)*options.snap_turn_degrees*.0174532925199433f,
            6.283185307179586f);
        offset_=add(pivot,scale(rotate(rotation(yaw_),head.position),-1));
        armed_=false;
    }
    EyePose pose(EyePose value) const {
        value.position=add(rotate(rotation(yaw_),value.position),offset_);
        value.position.y-=head_y_;
        value.orientation=multiply(rotation(yaw_),value.orientation);
        return value;
    }
    std::optional<float> movement_yaw(const OutlastComfortSettings& options,const MotionFrame& motion,
                                    const OutlastRecenter& recenter,EyePose head) const {
        if (options.movement_reference==MovementReference::game) return yaw_;
        if (options.movement_reference==MovementReference::head) return heading(pose(head).orientation);
        const unsigned hand=options.movement_reference==MovementReference::main_hand?
            options.primary_hand():1-options.primary_hand();
        if (!motion.hands[hand].grip_tracked) return {};
        const auto& q=motion.hands[hand].grip.orientation;
        return heading(pose(recenter.relative({{}, {q.x,q.y,q.z,q.w}})).orientation);
    }
    static float vignette(const OutlastComfortSettings& options,const MotionFrame& motion,bool gameplay) {
        if (!gameplay || !motion.focused) return 0;
        const auto move=options.left_handed?motion.turn:motion.move;
        const auto turn=options.left_handed?motion.move:motion.turn;
        const float activity=std::max(std::sqrt(move.x*move.x+move.y*move.y),
            options.snap_turn_degrees?0.f:std::abs(turn.x));
        return options.vignette_strength*std::clamp(activity,0.f,1.f);
    }
};
}
