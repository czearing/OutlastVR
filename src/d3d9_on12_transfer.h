#pragma once
#include "d3d9_on12_factory.h"
#include <array>
#include <limits>
#include <cstdint>

namespace olvr {

struct D3D12EyeTarget {
    ID3D12Resource* resource = nullptr;
    UINT subresource = 0;
    D3D12_RESOURCE_STATES acquired_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
};

class D3D9On12StereoTransfer {
    Com<IDirect3DDevice9On12> bridge_;
    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<ID3D12Fence> fence_;
    struct Flight {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> commands;
        Com<IDirect3DTexture9> atlas;
        Com<ID3D12Resource> source;
        std::array<Com<ID3D12Resource>,2> eyes;
        UINT64 serial=0;
        void release() noexcept {
            atlas.reset();source.reset();
            for (auto& eye:eyes) eye.reset();
        }
    };
    std::array<Flight,3> flights_;
    UINT64 serial_ = 0;
    bool poisoned_ = false;

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, UINT subresource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }
    void require_device(ID3D12Resource& resource) const {
        Com<ID3D12Device> owner;
        gpu_check(resource.GetDevice(__uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(owner.put())),
                  "Read swapchain resource device");
        Com<IUnknown> actual, expected;
        gpu_check(owner->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(actual.put())),
                  "Read swapchain device identity");
        gpu_check(device_->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(expected.put())),
                  "Read transfer device identity");
        if (actual.get() != expected.get())
            throw std::invalid_argument("Eye image belongs to a different D3D12 device");
    }
    void wait_until(UINT64 serial,UINT timeout_ms) {
        if (poisoned_)
            throw std::runtime_error("GPU stereo transfer is poisoned; recreate the device/session");
        const auto deadline=GetTickCount64()+timeout_ms;
        for (;;) {
            const auto completed=fence_->GetCompletedValue();
            if (completed==UINT64_MAX) {
                poisoned_=true;
                throw std::runtime_error("Stereo transfer device was removed");
            }
            if (completed>=serial) {
                for (auto& flight:flights_)
                    if (completed>=flight.serial) flight.release();
                return;
            }
            if (GetTickCount64()>=deadline)
                throw std::runtime_error("Stereo transfer completion timed out; resources remain retained");
            Sleep(1);
        }
    }
