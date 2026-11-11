#pragma once
#include "openxr_motion_input.h"
#include "outlast_config.h"
#include <xinput.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>

namespace olvr {

class OutlastMotionControls {
    mutable std::mutex mutex_;
    MotionFrame frame_{};
    UINT64 sampled_at_=0;
    DWORD packet_=0;
    XINPUT_GAMEPAD previous_{};
    bool use_=false,use_blocked_=false;
    XINPUT_VIBRATION vibration_{};
    bool physical_desired_=false,physical_owned_=false,stance_pending_=false,stance_sent_=false,stance_pulse_=false;
    UINT64 stance_started_=0;
    unsigned stance_attempts_=0;
    bool stance_blocked_=false,stance_warning_=false;
    bool manual_down_=false,manual_pulse_=false,manual_desired_=false,stance_controlled_=false;
    bool manual_blocked_=true;
    bool stance_eligible_=false;
    OutlastComfortSettings options_;
    bool recenter_blocked_=true,recenter_down_=false,recenter_pending_=false,recenter_consuming_=false;
    UINT64 recenter_started_=0;
    std::optional<float> movement_yaw_=0.f;
    bool sprint_down_=false,sprinting_=false,sprint_blocked_=true;
    static SHORT axis(float value) {
        return SHORT(std::lround(value*(value<0?32768.f:32767.f)));
    }
    static bool threshold(float value,bool previous) {
        return value>(previous?.45f:.55f);
    }
    void cancel_stance_pulse() {
        if (!stance_pulse_) return;
        stance_pulse_=false;stance_pending_=false;
        if (stance_attempts_) --stance_attempts_;
    }
public:
    void configure(const OutlastComfortSettings& options) {
        std::lock_guard<std::mutex> lock(mutex_);
        options_=options;
    }
    void movement_yaw(std::optional<float> yaw,bool gameplay=true) {
        if (yaw && !std::isfinite(*yaw)) throw std::invalid_argument("Nonfinite movement heading");
        std::lock_guard<std::mutex> lock(mutex_);movement_yaw_=yaw;
        if (!yaw || !gameplay) {sprinting_=false;sprint_blocked_=true;}
    }
    bool take_recenter(UINT64 now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool requested=recenter_pending_ && frame_.focused &&
            now>=sampled_at_ && now-sampled_at_<=250000;
        recenter_pending_=false;return requested;
    }
    void publish(const MotionFrame& frame,UINT64 now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool interrupted=!frame.focused || now<sampled_at_ || now-sampled_at_>250000;
        if (interrupted) {
            vibration_={};
            cancel_stance_pulse();manual_pulse_=false;manual_blocked_=true;
            recenter_blocked_=true;recenter_pending_=false;recenter_down_=false;
            sprint_blocked_=true;
        }
        const auto primary=options_.primary_hand();
        if (interrupted || !frame.hands[primary].aim_tracked) use_blocked_=true;
        frame_=frame.focused?frame:MotionFrame{};
        const bool chord=frame_.hands[0].stick_click && frame_.hands[1].stick_click;
        const bool any_click=frame_.hands[0].stick_click || frame_.hands[1].stick_click;
        if (frame_.focused && !any_click) {
            recenter_blocked_=false;recenter_down_=false;recenter_consuming_=false;
        }
        else if (chord) {
            recenter_consuming_=true;
            frame_.hands[0].stick_click=frame_.hands[1].stick_click=false;
            if (!recenter_blocked_ && !recenter_down_) {recenter_started_=now;recenter_down_=true;}
            if (!recenter_blocked_ && recenter_down_ && now>=recenter_started_ &&
                now-recenter_started_>=1000000) {
                recenter_pending_=true;recenter_blocked_=true;
            }
        }
        else if (recenter_consuming_ && any_click) {
            frame_.hands[0].stick_click=frame_.hands[1].stick_click=false;
            recenter_blocked_=true;recenter_down_=false;
        }
        else if (frame_.focused) {recenter_blocked_=false;recenter_down_=false;}
        const auto move=options_.left_handed?frame_.turn:frame_.move;
        const bool sprint=frame_.hands[1-primary].stick_click;
        if (chord) sprint_blocked_=true;
        else if (frame_.focused && !sprint) sprint_blocked_=false;
        if (interrupted || chord || move.x*move.x+move.y*move.y<.01f) sprinting_=false;
        else if (sprint && !sprint_down_ && !sprint_blocked_) sprinting_=!sprinting_;
        sprint_down_=sprint;
        sampled_at_=now;
        if (!frame_.focused) {stance_pulse_=false;manual_pulse_=false;stance_eligible_=false;}
        else if (!frame_.hands[primary].secondary) manual_blocked_=false;
        else if (!manual_down_ && !manual_blocked_) manual_pulse_=true;
        manual_down_=frame_.hands[primary].secondary;
        use_=threshold(frame_.hands[primary].trigger,use_);
        if (frame_.focused && frame_.hands[primary].aim_tracked && frame_.hands[primary].trigger<=.45f)
            use_blocked_=false;
    }
    MotionFrame sample(UINT64 now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (now<sampled_at_ || now-sampled_at_>250000) return {};
        return frame_;
    }
    DWORD vibration(DWORD index,const XINPUT_VIBRATION* value,DWORD physical_result,UINT64 now) {
        if (index!=0) return physical_result;
        if (!value) return ERROR_BAD_ARGUMENTS;
        if (physical_result!=ERROR_SUCCESS && physical_result!=ERROR_DEVICE_NOT_CONNECTED)
            return physical_result;
        std::lock_guard<std::mutex> lock(mutex_);
        vibration_=frame_.focused && now>=sampled_at_ && now-sampled_at_<=250000?
            *value:XINPUT_VIBRATION{};
        return ERROR_SUCCESS;
    }
    std::array<float,2> rumble(UINT64 now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::array<float,2> result{};
        if (!frame_.focused || now<sampled_at_ || now-sampled_at_>250000) return result;
        // XInput motors are low/high frequency, not spatial left/right hands.
        const float amplitude=std::max(vibration_.wLeftMotorSpeed,vibration_.wRightMotorSpeed)/65535.f;
        for (unsigned hand=0;hand<2;++hand)
            if (frame_.hands[hand].grip_tracked) result[hand]=amplitude;
        return result;
    }
    bool physical_stance(bool eligible,float height_delta,bool native_crouched,bool transitioning,UINT64 now) {
        std::lock_guard<std::mutex> lock(mutex_);
        stance_eligible_=eligible && frame_.focused && now>=sampled_at_ && now-sampled_at_<=250000;
        if (!stance_eligible_) {
            cancel_stance_pulse();
            return false;
        }
        if (!std::isfinite(height_delta)) throw std::runtime_error("Nonfinite physical crouch height");
        const bool previous=physical_desired_;
        if (height_delta<-.35f) physical_desired_=true;
        else if (height_delta>-.2f) physical_desired_=false;
        if (!stance_controlled_ && !transitioning) manual_desired_=native_crouched;
        const bool manual_changed=manual_pulse_;
        if (manual_changed) {
            manual_desired_=!manual_desired_;
            manual_pulse_=false;
            stance_controlled_=true;
        }
        if (physical_desired_) {
            stance_controlled_=true;
            physical_owned_=true;
        }
        const bool desired=physical_desired_ || manual_desired_;
        if (previous!=physical_desired_ || manual_changed) {
            stance_attempts_=0;stance_blocked_=false;
        }
        if (stance_sent_!=desired) cancel_stance_pulse();
        if (stance_pending_ && native_crouched==stance_sent_ && !transitioning) {
            stance_pending_=false;stance_pulse_=false;stance_attempts_=0;stance_blocked_=false;
        }
        if (stance_pending_ && !transitioning &&
            (now<stance_started_ || now-stance_started_>1500000)) {
            stance_pending_=false;
            if (stance_attempts_>=2) {stance_blocked_=true;stance_warning_=true;}
        }
        if (stance_controlled_ && !transitioning && !stance_pending_ && !stance_blocked_ &&
            desired!=native_crouched) {
            stance_pulse_=true;stance_pending_=true;stance_sent_=desired;
            stance_started_=now;++stance_attempts_;
        }
        if (!physical_desired_ && !transitioning && !stance_pending_ && desired==native_crouched) {
            physical_owned_=false;
            if (!manual_desired_) stance_controlled_=false;
        }
        return physical_owned_;
    }
    bool take_stance_warning() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool result=stance_warning_;stance_warning_=false;return result;
    }
    DWORD state(DWORD index,XINPUT_STATE* output,DWORD physical_result,
                const XINPUT_STATE& physical,UINT64 now,bool menu_pointer=false,bool use_ready=true,
                bool native_night_vision=true) {
        if (index!=0) {
            if (output && physical_result==ERROR_SUCCESS) *output=physical;
            return physical_result;
        }
        if (!output) return ERROR_BAD_ARGUMENTS;
        if (physical_result!=ERROR_SUCCESS && physical_result!=ERROR_DEVICE_NOT_CONNECTED)
            return physical_result;
        std::lock_guard<std::mutex> lock(mutex_);
        // A suppressed or interrupted press must be released before gameplay can own it.
        if (menu_pointer) {
            sprinting_=false;sprint_blocked_=true;
            stance_eligible_=false;cancel_stance_pulse();
        }
        if ((menu_pointer || !use_ready) && use_) use_blocked_=true;
        XINPUT_STATE result{};
        if (physical_result==ERROR_SUCCESS) result=physical;
        if (frame_.focused && now>=sampled_at_ && now-sampled_at_<=250000) {
            const auto& left=frame_.hands[1-options_.primary_hand()];
            const auto& right=frame_.hands[options_.primary_hand()];
            auto move=options_.left_handed?frame_.turn:frame_.move;
            const auto turn=options_.left_handed?frame_.move:frame_.turn;
            if (!menu_pointer) {
                if (movement_yaw_) {
                    const float c=std::cos(*movement_yaw_),s=std::sin(*movement_yaw_);
                    move={c*move.x-s*move.y,s*move.x+c*move.y};
                    const float limit=std::max({1.f,std::abs(move.x),std::abs(move.y)});
                    move.x/=limit;move.y/=limit;
                } else move={};
            }
            auto& pad=result.Gamepad;
            auto button=[&](bool down,WORD bit){if (down) pad.wButtons|=bit;};
            button(right.primary,XINPUT_GAMEPAD_A);
            // In gameplay the stance reconciler owns this edge. Native polling
            // can precede the render callback; consuming it here loses intent.
            if (!stance_eligible_) {
                button(manual_pulse_,XINPUT_GAMEPAD_B);
                manual_pulse_=false;
            }
            button(stance_pulse_,XINPUT_GAMEPAD_B);
            stance_pulse_=false;
            button(use_ && !use_blocked_ && right.aim_tracked,XINPUT_GAMEPAD_X);
            button(left.secondary,XINPUT_GAMEPAD_Y);
            button(options_.toggle_sprint?sprinting_:left.stick_click,XINPUT_GAMEPAD_LEFT_SHOULDER);
            button(left.primary,XINPUT_GAMEPAD_BACK);
            button(native_night_vision && right.stick_click,XINPUT_GAMEPAD_RIGHT_THUMB);
            button(frame_.menu,XINPUT_GAMEPAD_START);
            button(turn.y>.55f,XINPUT_GAMEPAD_DPAD_UP);
            button(turn.y<-.55f,XINPUT_GAMEPAD_DPAD_DOWN);
            if (std::abs(move.x)>.05f) pad.sThumbLX=axis(std::clamp(move.x,-1.f,1.f));
            if (std::abs(move.y)>.05f) pad.sThumbLY=axis(std::clamp(move.y,-1.f,1.f));
            if ((!options_.snap_turn_degrees || menu_pointer) && std::abs(turn.x)>.05f)
                pad.sThumbRX=axis(turn.x);
        }
        if (std::memcmp(&previous_,&result.Gamepad,sizeof(previous_))!=0) {
            previous_=result.Gamepad;
            ++packet_;
        }
        result.dwPacketNumber=packet_;
        *output=result;
        return ERROR_SUCCESS;
    }
};

inline bool outlast_virtual_pad_presence(void* data,INT count) {
    if (count<0 || count>256 || (count && !data))
        throw std::runtime_error("Invalid native controller inventory");
    if (!count) return false;
    const auto* bytes=static_cast<const unsigned char*>(data);
    void* direct_input=nullptr;
    INT kind=0;
    std::memcpy(&direct_input,bytes,sizeof(direct_input));
    std::memcpy(&kind,bytes+8,sizeof(kind));
    if (direct_input || kind!=3)
        throw std::runtime_error("Native controller zero is not the recovered XInput pad");
    const UINT connected=1;
    std::memcpy(static_cast<unsigned char*>(data)+0x1f0,&connected,sizeof(connected));
    return true;
}

}
