#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <stdexcept>

namespace olvr {
constexpr std::intptr_t native_probe_key_marker=std::intptr_t(1u<<25);
constexpr std::uintptr_t native_probe_mouse_marker=std::uintptr_t(1)<<15;
constexpr unsigned native_probe_gait_version=3;

inline std::array<float,4> native_probe_camera_optical_roll(bool combined_material) {
    return combined_material?std::array<float,4>{0,0,0,1}:std::array<float,4>{0,1,0,0};
}

inline std::array<float,4> native_probe_camera_grip(bool combined_material,float yaw) {
    if (!std::isfinite(yaw)) throw std::invalid_argument("Nonfinite diagnostic camera grip yaw");
    const auto s=std::sin(yaw*.5f),c=std::cos(yaw*.5f);
    if (!combined_material) return {0,s,0,c};
    constexpr float h=.7071067812f;
    return {-c*h,s*h,s*h,c*h};
}

inline float native_probe_camera_yaw(bool combined_material,std::uint64_t world,bool held) {
    return held?(combined_material?.5f:.15f)*std::sin(float(world)*.025f):0.f;
}

inline bool native_probe_body_fixture(bool continue_game,bool gait,bool head_turn,bool combined_material) {
    return continue_game && !gait && (!head_turn || combined_material);
}

struct NativeProbeCameraRegrip {
    int hand=-1;
    float squeeze=0;
    bool approach=false;
};

inline NativeProbeCameraRegrip native_probe_material_regrip(std::uint64_t world) {
    if (world>=322 && world<334) return {1,world>=328?1.f:0.f,true};
    if (world>=398 && world<400) return {0,1,true};
    if (world>=402 && world<412) return {0,world>=406?1.f:0.f,true};
    if (world>=422 && world<434) return {0,world>=428?1.f:0.f,true};
    return {};
}

inline bool native_probe_readback_free_fixture(bool recommended,bool physical_camera,
                                               bool occlusion,bool material,bool selected_draw,
                                               bool material_body_camera=false) {
    if (!recommended) return false;
    if (material_body_camera) return physical_camera && !occlusion && material && selected_draw;
    return (physical_camera && !occlusion && !material && !selected_draw) ||
        (occlusion && !physical_camera && !material && selected_draw) ||
        (material && !physical_camera && !occlusion && selected_draw);
}

inline std::array<float,3> native_probe_window_translation(std::uint64_t world,bool measured_floor) {
    if (world<300 || world>=450)
        throw std::invalid_argument("Window translation requires its declared moving-head interval");
    const float phase=float(world-300);
    return {.45f*std::sin(phase*.07f),(measured_floor?1.7f:0.f)+.25f*std::sin(phase*.11f),
        .45f*std::sin(phase*.05f)};
}

inline std::array<float,4> native_probe_asymmetric_fov(unsigned eye) {
    if (eye>1) throw std::invalid_argument("Diagnostic FOV eye is outside the stereo pair");
    return eye?std::array<float,4>{-.85f,1.f,.9f,-.95f}:
        std::array<float,4>{-1.f,.85f,.9f,-.95f};
}

struct NativeProbeAction {
    std::uint32_t key=0;
    bool down=false;
};

inline NativeProbeAction native_probe_gait_action(std::uint64_t frame,bool lean) {
    switch (frame) {
    case 60:return {'W',true};
    case 80:return {'W',false};
    case 300:return {0xa2,true};
    case 420:return {0xa2,false};
    case 500:if (lean) return {'Q',true};break;
    case 560:if (lean) return {'Q',false};break;
    case 650:if (lean) return {'E',true};break;
    case 710:if (lean) return {'E',false};break;
    }
    return {};
}

inline bool native_probe_input_message(std::uint32_t message,std::intptr_t& bits) {
    if ((message>=0x100 && message<=0x107) || message==0x109) {
        if (!(bits&native_probe_key_marker)) return false;
        bits&=~native_probe_key_marker;
    } else if (message>=0x200 && message<=0x20e) {
        return false;
    }
    return true;
}

class NativeProbeKeys {
    std::array<std::atomic<std::uint64_t>,4> keys_{};
public:
    bool accept_message(std::uint32_t message,std::uintptr_t& key,std::intptr_t& bits) {
        if (message>=0x200 && message<=0x20e) {
            if ((message!=0x200 && message!=0x201 && message!=0x202) ||
                !(key&native_probe_mouse_marker)) return false;
            key&=~native_probe_mouse_marker;
            if (message==0x201) keys_[0].fetch_or(2,std::memory_order_release);
            if (message==0x202) keys_[0].fetch_and(~std::uint64_t(2),std::memory_order_release);
            return true;
        }
        return accept(message,std::uint32_t(key),bits);
    }
    bool accept(std::uint32_t message,std::uint32_t key,std::intptr_t& bits) {
        if (!native_probe_input_message(message,bits)) return false;
        if (message==0x100 || message==0x101 || message==0x104 || message==0x105) {
            if (key>=256) throw std::invalid_argument("Owned probe key exceeds the native keyboard");
            const auto mask=std::uint64_t(1)<<(key%64);
            if (message==0x100 || message==0x104)
                keys_[key/64].fetch_or(mask,std::memory_order_release);
            else
                keys_[key/64].fetch_and(~mask,std::memory_order_release);
        }
        return true;
    }
    bool down(std::uint32_t key) const {
        if (key>=256) throw std::invalid_argument("Probe key query exceeds the native keyboard");
        if (key==0x10) return down(0xa0) || down(0xa1);
        if (key==0x11) return down(0xa2) || down(0xa3);
        if (key==0x12) return down(0xa4) || down(0xa5);
        return (keys_[key/64].load(std::memory_order_acquire)&(std::uint64_t(1)<<(key%64)))!=0;
    }
};
}
