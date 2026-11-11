#pragma once
#include <windows.h>
#include "openxr_function.h"
#include <array>
#include <cmath>
#include <vector>

namespace olvr {
struct MotionHand {
    XrPosef grip{{0,0,0,1},{0,0,0}},aim{{0,0,0,1},{0,0,0}};
    bool grip_tracked=false,aim_tracked=false;
    float trigger=0,squeeze=0;
    bool primary=false,secondary=false,stick_click=false;
};

struct MotionFrame {
    XrTime time=0;
    bool focused=false,menu=false;
    XrVector2f move{},turn{};
    std::array<MotionHand,2> hands{};
};

class OpenXRMotionInput {
    XrInstance instance_;
    XrSession session_;
    XrActionSet set_=XR_NULL_HANDLE;
    std::array<XrPath,2> hands_{};
    std::array<XrSpace,2> grips_{},aims_{};
    XrAction grip_=XR_NULL_HANDLE,aim_=XR_NULL_HANDLE;
    XrAction trigger_=XR_NULL_HANDLE,squeeze_=XR_NULL_HANDLE;
    XrAction primary_=XR_NULL_HANDLE,secondary_=XR_NULL_HANDLE,stick_=XR_NULL_HANDLE;
    XrAction move_=XR_NULL_HANDLE,turn_=XR_NULL_HANDLE,menu_=XR_NULL_HANDLE;
    XrAction haptic_=XR_NULL_HANDLE;
    mutable std::array<bool,2> vibrating_{};
    enum class ClickGesture {none,primary,secondary,center,blocked};
    mutable std::array<ClickGesture,2> click_gestures_{};
    mutable bool pause_consuming_=false,pause_armed_=true;
    mutable std::array<bool,2> pause_clicks_{};
#define OLVR_MOTION_FUNCTIONS(F) \
    F(StringToPath) F(CreateActionSet) F(DestroyActionSet) F(CreateAction) \
    F(CreateActionSpace) F(DestroySpace) F(SuggestInteractionProfileBindings) \
    F(AttachSessionActionSets) F(SyncActions) F(GetActionStatePose) \
    F(GetActionStateFloat) F(GetActionStateBoolean) F(GetActionStateVector2f) \
    F(LocateSpace) F(ApplyHapticFeedback) F(StopHapticFeedback)
#define OLVR_MOTION_DECLARE(name) PFN_xr##name name##_ = nullptr;
    OLVR_MOTION_FUNCTIONS(OLVR_MOTION_DECLARE)
#undef OLVR_MOTION_DECLARE

