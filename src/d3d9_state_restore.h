#pragma once
#include "d3d9_resource.h"
#include <cstdint>
#include <iostream>
#include <array>
#include <algorithm>

namespace olvr {

struct D3D9StateCache {
    Com<IDirect3DStateBlock9> block;
    IDirect3DDevice9* device=nullptr;
    bool active=false;
    std::uint64_t allocations=0,captures=0;
};

class StateRestore {
    Com<IDirect3DStateBlock9> state_;
    D3D9StateCache* cache_=nullptr;
    IDirect3DDevice9* selected_device_=nullptr;
    // Resource bindings are held only for this draw, never by a persistent per-variant block.
    struct SavedStream {
        Com<IDirect3DVertexBuffer9> buffer;
        UINT slot=0,offset=0,stride=0;
    };
    std::array<SavedStream,2> streams_;
    std::array<Com<IDirect3DBaseTexture9>,18> textures_;
    std::array<UINT,18> samplers_{};
    Com<IDirect3DIndexBuffer9> indices_;
    UINT stream_count_=0,sampler_count_=0;
    bool restore_indices_=false;
    bool active_ = true;
public:
    explicit StateRestore(IDirect3DDevice9& device) {
        gpu_check(device.CreateStateBlock(D3DSBT_ALL, state_.put()), "Capture stereo caller state");
    }
    StateRestore(IDirect3DDevice9& device,D3D9StateCache& cache):cache_(&cache) {
        if (cache.active || (cache.device && cache.device!=&device))
            throw std::logic_error("Stereo state cache is already active or belongs to another device");
        if (!cache.block.get()) {
            gpu_check(device.CreateStateBlock(D3DSBT_ALL,cache.block.put()),"Create reusable stereo caller state");
            cache.device=&device;++cache.allocations;
        } else {
            gpu_check(cache.block->Capture(),"Refresh reusable stereo caller state");
        }
        *state_.put()=cache.block.get();state_->AddRef();
        ++cache.captures;cache.active=true;
    }
    template<std::size_t Size>
    StateRestore(IDirect3DDevice9& device,D3D9StateCache& cache,IDirect3DStateBlock9& selected,
                 UINT eye_stream,const std::array<UINT,Size>& samplers,UINT sampler_count,
                 bool convert_indices=false,bool upload_vertices=false):cache_(&cache) {
        if (cache.active || (cache.device && cache.device!=&device))
            throw std::logic_error("Selected caller-state cache is active or belongs to another device");
        if (!eye_stream || sampler_count>textures_.size() || sampler_count>Size)
            throw std::invalid_argument("Invalid selected caller resource bindings");
        std::copy_n(samplers.begin(),sampler_count,samplers_.begin());sampler_count_=sampler_count;
        for (UINT i=0;i<sampler_count_;++i)
            gpu_check(device.GetTexture(samplers_[i],textures_[i].put()),"Save transient caller view texture");
        stream_count_=upload_vertices?2:1;
        streams_[0].slot=eye_stream;
        if (upload_vertices) streams_[1].slot=0;
        for (UINT i=0;i<stream_count_;++i) {
            auto& stream=streams_[i];
            gpu_check(device.GetStreamSource(stream.slot,stream.buffer.put(),&stream.offset,&stream.stride),
                "Save transient caller vertex stream");
        }
        restore_indices_=convert_indices;
        if (restore_indices_) gpu_check(device.GetIndices(indices_.put()),"Save transient caller indices");
        gpu_check(selected.Capture(),"Refresh selected stereo caller state");
        *state_.put()=&selected;selected.AddRef();
        selected_device_=&device;
        cache.device=&device;
        ++cache.captures;cache.active=true;
    }
    StateRestore(const StateRestore&) = delete;
    StateRestore& operator=(const StateRestore&) = delete;
    HRESULT apply() noexcept {
        if (!active_) return D3DERR_INVALIDCALL;
        active_ = false;
        auto result=state_->Apply();
        auto restored=[&](HRESULT status) {if (FAILED(status) && SUCCEEDED(result)) result=status;};
        if (selected_device_) {
            for (UINT i=0;i<stream_count_;++i) {
                const auto& stream=streams_[i];
                restored(selected_device_->SetStreamSource(stream.slot,stream.buffer.get(),stream.offset,stream.stride));
            }
            if (restore_indices_) restored(selected_device_->SetIndices(indices_.get()));
            for (UINT i=0;i<sampler_count_;++i)
                restored(selected_device_->SetTexture(samplers_[i],textures_[i].get()));
        }
        if (cache_) cache_->active=false;
        return result;
    }
    ~StateRestore() {
        if (active_) {
            const auto status = apply();
            if (FAILED(status))
                std::cerr << "Stereo state restoration failed during exception unwinding: HRESULT="
                          << static_cast<long>(status) << '\n';
        }
    }
};

}
