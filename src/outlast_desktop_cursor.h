#pragma once
#include <windows.h>
#include <dinput.h>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <mutex>
#include <optional>
#include <cmath>

namespace olvr {
inline POINT outlast_menu_source_point(float u,float v,UINT width,UINT height) {
    if (!std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1 ||
        !width || !height || width>UINT(std::numeric_limits<SHORT>::max())+1 ||
        height>UINT(std::numeric_limits<SHORT>::max())+1)
        throw std::invalid_argument("Invalid native menu source coordinates");
    return {LONG(std::lround(u*(width-1))),LONG(std::lround(v*(height-1)))};
}
class ControllerMenuPress {
    unsigned hand_=2;
    bool armed_=false,down_=false;
public:
    bool update(bool enabled,bool hit,unsigned hand,float trigger) {
        if (hand>1 || !std::isfinite(trigger) || trigger<0 || trigger>1)
            throw std::invalid_argument("Invalid tracked menu trigger");
        if (!enabled) {hand_=2;armed_=false;down_=false;return false;}
        if (hand_!=hand) {hand_=hand;armed_=false;down_=false;}
        if (trigger<=.4f) {armed_=true;down_=false;}
        else if (trigger>=.6f && armed_) {armed_=false;down_=hit;}
        else if (!hit) down_=false;
        return down_;
    }
};
inline bool outlast_desktop_mouse(REFGUID device) {
    constexpr GUID mouse{0x6f1d2b60,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0}};
    return IsEqualGUID(device,mouse)!=FALSE;
}
inline DWORD outlast_mouse_cooperation(DWORD flags) {
    return (flags&~DWORD(DISCL_EXCLUSIVE))|DISCL_NONEXCLUSIVE;
}
class OutlastDesktopCursor {
    std::uintptr_t base_,end_;
    mutable std::mutex mutex_;
    POINT menu_point_{};
    bool menu_active_=false;
public:
    using Clip=BOOL(WINAPI*)(const RECT*);
    using Position=BOOL(WINAPI*)(int,int);
    using GetPosition=BOOL(WINAPI*)(POINT*);
    OutlastDesktopCursor(std::uintptr_t base,std::size_t extent):base_(base),end_(base) {
        if (!base || !extent || extent>std::numeric_limits<std::uintptr_t>::max()-base)
            throw std::invalid_argument("Desktop cursor policy requires a bounded native image");
        end_+=extent;
    }
    bool owns(std::uintptr_t caller) const {return caller>=base_ && caller<end_;}
    void menu_pointer(bool active,POINT point={}) {
        std::lock_guard<std::mutex> lock(mutex_);
        menu_active_=active;menu_point_=point;
    }
    std::optional<POINT> menu_position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return menu_active_?std::optional<POINT>(menu_point_):std::nullopt;
    }
    BOOL get_position(GetPosition original,std::uintptr_t caller,POINT* point) const {
        if (!original) throw std::invalid_argument("Missing original cursor query entry");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (menu_active_ && owns(caller) && point) {*point=menu_point_;return TRUE;}
        }
        return original(point);
    }
    BOOL clip(Clip original,std::uintptr_t caller,const RECT* rectangle) const {
        if (!original) throw std::invalid_argument("Missing original cursor confinement entry");
        if (rectangle && owns(caller)) return TRUE;
        return original(rectangle);
    }
    BOOL position(Position original,std::uintptr_t caller,int x,int y) const {
        if (!original) throw std::invalid_argument("Missing original cursor positioning entry");
        if (owns(caller)) return TRUE;
        return original(x,y);
    }
};
}