    XrPath path(const char* value) const {
        XrPath result=XR_NULL_PATH;
        xr_check(StringToPath_(instance_,value,&result),"Resolve controller binding path");
        if (!result) throw std::runtime_error("OpenXR returned an empty controller path");
        return result;
    }
    XrAction action(const char* name,const char* label,XrActionType type) {
        XrActionCreateInfo info{};
        info.type=XR_TYPE_ACTION_CREATE_INFO;
        std::strcpy(info.actionName,name);std::strcpy(info.localizedActionName,label);
        info.actionType=type;info.countSubactionPaths=2;info.subactionPaths=hands_.data();
        XrAction result=XR_NULL_HANDLE;
        xr_check(CreateAction_(set_,&info,&result),"Create controller action");
        if (!result) throw std::runtime_error("OpenXR returned an empty controller action");
        return result;
    }
    XrSpace pose_space(XrAction action,XrPath hand) {
        XrActionSpaceCreateInfo info{};
        info.type=XR_TYPE_ACTION_SPACE_CREATE_INFO;info.action=action;
        info.subactionPath=hand;info.poseInActionSpace.orientation.w=1;
        XrSpace result=XR_NULL_HANDLE;
        xr_check(CreateActionSpace_(session_,&info,&result),"Create tracked controller space");
        if (!result) throw std::runtime_error("OpenXR returned an empty controller space");
        return result;
    }
    XrActionStateGetInfo query(XrAction action,unsigned hand) const {
        XrActionStateGetInfo info{};
        info.type=XR_TYPE_ACTION_STATE_GET_INFO;info.action=action;
        info.subactionPath=hands_.at(hand);
        return info;
    }
    float scalar(XrAction action,unsigned hand) const {
        const auto info=query(action,hand);
        XrActionStateFloat state{};state.type=XR_TYPE_ACTION_STATE_FLOAT;
        xr_check(GetActionStateFloat_(session_,&info,&state),"Read analog controller action");
        if (!state.isActive) return 0;
        if (!std::isfinite(state.currentState) || state.currentState<0 || state.currentState>1)
            throw std::runtime_error("Invalid active controller trigger/squeeze value");
        return state.currentState;
    }
    bool button(XrAction action,unsigned hand,bool* available=nullptr) const {
        const auto info=query(action,hand);
        XrActionStateBoolean state{};state.type=XR_TYPE_ACTION_STATE_BOOLEAN;
        xr_check(GetActionStateBoolean_(session_,&info,&state),"Read controller button");
        if (available) *available=state.isActive!=XR_FALSE;
        return state.isActive && state.currentState;
    }
    XrVector2f stick(XrAction action,unsigned hand) const {
        const auto info=query(action,hand);
        XrActionStateVector2f state{};state.type=XR_TYPE_ACTION_STATE_VECTOR2F;
        xr_check(GetActionStateVector2f_(session_,&info,&state),"Read controller stick");
        if (!state.isActive) return {};
        if (!std::isfinite(state.currentState.x) || !std::isfinite(state.currentState.y) ||
            std::abs(state.currentState.x)>1 || std::abs(state.currentState.y)>1)
            throw std::runtime_error("Invalid active controller stick value");
        return state.currentState;
    }
    bool pose(XrAction action,XrSpace space,unsigned hand,XrSpace base,XrTime time,XrPosef& result) const {
        const auto info=query(action,hand);
        XrActionStatePose state{};state.type=XR_TYPE_ACTION_STATE_POSE;
        xr_check(GetActionStatePose_(session_,&info,&state),"Read controller pose activity");
        if (!state.isActive) return false;
        XrSpaceLocation location{};location.type=XR_TYPE_SPACE_LOCATION;
        xr_check(LocateSpace_(space,base,time,&location),"Locate controller at predicted display time");
        // VALID includes usable runtime estimates during temporary optical occlusion.
        constexpr XrSpaceLocationFlags required=XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags&required)!=required) return false;
        const auto& p=location.pose.position;const auto& q=location.pose.orientation;
        const double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(norm) || std::abs(norm-1)>.01)
            throw std::runtime_error("Invalid usable controller pose");
        result=location.pose;
        const auto length=float(std::sqrt(norm));
        result.orientation={q.x/length,q.y/length,q.z/length,q.w/length};
        return true;
    }
    void cleanup() noexcept {
        auto checked=[](XrResult result,const char* name) {
            if (XR_FAILED(result)) {
                const auto text=std::string(name)+" XrResult="+std::to_string(result);
                OutputDebugStringA(text.c_str());
            }
        };
        for (unsigned hand=0;hand<2;++hand) if (vibrating_[hand]) {
            XrHapticActionInfo action{};
            action.type=XR_TYPE_HAPTIC_ACTION_INFO;action.action=haptic_;action.subactionPath=hands_[hand];
            checked(StopHapticFeedback_(session_,&action),"Stop controller vibration during cleanup");
            vibrating_[hand]=false;
        }
        for (auto& space:grips_) if (space) {
            checked(DestroySpace_(space),"Destroy controller grip space");space=XR_NULL_HANDLE;
        }
        for (auto& space:aims_) if (space) {
            checked(DestroySpace_(space),"Destroy controller aim space");space=XR_NULL_HANDLE;
        }
        if (set_) {checked(DestroyActionSet_(set_),"Destroy controller action set");set_=XR_NULL_HANDLE;}
    }
