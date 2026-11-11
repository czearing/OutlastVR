#pragma once
#include "d3d9_resource.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <initguid.h>
#include <d3d9on12.h>
#include <cstring>
#include <iterator>
#include <string>

namespace olvr {

class SystemD3D9Module {
public:
    SystemD3D9Module() {
        wchar_t directory[32768]{};
        const auto size=GetSystemDirectoryW(directory,static_cast<UINT>(std::size(directory)));
        if (!size || size>=std::size(directory))
            throw std::runtime_error("Cannot resolve absolute system D3D9 directory");
        const auto path=std::wstring(directory,size)+L"\\d3d9.dll";
        value_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!value_)
            throw std::runtime_error("Cannot load system D3D9 for GPU-only translation");
    }
    ~SystemD3D9Module() { FreeLibrary(value_); }
    SystemD3D9Module(const SystemD3D9Module&) = delete;
    SystemD3D9Module& operator=(const SystemD3D9Module&) = delete;
    FARPROC find(const char* name) const { return GetProcAddress(value_, name); }
private:
    HMODULE value_ = nullptr;
};

class D3D9On12Factory {
    SystemD3D9Module module_;
    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<IDirect3D9> factory_;
    UINT adapter_ordinal_ = 0;
public:
    explicit D3D9On12Factory(const LUID* required_adapter = nullptr,
                            D3D_FEATURE_LEVEL minimum_level = D3D_FEATURE_LEVEL_11_0) {
        Com<IDXGIFactory4> inventory;
        gpu_check(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                    reinterpret_cast<void**>(inventory.put())), "Create adapter inventory");
        Com<IDXGIAdapter1> adapter;
        if (required_adapter) {
            gpu_check(inventory->EnumAdapterByLuid(*required_adapter, __uuidof(IDXGIAdapter1),
                                                  reinterpret_cast<void**>(adapter.put())),
                      "Select runtime-required GPU adapter");
        } else {
            gpu_check(inventory->EnumAdapters1(0, adapter.put()), "Select isolated hardware adapter");
        }
        DXGI_ADAPTER_DESC1 description{};
        gpu_check(adapter->GetDesc1(&description), "Read hardware adapter identity");
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            throw std::runtime_error("GPU-only bridge refuses a software adapter");
        gpu_check(D3D12CreateDevice(adapter.get(), minimum_level, __uuidof(ID3D12Device),
                                  reinterpret_cast<void**>(device_.put())), "Create translation GPU device");
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        gpu_check(device_->CreateCommandQueue(&queue_description, __uuidof(ID3D12CommandQueue),
                                              reinterpret_cast<void**>(queue_.put())), "Create translation queue");
        PFN_Direct3DCreate9On12 create = nullptr;
        const auto address = module_.find("Direct3DCreate9On12");
        static_assert(sizeof(create) == sizeof(address));
        std::memcpy(&create, &address, sizeof(create));
        if (!create)
            throw std::runtime_error("System D3D9 does not expose Direct3DCreate9On12");
        D3D9ON12_ARGS override{};
        override.Enable9On12 = TRUE;
        override.pD3D12Device = device_.get();
        override.ppD3D12Queues[0] = queue_.get();
        override.NumQueues = 1;
        *factory_.put() = create(D3D_SDK_VERSION, &override, 1);
        if (!factory_.get())
            throw std::runtime_error("Direct3DCreate9On12 returned no classic factory");
        const auto requested_luid = device_->GetAdapterLuid();
        using CreateEx = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
        CreateEx create_ex = nullptr;
        const auto ex_address = module_.find("Direct3DCreate9Ex");
        static_assert(sizeof(create_ex) == sizeof(ex_address));
        std::memcpy(&create_ex, &ex_address, sizeof(create_ex));
        if (!create_ex)
            throw std::runtime_error("System D3D9 lacks adapter-LUID inventory support");
        Com<IDirect3D9Ex> identities;
        gpu_check(create_ex(D3D_SDK_VERSION, identities.put()), "Create adapter identity inventory only");
        bool found = false;
        for (UINT index = 0; index < identities->GetAdapterCount(); ++index) {
            LUID candidate{};
            gpu_check(identities->GetAdapterLUID(index, &candidate), "Read legacy adapter LUID");
            if (candidate.LowPart != requested_luid.LowPart || candidate.HighPart != requested_luid.HighPart)
                continue;
            if (index >= factory_->GetAdapterCount())
                throw std::runtime_error("Classic translation factory omits the requested adapter");
            D3DADAPTER_IDENTIFIER9 expected{}, exposed{};
            gpu_check(identities->GetAdapterIdentifier(index, 0, &expected), "Read identified adapter");
            gpu_check(factory_->GetAdapterIdentifier(index, 0, &exposed), "Read translation adapter");
            if (std::memcmp(&expected.DeviceIdentifier, &exposed.DeviceIdentifier, sizeof(GUID)) != 0 ||
                expected.VendorId != exposed.VendorId || expected.DeviceId != exposed.DeviceId)
                throw std::runtime_error("Legacy and translation adapter inventories disagree");
            adapter_ordinal_ = index;
            found = true;
            break;
        }
        if (!found)
            throw std::runtime_error("No classic D3D9 adapter matches the OpenXR D3D12 device");
    }
    IDirect3D9* get() const { return factory_.get(); }
    ID3D12Device* device() const { return device_.get(); }
    ID3D12CommandQueue* queue() const { return queue_.get(); }
    UINT adapter_ordinal() const { return adapter_ordinal_; }
};

inline void verify_on12_device(IDirect3DDevice9& legacy, ID3D12Device& expected) {
    Com<IDirect3DDevice9On12> bridge;
    gpu_check(legacy.QueryInterface(IID_IDirect3DDevice9On12,
                                    reinterpret_cast<void**>(bridge.put())),
              "Require actual D3D9On12 device; native fallback is forbidden");
    Com<ID3D12Device> underlying;
    gpu_check(bridge->GetD3D12Device(__uuidof(ID3D12Device),
                                    reinterpret_cast<void**>(underlying.put())),
              "Verify translation device identity");
    const auto actual_luid = underlying->GetAdapterLuid();
    const auto expected_luid = expected.GetAdapterLuid();
    if (actual_luid.LowPart != expected_luid.LowPart || actual_luid.HighPart != expected_luid.HighPart)
        throw std::runtime_error("Legacy renderer and submission device adapter identities differ");
    Com<IUnknown> actual_identity, expected_identity;
    gpu_check(underlying->QueryInterface(__uuidof(IUnknown),
                                        reinterpret_cast<void**>(actual_identity.put())),
              "Read translation device COM identity");
    gpu_check(expected.QueryInterface(__uuidof(IUnknown),
                                      reinterpret_cast<void**>(expected_identity.put())),
              "Read submission device COM identity");
    if (actual_identity.get() != expected_identity.get())
        throw std::runtime_error("Translation and submission must use the same D3D12 device, not just the same adapter");
}

} // namespace olvr
