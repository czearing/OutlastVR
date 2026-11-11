#pragma once
#include "openxr_motion_input.h"
#include "outlast_body_ik.h"
#include "stereo_reconstruction.h"

namespace olvr {

struct CamcorderNativeState {
    bool available=false,controls_allowed=false,raised=false,transitioning=false;
};

struct CamcorderFrame {
    EyePose pose{},lens{},screen{};
    Vec3 screen_half_extents{.045f,.0253125f,1};
    bool held=false,visible=false,request_toggle=false,request_rejected=false;
    int hand=-1;
};

inline EyePose camcorder_offset(EyePose parent,Vec3 offset,Quaternion orientation={0,0,0,1}) {
    if (!finite(parent.position) || !finite(offset))
        throw std::invalid_argument("Nonfinite camcorder attachment position");
    const auto q=parent.orientation;
    const float squared=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if (!std::isfinite(squared) || std::abs(squared-1)>.0001f)
        throw std::invalid_argument("Camcorder attachment rotation must be normalized");
    const auto child=orientation;
    const float child_squared=child.x*child.x+child.y*child.y+child.z*child.z+child.w*child.w;
    if (!std::isfinite(child_squared) || std::abs(child_squared-1)>.0001f)
        throw std::invalid_argument("Camcorder local rotation must be normalized");
    return {add(parent.position,rotate(q,offset)),bone_multiply(q,orientation)};
}

inline EyePose camcorder_grip(const XrPosef& grip,unsigned hand) {
    if (hand>1) throw std::invalid_argument("Invalid camcorder holding hand");
    return camcorder_offset({{grip.position.x,grip.position.y,grip.position.z},
        {grip.orientation.x,grip.orientation.y,grip.orientation.z,grip.orientation.w}},
        {hand==0?.045f:-.045f,.015f,0});
}

inline EyePose camcorder_holster(EyePose head,Quaternion torso_yaw) {
    if (std::abs(torso_yaw.x)>.0001f || std::abs(torso_yaw.z)>.0001f)
        throw std::invalid_argument("Chest holster requires upright torso yaw, not headset pitch/roll");
    head.orientation=torso_yaw;
    return camcorder_offset(head,{.11f,-.32f,-.16f});
}

inline std::array<INT,3> camcorder_native_rotation(const TrackingFrame& tracking,EyePose lens) {
    camcorder_offset(lens,{0,0,0});
    const auto forward=map_tracking(tracking,rotate(lens.orientation,{0,0,-1}));
    const auto up=map_tracking(tracking,rotate(lens.orientation,{0,1,0}));
    if (!finite(forward) || !finite(up) ||
        std::abs(dot(forward,forward)-1)>.0001f || std::abs(dot(up,up)-1)>.0001f ||
        std::abs(dot(forward,up))>.0001f)
        throw std::invalid_argument("Native camcorder rotation requires orthonormal world axes");
    const float yaw=std::atan2(forward.y,forward.x);
    const float pitch=std::atan2(forward.z,std::hypot(forward.x,forward.y));
    const Vec3 right{-std::sin(yaw),std::cos(yaw),0};
    const Vec3 level_up{-std::sin(pitch)*std::cos(yaw),-std::sin(pitch)*std::sin(yaw),std::cos(pitch)};
    const float roll=std::atan2(dot(up,right),dot(up,level_up));
    constexpr double units=65536./6.2831853071795864769;
    return {INT(std::lround(pitch*units)),INT(std::lround(yaw*units)),INT(std::lround(roll*units))};
}

inline StereoViewConstants camcorder_object_constants(EyePose screen,
    const std::array<EyePose,2>& eyes,const std::array<EyeFov,2>& fovs,
    float near_m,float far_m,float native_depth_scale,Vec3 size={1,1,1}) {
    const auto checked=camcorder_offset(screen,{0,0,0});
    const auto right=rotate(checked.orientation,{1,0,0}),up=rotate(checked.orientation,{0,1,0});
    const auto back=rotate(checked.orientation,{0,0,1});
    Matrix4 plane{};
    const float x[]{right.x,right.y,right.z},y[]{up.x,up.y,up.z};
    const float z[]{back.x,back.y,back.z};
    const float center[]{screen.position.x,screen.position.y,screen.position.z};
    for (unsigned row=0;row<3;++row) {
        plane[row*4]=x[row]*size.x;
        plane[row*4+1]=y[row]*size.y;
        plane[row*4+2]=z[row]*size.z;
        plane[row*4+3]=center[row];
    }
    plane[15]=1;
    const TrackingFrame tracking{{0,0,0},{1,0,0},{0,1,0},{0,0,1},1,{0,0,0}};
    StereoViewConstants result{};
    for (unsigned eye=0;eye<2;++eye) {
        result[eye]=eye_constants(tracking,eyes[eye],fovs[eye],near_m,far_m,native_depth_scale);
        Matrix4 view{};std::copy_n(result[eye].begin(),16,view.begin());
        const auto transform=matrix_product(view,plane);
        std::copy(transform.begin(),transform.end(),result[eye].begin());
    }
    return result;
}

inline StereoViewConstants camcorder_screen_constants(EyePose screen,
    const std::array<EyePose,2>& eyes,const std::array<EyeFov,2>& fovs,
    float near_m=.005f,float far_m=1000,float native_depth_scale=0) {
    return camcorder_object_constants(screen,eyes,fovs,near_m,far_m,native_depth_scale,
        {.045f,.0253125f,1});
}

class OutlastCamcorder {
    std::array<bool,2> squeeze_{{true,true}};
    int hand_=-1;
    bool pending_=false,sent_=false,blocked_=false;
    UINT64 sent_at_=0,last_clock_=0;
public:
    CamcorderFrame update(const MotionFrame& motion,EyePose head,Quaternion torso_yaw,
        CamcorderNativeState native,UINT64 now,std::optional<EyePose> native_holster=std::nullopt) {
        if (last_clock_ && now<last_clock_)
            throw std::invalid_argument("Camcorder input clock moved backward");
        last_clock_=now;
        CamcorderFrame result{};
        result.pose=native_holster?camcorder_offset(*native_holster,{0,0,0}):camcorder_holster(head,torso_yaw);
        std::array<bool,2> edge{};
        for (unsigned hand=0;hand<2;++hand) {
            const float value=motion.hands[hand].squeeze;
            if (!std::isfinite(value) || value<0 || value>1)
                throw std::invalid_argument("Invalid camcorder grip action");
            const bool tracked=motion.focused && motion.hands[hand].grip_tracked;
            const bool down=tracked && value>(squeeze_[hand]?.45f:.55f);
            edge[hand]=down && !squeeze_[hand];
            // An untracked/focus-lost hand must release before acquiring again.
            squeeze_[hand]=tracked?down:true;
        }
        const bool eligible=motion.focused && native.available && native.controls_allowed;
        if (!eligible || (hand_>=0 && !squeeze_[unsigned(hand_)])) hand_=-1;
        if (hand_>=0 && !motion.hands[unsigned(hand_)].grip_tracked) hand_=-1;
        if (!eligible) {
            pending_=false;blocked_=false;
            return result;
        }
        if (hand_<0) {
            float nearest=.18f*.18f;
            for (unsigned hand=0;hand<2;++hand) {
                if (!edge[hand]) continue;
                const auto& p=motion.hands[hand].grip.position;
                const Vec3 delta=add({p.x,p.y,p.z},scale(result.pose.position,-1));
                if (!finite(delta)) throw std::invalid_argument("Nonfinite camcorder grab position");
                const float distance=dot(delta,delta);
                if (distance<nearest) {hand_=int(hand);nearest=distance;}
            }
        }
        if (pending_ && !native.transitioning && native.raised==sent_) pending_=false;
        if (pending_ && now-sent_at_>2000000 && !native.transitioning) {
            pending_=false;blocked_=true;hand_=-1;result.request_rejected=true;
        }
        if (edge[0] || edge[1]) blocked_=false;
        const bool desired=hand_>=0;
        if (!pending_ && !blocked_ && !native.transitioning && desired!=native.raised) {
            pending_=true;sent_=desired;sent_at_=now;result.request_toggle=true;
        }
        result.visible=true;
        result.held=desired;
        result.hand=hand_;
        if (desired) {
            const auto& hand=motion.hands[unsigned(hand_)];
            result.pose=camcorder_grip(hand.grip,unsigned(hand_));
            if (hand.aim_tracked) {
                const auto& q=hand.aim.orientation;
                result.pose.orientation=camcorder_offset({result.pose.position,{q.x,q.y,q.z,q.w}},{0,0,0}).orientation;
            }
        }
        result.lens=camcorder_offset(result.pose,{0,.015f,-.09f});
        result.screen=camcorder_offset(result.pose,{-.075f,.015f,.015f});
        return result;
    }
};

}