public:
    OpenXRMotionInput(PFN_xrGetInstanceProcAddr resolve,XrInstance instance,XrSession session)
        :instance_(instance),session_(session) {
        if (!instance || !session) throw std::invalid_argument("Controller input requires a live XR session");
        try {
#define OLVR_MOTION_LOAD(name) xr_load_function(resolve,instance_,"xr" #name,name##_);
            OLVR_MOTION_FUNCTIONS(OLVR_MOTION_LOAD)
#undef OLVR_MOTION_LOAD
            hands_={path("/user/hand/left"),path("/user/hand/right")};
            XrActionSetCreateInfo info{};info.type=XR_TYPE_ACTION_SET_CREATE_INFO;
            std::strcpy(info.actionSetName,"outlast_motion");
            std::strcpy(info.localizedActionSetName,"Outlast motion controls");
            xr_check(CreateActionSet_(instance_,&info,&set_),"Create Outlast motion action set");
            if (!set_) throw std::runtime_error("OpenXR returned an empty action set");
            grip_=action("grip_pose","Tracked grip",XR_ACTION_TYPE_POSE_INPUT);
            aim_=action("aim_pose","Tracked aim",XR_ACTION_TYPE_POSE_INPUT);
            trigger_=action("trigger","Trigger",XR_ACTION_TYPE_FLOAT_INPUT);
            squeeze_=action("squeeze","Grip squeeze",XR_ACTION_TYPE_FLOAT_INPUT);
            primary_=action("primary","Primary button",XR_ACTION_TYPE_BOOLEAN_INPUT);
            secondary_=action("secondary","Secondary button",XR_ACTION_TYPE_BOOLEAN_INPUT);
            stick_=action("stick_click","Stick click",XR_ACTION_TYPE_BOOLEAN_INPUT);
            move_=action("move","Move",XR_ACTION_TYPE_VECTOR2F_INPUT);
            turn_=action("turn","Turn",XR_ACTION_TYPE_VECTOR2F_INPUT);
            menu_=action("menu","Menu",XR_ACTION_TYPE_BOOLEAN_INPUT);
            haptic_=action("haptic","Haptic feedback",XR_ACTION_TYPE_VIBRATION_OUTPUT);
            struct Profile {const char* name;const char* axis;const char* squeeze;bool face;};
            const Profile profiles[]{
                {"/interaction_profiles/oculus/touch_controller","thumbstick","value",true},
                {"/interaction_profiles/valve/index_controller","thumbstick","force",true},
                {"/interaction_profiles/microsoft/motion_controller","thumbstick","click",false},
                {"/interaction_profiles/htc/vive_controller","trackpad","click",false}};
            unsigned accepted=0;
            for (const auto& profile:profiles) {
                std::vector<XrActionSuggestedBinding> bindings;
                for (unsigned hand=0;hand<2;++hand) {
                    const auto root=std::string("/user/hand/")+(hand?"right":"left");
                    auto bind=[&](XrAction action,const std::string& component) {
                        bindings.push_back({action,path((root+component).c_str())});
                    };
                    bind(grip_,"/input/grip/pose");bind(aim_,"/input/aim/pose");
                    bind(trigger_,"/input/trigger/value");
                    bind(squeeze_,std::string("/input/squeeze/")+profile.squeeze);
                    bind(haptic_,"/output/haptic");
                    bind(hand?turn_:move_,std::string("/input/")+profile.axis);
                    bind(stick_,std::string("/input/")+profile.axis+"/click");
                    if (profile.face) {
                        bind(primary_,hand || std::strcmp(profile.name,"/interaction_profiles/valve/index_controller")==0?
                            "/input/a/click":"/input/x/click");
                        bind(secondary_,hand || std::strcmp(profile.name,"/interaction_profiles/valve/index_controller")==0?
                            "/input/b/click":"/input/y/click");
                    }
                    if (!hand && std::strcmp(profile.name,"/interaction_profiles/valve/index_controller")!=0)
                        bind(menu_,"/input/menu/click");
                }
                XrInteractionProfileSuggestedBinding suggestion{};
                suggestion.type=XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
                suggestion.interactionProfile=path(profile.name);
                suggestion.countSuggestedBindings=uint32_t(bindings.size());
                suggestion.suggestedBindings=bindings.data();
                const auto result=SuggestInteractionProfileBindings_(instance_,&suggestion);
                if (result==XR_ERROR_PATH_UNSUPPORTED) {
                    OutputDebugStringA((std::string("Unsupported optional controller profile: ")+profile.name).c_str());
                    continue;
                }
                xr_check(result,"Suggest controller profile bindings");
                ++accepted;
            }
            if (!accepted) throw std::runtime_error("Runtime accepted no supported motion-controller profile");
            for (unsigned hand=0;hand<2;++hand) {
                grips_[hand]=pose_space(grip_,hands_[hand]);aims_[hand]=pose_space(aim_,hands_[hand]);
            }
            XrSessionActionSetsAttachInfo attach{};attach.type=XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
            attach.countActionSets=1;attach.actionSets=&set_;
            xr_check(AttachSessionActionSets_(session_,&attach),"Attach Outlast motion controls");
        } catch (...) {cleanup();throw;}
    }
    ~OpenXRMotionInput(){cleanup();}
    OpenXRMotionInput(const OpenXRMotionInput&)=delete;
    OpenXRMotionInput& operator=(const OpenXRMotionInput&)=delete;

    MotionFrame sample(XrSpace base,XrTime time) const {
        if (!base || time<=0) throw std::invalid_argument("Controller sampling requires a predicted XR time and reference space");
        MotionFrame frame{};frame.time=time;
        XrActiveActionSet active{set_,XR_NULL_PATH};
        XrActionsSyncInfo sync{};sync.type=XR_TYPE_ACTIONS_SYNC_INFO;
        sync.countActiveActionSets=1;sync.activeActionSets=&active;
        const auto result=SyncActions_(session_,&sync);
        if (result==XR_SESSION_NOT_FOCUSED) {
            suspend();return frame;
        }
        xr_check(result,"Synchronize motion controls");
        frame.focused=true;frame.move=stick(move_,0);frame.turn=stick(turn_,1);
        bool menu_available=false;
        frame.menu=button(menu_,0,&menu_available);
        std::array<bool,2> secondary_down{},secondary_clicks{};
        for (unsigned hand=0;hand<2;++hand) {
            auto& value=frame.hands[hand];
            value.grip_tracked=pose(grip_,grips_[hand],hand,base,time,value.grip);
            value.aim_tracked=pose(aim_,aims_[hand],hand,base,time,value.aim);
            value.trigger=scalar(trigger_,hand);value.squeeze=scalar(squeeze_,hand);
            bool primary_available=false,secondary_available=false;
            value.primary=button(primary_,hand,&primary_available);
            value.secondary=button(secondary_,hand,&secondary_available);
            bool click_available=false;
            value.stick_click=button(stick_,hand,&click_available);
            const bool raw_click=value.stick_click;
            auto& gesture=click_gestures_[hand];
            auto& axis=hand?frame.turn:frame.move;
            const bool fallback=!primary_available && !secondary_available;
            if (!click_available) gesture=ClickGesture::blocked;
            else if (!value.stick_click) gesture=ClickGesture::none;
            else if (fallback && gesture==ClickGesture::none) {
                gesture=std::abs(axis.y)>.55f && std::abs(axis.y)>std::abs(axis.x)?
                    (axis.y>0?ClickGesture::primary:ClickGesture::secondary):ClickGesture::center;
            }
            else if (!fallback && (gesture==ClickGesture::primary || gesture==ClickGesture::secondary))
                gesture=ClickGesture::blocked;
            if (value.stick_click && gesture!=ClickGesture::none && gesture!=ClickGesture::center) {
                if (gesture==ClickGesture::primary) value.primary=true;
                if (gesture==ClickGesture::secondary) value.secondary=true;
                value.stick_click=false;axis={};
            }
            secondary_clicks[hand]=gesture==ClickGesture::secondary;
            secondary_down[hand]=pause_consuming_ && (pause_clicks_[hand] || !secondary_available)?
                !click_available || raw_click:value.secondary;
            if (!value.grip_tracked) stop_vibration(hand);
        }
        const bool both_secondary=frame.hands[0].secondary && frame.hands[1].secondary;
        const bool any_secondary=secondary_down[0] || secondary_down[1];
        if (!any_secondary) {pause_consuming_=false;pause_armed_=true;}
        else if (pause_consuming_ || (!menu_available && both_secondary)) {
            if (!pause_consuming_) pause_clicks_=secondary_clicks;
            pause_consuming_=true;
            if (!both_secondary) pause_armed_=false;
            if (!menu_available && both_secondary && pause_armed_) frame.menu=true;
            frame.hands[0].secondary=frame.hands[1].secondary=false;
            for (unsigned hand=0;hand<2;++hand) if (pause_clicks_[hand]) {
                frame.hands[hand].primary=frame.hands[hand].stick_click=false;
                if (secondary_down[hand]) (hand?frame.turn:frame.move)={};
            }
        }
        return frame;
    }
    void suspend() const {
        for (auto& gesture:click_gestures_)
            if (gesture!=ClickGesture::none) gesture=ClickGesture::blocked;
        if (pause_consuming_) pause_armed_=false;
        stop_vibrations();
    }
    void vibrate(unsigned hand,float amplitude,XrDuration duration,float frequency=XR_FREQUENCY_UNSPECIFIED) const {
        if (hand>=2 || !std::isfinite(amplitude) || amplitude<0 || amplitude>1 ||
            duration<=0 || duration>1000000000 || !std::isfinite(frequency) || frequency<0)
            throw std::invalid_argument("Invalid bounded controller haptic request");
        XrHapticActionInfo action{};action.type=XR_TYPE_HAPTIC_ACTION_INFO;
        action.action=haptic_;action.subactionPath=hands_[hand];
        XrHapticVibration effect{};effect.type=XR_TYPE_HAPTIC_VIBRATION;
        effect.amplitude=amplitude;effect.duration=duration;effect.frequency=frequency;
        xr_check(ApplyHapticFeedback_(session_,&action,
            reinterpret_cast<const XrHapticBaseHeader*>(&effect)),"Apply controller haptic feedback");
        vibrating_[hand]=true;
    }
    void stop_vibration(unsigned hand) const {
        if (hand>=2) throw std::invalid_argument("Invalid controller hand");
        if (!vibrating_[hand]) return;
        XrHapticActionInfo action{};action.type=XR_TYPE_HAPTIC_ACTION_INFO;
        action.action=haptic_;action.subactionPath=hands_[hand];
        xr_check(StopHapticFeedback_(session_,&action),"Stop controller haptic feedback");
        vibrating_[hand]=false;
    }
    void stop_vibrations() const {for (unsigned hand=0;hand<2;++hand) stop_vibration(hand);}
};
#undef OLVR_MOTION_FUNCTIONS
}
