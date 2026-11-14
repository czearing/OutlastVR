#pragma once
#include <windows.h>
#include <dinput.h>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <mutex>
#include <optional>
#include <cmath>
#include <array>
#include <exception>

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
    std::array<bool,2> armed_{};
    bool down_=false;
public:
    unsigned select_hand(bool enabled,unsigned current,const std::array<bool,2>& tracked,
                         const std::array<float,2>& triggers) {
        if (current>1) throw std::invalid_argument("Invalid current menu pointer hand");
        for (const auto trigger:triggers)
            if (!std::isfinite(trigger) || trigger<0 || trigger>1)
                throw std::invalid_argument("Invalid tracked menu trigger");
        if (!enabled) {hand_=2;armed_={};down_=false;return current;}
        for (unsigned hand=0;hand<2;++hand) {
            if (!tracked[hand]) armed_[hand]=false;
            else if (triggers[hand]<=.4f) armed_[hand]=true;
        }
        unsigned selected=current;
        if (!down_) {
            if (!tracked[current] && tracked[1-current]) selected=1-current;
            if (tracked[1-current] && armed_[1-current] && triggers[1-current]>=.6f) selected=1-current;
            if (tracked[current] && armed_[current] && triggers[current]>=.6f) selected=current;
        }
        for (unsigned hand=0;hand<2;++hand)
            if (hand!=selected && triggers[hand]>=.6f) armed_[hand]=false;
        return selected;
    }
    bool update(bool enabled,bool hit,unsigned hand,float trigger) {
        if (hand>1 || !std::isfinite(trigger) || trigger<0 || trigger>1)
            throw std::invalid_argument("Invalid tracked menu trigger");
        if (!enabled) {hand_=2;armed_={};down_=false;return false;}
        if (hand_!=hand) {hand_=hand;down_=false;}
        if (trigger<=.4f) {armed_[hand]=true;down_=false;}
        else if (trigger>=.6f && armed_[hand]) {armed_[hand]=false;down_=hit;}
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
    using Cursor=HCURSOR(WINAPI*)(HCURSOR);
    OutlastDesktopCursor(std::uintptr_t base,std::size_t extent):base_(base),end_(base) {
        if (!base || !extent || extent>std::numeric_limits<std::uintptr_t>::max()-base)
            throw std::invalid_argument("Desktop cursor policy requires a bounded native image");
        end_+=extent;
    }
    bool owns(std::uintptr_t caller) const {return caller>=base_ && caller<end_;}
    HCURSOR cursor(Cursor original,std::uintptr_t caller,HCURSOR requested,bool focused_client) const {
        if (!original) throw std::invalid_argument("Missing original cursor visibility entry");
        std::lock_guard<std::mutex> lock(mutex_);
        return original(owns(caller) && menu_active_ && focused_client?nullptr:requested);
    }
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

class OutlastWindowCursor {
    inline static std::recursive_mutex instance_mutex_;
    inline static OutlastWindowCursor* instance_=nullptr;
    HWND window_;
    const OutlastDesktopCursor& policy_;
    bool(WINAPI* focused_client_)(HWND);
    HHOOK hook_=nullptr;
    HHOOK input_hook_=nullptr;
    bool(WINAPI* settings_open_)()=nullptr;
    UINT desktop_message_=0;
    bool desktop_pending_=false;
    std::exception_ptr desktop_error_;
    void park_on_ui_thread() {
        RECT area{};
        if (!GetClientRect(window_,&area)) throw std::runtime_error("Cannot inspect native desktop frame");
        const auto style=GetWindowLongPtrW(window_,GWL_EXSTYLE);
        const auto parked_style=(style|WS_EX_TOOLWINDOW)&~LONG_PTR(WS_EX_APPWINDOW);
        if (!AdjustWindowRectEx(&area,DWORD(GetWindowLongPtrW(window_,GWL_STYLE)),
            GetMenu(window_)!=nullptr,DWORD(parked_style)))
            throw std::runtime_error("Cannot preserve native desktop client extent");
        SetLastError(0);
        if (!SetWindowLongPtrW(window_,GWL_EXSTYLE,parked_style) &&
            GetLastError()!=0)
            throw std::runtime_error("Cannot remove the duplicate native taskbar frame");
        const int x=GetSystemMetrics(SM_XVIRTUALSCREEN)-(area.right-area.left)-32;
        if (!SetWindowPos(window_,nullptr,x,GetSystemMetrics(SM_YVIRTUALSCREEN),
            area.right-area.left,area.bottom-area.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED))
            throw std::runtime_error("Cannot park the redundant native desktop frame");
    }
    static LRESULT CALLBACK queued(int code,WPARAM removed,LPARAM data) {
        if (code>=0 && removed==PM_REMOVE) {
            std::lock_guard<std::recursive_mutex> lock(instance_mutex_);
            auto* self=instance_;
            auto& event=*reinterpret_cast<MSG*>(data);
            if (self && event.hwnd==self->window_ && event.message==self->desktop_message_) {
                event.message=WM_NULL;
                self->desktop_pending_=false;
                try {self->park_on_ui_thread();}
                catch (...) {self->desktop_error_=std::current_exception();}
            }
            if (self && event.hwnd==self->window_ && self->settings_open_ &&
                self->settings_open_() &&
                ((event.message>=WM_KEYFIRST && event.message<=WM_KEYLAST) ||
                 (event.message>=WM_MOUSEFIRST && event.message<=WM_MOUSELAST)))
                event.message=WM_NULL;
        }
        return CallNextHookEx(nullptr,code,removed,data);
    }
    static LRESULT CALLBACK returned(int code,WPARAM message,LPARAM data) {
        const auto result=CallNextHookEx(nullptr,code,message,data);
        if (code<0) return result;
        const auto& event=*reinterpret_cast<const CWPRETSTRUCT*>(data);
        if (event.message!=WM_SETCURSOR || LOWORD(event.lParam)!=HTCLIENT) return result;
        std::lock_guard<std::recursive_mutex> lock(instance_mutex_);
        const auto* self=instance_;
        if (self) {
            if (event.hwnd==self->window_ && self->focused_client_(self->window_) &&
                self->policy_.menu_position())
                SetCursor(nullptr);
        }
        return result;
    }
public:
    OutlastWindowCursor(HWND window,const OutlastDesktopCursor& policy,
        bool(WINAPI* focused_client)(HWND),bool(WINAPI* settings_open)()=nullptr):
        window_(window),policy_(policy),focused_client_(focused_client),settings_open_(settings_open) {
        DWORD process=0;
        const auto thread=GetWindowThreadProcessId(window,&process);
        if (!window || !focused_client || !thread || process!=GetCurrentProcessId())
            throw std::invalid_argument("Menu cursor hook requires an owned native window");
        desktop_message_=RegisterWindowMessageW(L"OutlastVR.NativeDesktopHost");
        if (!desktop_message_) throw std::runtime_error("Cannot register native desktop window policy");
        std::lock_guard<std::recursive_mutex> lock(instance_mutex_);
        if (instance_)
            throw std::logic_error("Native menu cursor window hook is already active");
        instance_=this;
        hook_=SetWindowsHookExW(WH_CALLWNDPROCRET,&returned,nullptr,thread);
        if (!hook_) {
            instance_=nullptr;
            throw std::runtime_error("Cannot observe owned native cursor messages");
        }
        input_hook_=SetWindowsHookExW(WH_GETMESSAGE,&queued,nullptr,thread);
        if (!input_hook_) {
            if (!UnhookWindowsHookEx(hook_)) std::terminate();
            instance_=nullptr;
            throw std::runtime_error("Cannot isolate native window messages");
        }
    }
    OutlastWindowCursor(const OutlastWindowCursor&)=delete;
    OutlastWindowCursor& operator=(const OutlastWindowCursor&)=delete;
    void park_native_desktop() {
        std::lock_guard<std::recursive_mutex> lock(instance_mutex_);
        if (desktop_error_) std::rethrow_exception(desktop_error_);
        RECT area{};
        if (!GetWindowRect(window_,&area))
            throw std::runtime_error("Cannot locate native desktop frame");
        const auto style=GetWindowLongPtrW(window_,GWL_EXSTYLE);
        if ((style&WS_EX_TOOLWINDOW) && !(style&WS_EX_APPWINDOW) &&
            area.right<=GetSystemMetrics(SM_XVIRTUALSCREEN))
            return;
        // The UI can be waiting for this render thread; apply styles/moves on its own message queue.
        if (!desktop_pending_) {
            if (!PostMessageW(window_,desktop_message_,0,0))
                throw std::runtime_error("Cannot queue native desktop window policy");
            desktop_pending_=true;
        }
    }
    ~OutlastWindowCursor() {
        if (input_hook_ && !UnhookWindowsHookEx(input_hook_)) std::terminate();
        if (!UnhookWindowsHookEx(hook_)) std::terminate();
        std::lock_guard<std::recursive_mutex> lock(instance_mutex_);
        instance_=nullptr;
    }
};
}