public:
    D3D9On12StereoTransfer(IDirect3DDevice9& legacy, ID3D12Device& device,
                          ID3D12CommandQueue& queue) {
        verify_on12_device(legacy, device);
        if (queue.GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("Stereo transfer requires the direct OpenXR binding queue");
        Com<ID3D12Device> queue_owner;
        gpu_check(queue.GetDevice(__uuidof(ID3D12Device),
                                  reinterpret_cast<void**>(queue_owner.put())),
                  "Read transfer queue device");
        *device_.put() = &device;
        device.AddRef();
        // Resources and the queue must belong to the same actual device, not merely the same adapter.
        Com<IUnknown> actual, expected;
        gpu_check(queue_owner->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(actual.put())),
                  "Read queue device identity");
        gpu_check(device.QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(expected.put())),
                  "Read binding device identity");
        if (actual.get() != expected.get())
            throw std::invalid_argument("Transfer queue belongs to a different D3D12 device");
        *queue_.put() = &queue;
        queue.AddRef();
        gpu_check(legacy.QueryInterface(IID_IDirect3DDevice9On12,
                                        reinterpret_cast<void**>(bridge_.put())),
                  "Get stereo resource bridge");
        for (auto& flight:flights_) {
            gpu_check(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                __uuidof(ID3D12CommandAllocator),
                                                reinterpret_cast<void**>(flight.allocator.put())),
                  "Create persistent stereo-copy allocator");
            gpu_check(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, flight.allocator.get(), nullptr,
                                           __uuidof(ID3D12GraphicsCommandList),
                                           reinterpret_cast<void**>(flight.commands.put())),
                  "Create persistent stereo-copy list");
            gpu_check(flight.commands->Close(), "Initialize stereo-copy list");
        }
        gpu_check(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                     reinterpret_cast<void**>(fence_.put())),
                  "Create stereo ownership fence");
    }
    D3D9On12StereoTransfer(const D3D9On12StereoTransfer&) = delete;
    D3D9On12StereoTransfer& operator=(const D3D9On12StereoTransfer&) = delete;

    void wait(UINT timeout_ms = 10000) {
        wait_until(serial_,timeout_ms);
    }

    UINT64 copy(IDirect3DTexture9& atlas, const std::array<D3D12EyeTarget, 2>& eyes) {
        if (poisoned_)
            throw std::runtime_error("GPU stereo transfer is poisoned; recreate the device/session");
        D3DSURFACE_DESC description{};
        gpu_check(atlas.GetLevelDesc(0, &description), "Read stereo atlas description");
        if (description.Width < 2 || description.Width % 2 || !description.Height ||
            description.MultiSampleType != D3DMULTISAMPLE_NONE ||
            description.Format != D3DFMT_A8R8G8B8 || !(description.Usage & D3DUSAGE_RENDERTARGET) ||
            description.Pool != D3DPOOL_DEFAULT)
            throw std::invalid_argument("Stereo transport requires a non-MSAA BGRA8 default-pool render atlas");
        const UINT width = description.Width / 2, height = description.Height;
        for (const auto& eye : eyes) {
            if (!eye.resource || eye.acquired_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
                throw std::invalid_argument("Eye image must be acquired and writable in RENDER_TARGET state");
            require_device(*eye.resource);
            const auto target = eye.resource->GetDesc();
            if (target.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || target.Width != width ||
                target.Height != height ||
                (target.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                 target.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
                 target.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS) ||
                target.SampleDesc.Count != 1 || target.MipLevels != 1 ||
                eye.subresource >= target.DepthOrArraySize ||
                !(target.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
                throw std::invalid_argument("Eye image does not match atlas: expected="+
                    std::to_string(width)+"x"+std::to_string(height)+"; actual="+
                    std::to_string(target.Width)+"x"+std::to_string(target.Height)+
                    "; format="+std::to_string(target.Format)+"; mips="+std::to_string(target.MipLevels)+
                    "; layers="+std::to_string(target.DepthOrArraySize)+"; flags="+std::to_string(target.Flags));
        }
        if (eyes[0].resource == eyes[1].resource && eyes[0].subresource == eyes[1].subresource)
            throw std::invalid_argument("Left and right eyes cannot overwrite the same image subresource");
        if (serial_ == std::numeric_limits<UINT64>::max() - 1)
            throw std::overflow_error("Stereo ownership fence exhausted");
        auto& flight=flights_[serial_%flights_.size()];
        wait_until(flight.serial,10000);
        auto* commands=flight.commands.get();
        gpu_check(flight.allocator->Reset(), "Reuse completed stereo-copy allocator");
        gpu_check(commands->Reset(flight.allocator.get(), nullptr), "Reuse stereo-copy commands");
        Com<ID3D12Resource> source;
        gpu_check(bridge_->UnwrapUnderlyingResource(&atlas, queue_.get(), __uuidof(ID3D12Resource),
                                                    reinterpret_cast<void**>(source.put())),
                  "Acquire atlas GPU ownership");
        bool executed = false;
        try {
            const auto source_description = source->GetDesc();
            if (source_description.Width != description.Width || source_description.Height != height ||
                source_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
                source_description.SampleDesc.Count != 1)
                throw std::runtime_error("Translated atlas identity or description changed");
            for (const auto& eye : eyes)
                if (source.get() == eye.resource)
                    throw std::invalid_argument("An eye image cannot alias the source atlas");
            auto source_barrier = transition(source.get(), 0, D3D12_RESOURCE_STATE_COMMON,
                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &source_barrier);
            for (UINT eye = 0; eye < 2; ++eye) {
                auto barrier = transition(eyes[eye].resource, eyes[eye].subresource,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                commands->ResourceBarrier(1, &barrier);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = source.get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource = eyes[eye].resource;
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = eyes[eye].subresource;
                const D3D12_BOX region{eye * width, 0, 0, (eye + 1) * width, height, 1};
                commands->CopyTextureRegion(&to, 0, 0, 0, &from, &region);
                barrier = transition(eyes[eye].resource, eyes[eye].subresource,
                                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
                commands->ResourceBarrier(1, &barrier);
            }
            source_barrier = transition(source.get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                         D3D12_RESOURCE_STATE_COMMON);
            commands->ResourceBarrier(1, &source_barrier);
            gpu_check(commands->Close(), "Close stereo image transfer");
            *flight.atlas.put() = &atlas;
            atlas.AddRef();
            *flight.source.put() = source.get();
            source->AddRef();
            for (UINT eye = 0; eye < 2; ++eye) {
                *flight.eyes[eye].put() = eyes[eye].resource;
                eyes[eye].resource->AddRef();
            }
            ID3D12CommandList* lists[]{commands};
            executed = true;
            queue_->ExecuteCommandLists(1, lists);
            ++serial_;
            flight.serial=serial_;
            gpu_check(queue_->Signal(fence_.get(), serial_), "Signal stereo image availability");
            ID3D12Fence* fences[]{fence_.get()};
            gpu_check(bridge_->ReturnUnderlyingResource(&atlas, 1, &serial_, fences),
                      "Return atlas ownership with completion dependency");
        } catch (...) {
            if (executed) {
                poisoned_ = true;
            } else {
                const auto status = bridge_->ReturnUnderlyingResource(&atlas, 0, nullptr, nullptr);
                if (FAILED(status)) {
                    poisoned_ = true;
                    throw std::runtime_error("Stereo-copy failure also failed to return atlas ownership; HRESULT=" +
                                             std::to_string(static_cast<long>(status)));
                }
            }
            throw;
        }
        return serial_;
    }

    ~D3D9On12StereoTransfer() {
        try {
            wait();
        } catch (const std::exception& error) {
            OutputDebugStringA(error.what());
            // Releasing command resources while a healthy GPU may still reference them is unsafe.
            if (SUCCEEDED(device_->GetDeviceRemovedReason()))
                std::terminate();
        }
    }
};

} // namespace olvr
