#pragma once
#include "d3d9_resource.h"
#ifdef OLVR_ISOLATED_9ON12
#include "d3d9_on12_factory.h"
#endif
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace olvr {

class HiddenWindow {
public:
    HiddenWindow(UINT width, UINT height) {
        value_ = CreateWindowExW(0, L"STATIC", L"Isolated single-pass oracle", WS_OVERLAPPED,
                                0, 0, static_cast<int>(width), static_cast<int>(height),
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!value_)
            throw std::runtime_error("Hidden validation window creation failed");
    }
    ~HiddenWindow() { if (value_) DestroyWindow(value_); }
    HWND get() const { return value_; }
private:
    HWND value_ = nullptr;
};

class IsolatedDevice {
    HiddenWindow window_;
#ifdef OLVR_ISOLATED_9ON12
    D3D9On12Factory translation_;
#endif
    Com<IDirect3D9> factory_;
public:
    IsolatedDevice(UINT width, UINT height) : window_(width, height) {
#ifdef OLVR_ISOLATED_9ON12
        IDirect3D9* factory = translation_.get();
        factory->AddRef();
        const UINT adapter = translation_.adapter_ordinal();
#else
        IDirect3D9* factory = Direct3DCreate9(D3D_SDK_VERSION);
        const UINT adapter = 0;
#endif
        if (!factory)
            throw std::runtime_error("Direct3DCreate9 failed");
        *factory_.put() = factory;
        gpu_check(factory_->GetDeviceCaps(adapter, D3DDEVTYPE_HAL, &caps), "GetDeviceCaps");
        if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) ||
            caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
            throw std::runtime_error("The isolated device requires SM3");
        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.BackBufferWidth = width;
        parameters.BackBufferHeight = height;
        parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
        parameters.hDeviceWindow = window_.get();
        gpu_check(factory_->CreateDevice(adapter, D3DDEVTYPE_HAL, window_.get(),
                                        D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                        &parameters, device.put()), "CreateDevice");
#ifdef OLVR_ISOLATED_9ON12
        verify_on12_device(*device.get(), *translation_.device());
#endif
        gpu_check(factory_->CheckDeviceFormat(
            adapter, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_VERTEXTEXTURE,
            D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F), "Float32 vertex texture capability");
    }
    D3DCAPS9 caps{};
    Com<IDirect3DDevice9> device;
#ifdef OLVR_ISOLATED_9ON12
    ID3D12Device& translation_device() const { return *translation_.device(); }
    ID3D12CommandQueue* translation_queue() const { return translation_.queue(); }
#endif
};

inline std::vector<DWORD> read_shader(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("Could not read shader " + path.string());
    const auto size = input.tellg();
    if (size < 8 || size % 4 != 0)
        throw std::runtime_error("Invalid shader extent " + path.string());
    std::vector<DWORD> code(static_cast<std::size_t>(size) / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), size);
    if (!input)
        throw std::runtime_error("Truncated shader " + path.string());
    return code;
}

inline void compile_fixture(const char* source, const char* profile, const std::filesystem::path& path,
                            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS) {
    Com<ID3DBlob> code, errors;
    const auto status = D3DCompile(source, std::char_traits<char>::length(source), "isolated-fixture",
                                  nullptr, nullptr, "main", profile, flags,
                                  0, code.put(), errors.put());
    if (FAILED(status)) {
        const auto message = errors.get()
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
            : std::string("No compiler diagnostics");
        throw std::runtime_error("Shader fixture compilation failed: " + message);
    }
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
    if (!output)
        throw std::runtime_error("Could not write isolated fixture " + path.string());
}

} // namespace olvr
