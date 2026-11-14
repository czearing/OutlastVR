#pragma once
#include "d3d9_on12_transfer.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>

namespace olvr {
class OutlastDesktopMirror {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread thread_;
    std::exception_ptr error_;
    HWND window_=nullptr;
    bool started_=false;
    std::atomic<bool> closed_{false};
    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<IDXGISwapChain3> chain_;
    std::unique_ptr<D3D9On12StereoTransfer> transfer_;
    Com<IDirect3DTexture9> scaled_;
    Com<IDirect3DSurface9> target_;
    UINT width_=0,height_=0;
    static constexpr UINT shutdown_message=WM_APP+137;
    static RECT eye_rectangle(UINT width,UINT height,UINT eye_width,UINT eye_height) {
        if (!width || !height || !eye_width || !eye_height)
            throw std::invalid_argument("Invalid desktop mirror extent");
        const float fit=std::min(float(width)/eye_width,float(height)/eye_height);
        const LONG fitted_width=std::clamp<LONG>(LONG(eye_width*fit),1,LONG(width));
        const LONG fitted_height=std::clamp<LONG>(LONG(eye_height*fit),1,LONG(height));
        const LONG x=(LONG(width)-fitted_width)/2,y=(LONG(height)-fitted_height)/2;
        return {x,y,x+fitted_width,y+fitted_height};
    }

    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
        if (message==WM_NCCREATE) {
            const auto* creation=reinterpret_cast<const CREATESTRUCTW*>(l);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        if (message==WM_CLOSE) {
            auto* mirror=reinterpret_cast<OutlastDesktopMirror*>(GetWindowLongPtrW(window,GWLP_USERDATA));
            if (mirror) mirror->closed_=true;
            ShowWindow(window,SW_HIDE);
            return 0;
        }
        if (message==shutdown_message) {DestroyWindow(window);return 0;}
        if (message==WM_DESTROY) {PostQuitMessage(0);return 0;}
        if (message==WM_ERASEBKGND) return 1;
        return DefWindowProcW(window,message,w,l);
    }
    void run() {
        HINSTANCE module=nullptr;
        ATOM type=0;
        HWND window=nullptr;
        try {
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&procedure),&module))
                throw std::runtime_error("Cannot locate desktop mirror module");
            WNDCLASSW description{};
            description.hInstance=module;description.lpfnWndProc=&procedure;
            description.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
            description.lpszClassName=L"OutlastVRDesktopMirror";
            type=RegisterClassW(&description);
            if (!type) throw std::runtime_error("Cannot register desktop mirror window");
            MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
            if (!GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor))
                throw std::runtime_error("Cannot locate desktop mirror monitor");
            const auto& work=monitor.rcWork;
            RECT area{0,0,std::min<LONG>(1280,work.right-work.left-80),
                std::min<LONG>(720,work.bottom-work.top-80)};
            if (!AdjustWindowRectEx(&area,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_APPWINDOW))
                throw std::runtime_error("Cannot size desktop mirror window");
            window=CreateWindowExW(WS_EX_APPWINDOW,description.lpszClassName,
                L"Outlast VR - Desktop",WS_OVERLAPPEDWINDOW,
                work.left+20,work.top+20,area.right-area.left,area.bottom-area.top,
                nullptr,nullptr,module,this);
            if (!window) throw std::runtime_error("Cannot create visible desktop mirror window");
            ShowWindow(window,SW_SHOWNOACTIVATE);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                window_=window;started_=true;
            }
            ready_.notify_one();
            MSG message{};
            for (;;) {
                const auto result=GetMessageW(&message,nullptr,0,0);
                if (result==-1) throw std::runtime_error("Desktop mirror message loop failed");
                if (!result) break;
                TranslateMessage(&message);DispatchMessageW(&message);
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_=std::current_exception();started_=true;
            }
            ready_.notify_one();
        }
        if (window && IsWindow(window)) DestroyWindow(window);
        closed_=true;
        if (type && !UnregisterClassW(L"OutlastVRDesktopMirror",module)) std::terminate();
    }
