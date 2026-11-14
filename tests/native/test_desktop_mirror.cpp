#include "outlast_desktop_mirror.h"
#include "outlast_desktop_cursor.h"
#include "isolated_d3d9.h"
#include <iostream>
using namespace olvr;
namespace {bool WINAPI owned_client(HWND) {return true;}}
int main() {
    try {
        IsolatedDevice gpu(128,64);
        Com<IDirect3DTexture9> atlas;
        Com<IDirect3DSurface9> surface;
        gpu_check(gpu.device->CreateTexture(128,64,1,D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,atlas.put(),nullptr),"Create independent mirror source");
        gpu_check(atlas->GetSurfaceLevel(0,surface.put()),"Get mirror source");
        OutlastDesktopMirror mirror;
        HiddenWindow native(128,64);
        SetWindowLongPtrW(native.get(),GWL_STYLE,WS_OVERLAPPEDWINDOW);
        if (!SetWindowPos(native.get(),nullptr,80,80,320,240,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED))
            throw std::runtime_error("Cannot configure the native framed-window regression");
        ShowWindow(native.get(),SW_SHOWNOACTIVATE);
        OutlastDesktopCursor policy(0x1000,0x100);
        OutlastWindowCursor host(native.get(),policy,&owned_client);
        RECT before{},after{};
        GetClientRect(native.get(),&before);
        host.park_native_desktop();
        MSG message{};
        for (unsigned attempt=0;attempt<30;++attempt) {
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            Sleep(10);
        }
        GetClientRect(native.get(),&after);
        RECT parked{};GetWindowRect(native.get(),&parked);
        if (!IsWindowVisible(native.get()) || IsIconic(native.get()) ||
            parked.right>GetSystemMetrics(SM_XVIRTUALSCREEN) ||
            !(GetWindowLongPtrW(native.get(),GWL_EXSTYLE)&WS_EX_TOOLWINDOW) ||
            (GetWindowLongPtrW(native.get(),GWL_EXSTYLE)&WS_EX_APPWINDOW) ||
            before.right!=after.right || before.bottom!=after.bottom)
            throw std::runtime_error("Native frame was not parked without hiding, minimizing or changing render extent");
        if (!SetWindowPos(native.get(),nullptr,80,80,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE))
            throw std::runtime_error("Cannot exercise native window resurfacing");
        host.park_native_desktop();
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        GetWindowRect(native.get(),&parked);
        GetClientRect(native.get(),&after);
        if (parked.right>GetSystemMetrics(SM_XVIRTUALSCREEN) ||
            before.right!=after.right || before.bottom!=after.bottom)
            throw std::runtime_error("Native startup/restore resurfaced the duplicate frame or changed render extent");
        if (!IsWindowVisible(mirror.window()))
            throw std::runtime_error("Spectator window is not visible on the actual desktop");
        if (!SetWindowPos(mirror.window(),HWND_TOPMOST,40,40,640,480,SWP_NOACTIVATE))
            throw std::runtime_error("Cannot expose isolated spectator window for desktop pixel proof");
        for (unsigned phase=0;phase<3;++phase) {
            if (phase==1 && !SetWindowPos(mirror.window(),nullptr,0,0,800,540,SWP_NOMOVE|
                SWP_NOZORDER|SWP_NOACTIVATE))
                throw std::runtime_error("Cannot resize visible spectator window");
            if (phase==2) mirror.reset();
            const RECT left{0,0,64,64};
            const DWORD color=phase==1?0xff00ff00:0xffff0000;
            gpu_check(gpu.device->ColorFill(surface.get(),nullptr,0xff0000ff),"Fill distinct right eye");
            gpu_check(gpu.device->ColorFill(surface.get(),&left,color),"Fill independently selected left eye");
            bool visible=false;
            for (unsigned attempt=0;attempt<30 && !visible;++attempt) {
                if (!mirror.present(*gpu.device.get(),*surface.get(),64,64))
                    throw std::runtime_error("Visible spectator frame was not presented");
                Sleep(30);
                RECT client{};GetClientRect(mirror.window(),&client);
                POINT center{client.right/2,client.bottom/2};
                if (!ClientToScreen(mirror.window(),&center))
                    throw std::runtime_error("Cannot locate desktop pixel oracle");
                const auto pointer=mirror.pointer(center,64,64);
                POINT letterbox{0,client.bottom/2};
                if (!ClientToScreen(mirror.window(),&letterbox) || !pointer ||
                    std::abs((*pointer)[0]-.5f)>.003f || std::abs((*pointer)[1]-.5f)>.003f ||
                    mirror.pointer(letterbox,64,64))
                    throw std::runtime_error("Desktop mouse does not follow the resized/letterboxed visible eye");
                HDC screen=GetDC(nullptr);
                if (!screen) throw std::runtime_error("Cannot read independently composed desktop pixels");
                const auto pixel=GetPixel(screen,center.x,center.y);
                ReleaseDC(nullptr,screen);
                visible=pixel==(phase==1?RGB(0,255,0):RGB(255,0,0));
            }
            if (!visible)
                throw std::runtime_error("Composed desktop pixels do not show the changing final left eye");
        }
        ShowWindow(mirror.window(),SW_MINIMIZE);
        if (mirror.present(*gpu.device.get(),*surface.get(),64,64))
            throw std::runtime_error("Minimized desktop mirror still submits presentation work");
        ShowWindow(mirror.window(),SW_SHOWNOACTIVATE);
        SendMessageW(mirror.window(),WM_CLOSE,0,0);
        if (IsWindowVisible(mirror.window()) || mirror.present(*gpu.device.get(),*surface.get(),64,64))
            throw std::runtime_error("Closing the spectator did not stop its presentation independently");
        std::cout<<"PASS: visible_desktop_pixels; distinct_left_eye; animated_color_change; "
            "single_visible_window; unchanged_native_extent; resize_recovery; swapchain_recreation; "
            "letterboxed_mouse_mapping; minimized_skips; independent_close; no_second_world_pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