public:
    OutlastDesktopMirror() {
        thread_=std::thread([this] {run();});
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock,[this] {return started_;});
        if (error_) {
            const auto error=error_;
            lock.unlock();thread_.join();std::rethrow_exception(error);
        }
    }
    OutlastDesktopMirror(const OutlastDesktopMirror&)=delete;
    OutlastDesktopMirror& operator=(const OutlastDesktopMirror&)=delete;
    ~OutlastDesktopMirror() {
        reset();
        if (IsWindow(window_) && !PostMessageW(window_,shutdown_message,0,0))
            std::terminate();
        if (thread_.joinable()) thread_.join();
    }
    HWND window() const {return window_;}
    std::optional<std::array<float,2>> pointer(POINT screen,UINT eye_width,UINT eye_height) const {
        if (closed_ || IsIconic(window_)) return {};
        RECT client{};
        if (!GetClientRect(window_,&client) || !ScreenToClient(window_,&screen))
            throw std::runtime_error("Cannot locate desktop mirror pointer");
        if (!client.right || !client.bottom) return {};
        const auto image=eye_rectangle(UINT(client.right),UINT(client.bottom),eye_width,eye_height);
        if (screen.x<image.left || screen.x>=image.right || screen.y<image.top || screen.y>=image.bottom)
            return {};
        return std::array<float,2>{float(screen.x-image.left)/(image.right-image.left),
            float(screen.y-image.top)/(image.bottom-image.top)};
    }
    void reset() {
        transfer_.reset();target_.reset();scaled_.reset();chain_.reset();width_=height_=0;
    }
    bool present(IDirect3DDevice9& device,IDirect3DSurface9& atlas,UINT eye_width,UINT eye_height) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_) std::rethrow_exception(error_);
        }
        if (closed_ || IsIconic(window_)) return false;
        RECT client{};
        if (!GetClientRect(window_,&client))
            throw std::runtime_error("Cannot inspect desktop mirror client area");
        const auto width=UINT(client.right),height=UINT(client.bottom);
        if (!width || !height) return false;
        if (!device_.get()) {
            Com<IDirect3DDevice9On12> bridge;
            gpu_check(device.QueryInterface(IID_IDirect3DDevice9On12,
                reinterpret_cast<void**>(bridge.put())),"Get desktop GPU bridge");
            gpu_check(bridge->GetD3D12Device(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(device_.put())),"Get desktop presentation device");
            D3D12_COMMAND_QUEUE_DESC queue{};
            queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            gpu_check(device_->CreateCommandQueue(&queue,__uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(queue_.put())),"Create independent desktop presentation queue");
        }
        if (!transfer_)
            transfer_=std::make_unique<D3D9On12StereoTransfer>(device,*device_.get(),*queue_.get());
        if (!chain_.get()) {
            Com<IDXGIFactory4> factory;
            gpu_check(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                reinterpret_cast<void**>(factory.put())),"Create desktop presentation factory");
            DXGI_SWAP_CHAIN_DESC1 parameters{};
            parameters.Width=width;parameters.Height=height;
            parameters.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            parameters.SampleDesc.Count=1;
            parameters.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
            parameters.BufferCount=2;parameters.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
            parameters.Scaling=DXGI_SCALING_STRETCH;parameters.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
            Com<IDXGISwapChain1> created;
            gpu_check(factory->CreateSwapChainForHwnd(queue_.get(),window_,&parameters,nullptr,
                nullptr,created.put()),"Create independent DXGI desktop swapchain");
            gpu_check(created->QueryInterface(__uuidof(IDXGISwapChain3),
                reinterpret_cast<void**>(chain_.put())),"Get desktop flip swapchain");
            gpu_check(factory->MakeWindowAssociation(window_,DXGI_MWA_NO_ALT_ENTER),
                "Keep desktop mirror windowed");
        }
        if (width_!=width || height_!=height) {
            transfer_->wait();
            target_.reset();scaled_.reset();
            gpu_check(chain_->ResizeBuffers(2,width,height,DXGI_FORMAT_B8G8R8A8_UNORM,0),
                "Resize independent desktop swapchain");
            gpu_check(device.CreateTexture(width*2,height,1,D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,scaled_.put(),nullptr),
                "Create GPU-only desktop scaling atlas");
            gpu_check(scaled_->GetSurfaceLevel(0,target_.put()),"Get desktop scaling surface");
            width_=width;height_=height;
        }
        gpu_check(device.ColorFill(target_.get(),nullptr,0xff000000),"Clear desktop mirror letterbox");
        const RECT source{0,0,LONG(eye_width),LONG(eye_height)};
        const auto destination=eye_rectangle(width,height,eye_width,eye_height);
        gpu_check(device.StretchRect(&atlas,&source,target_.get(),&destination,D3DTEXF_LINEAR),
            "Copy final left eye to visible desktop without rendering a second world pass");
        Com<ID3D12Resource> target;
        gpu_check(chain_->GetBuffer(chain_->GetCurrentBackBufferIndex(),__uuidof(ID3D12Resource),
            reinterpret_cast<void**>(target.put())),"Acquire DXGI desktop image");
        transfer_->copy(*scaled_.get(),{{{target.get(),0,D3D12_RESOURCE_STATE_PRESENT},{}}},1);
        const auto status=chain_->Present(0,0);
        if (status==DXGI_STATUS_OCCLUDED) return false;
        gpu_check(status,"Present visible desktop mirror");
        return true;
    }
};
}
