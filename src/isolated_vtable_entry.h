#pragma once
#include <windows.h>
#include <iostream>
#include <stdexcept>

namespace olvr {

class IsolatedVtableEntry {
    void** address_ = nullptr;
    void* original_ = nullptr;
    DWORD original_protection_ = 0;
public:
    IsolatedVtableEntry() = default;
    IsolatedVtableEntry(const IsolatedVtableEntry&) = delete;
    IsolatedVtableEntry& operator=(const IsolatedVtableEntry&) = delete;
    void install(void** address, void* replacement) {
        if (address_)
            throw std::logic_error("Native counter entry is already attached");
        DWORD protection = 0;
        if (!VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &protection))
            throw std::runtime_error("Cannot protect D3D12 counter entry");
        address_ = address;
        original_protection_ = protection;
        original_ = InterlockedExchangePointer(address, replacement);
        DWORD ignored = 0;
        if (!VirtualProtect(address, sizeof(*address), protection, &ignored))
            throw std::runtime_error("Cannot restore D3D12 counter entry protection");
    }
    void* original() const { return original_; }
    void detach() noexcept {
        if (!address_)
            return;
        DWORD protection = 0, ignored = 0;
        if (!VirtualProtect(address_, sizeof(*address_), PAGE_READWRITE, &protection)) {
            std::cerr << "Cannot detach D3D12 native draw counter: " << GetLastError() << '\n';
            std::terminate();
        }
        InterlockedExchangePointer(address_, original_);
        if (!VirtualProtect(address_, sizeof(*address_), original_protection_, &ignored)) {
            std::cerr << "Cannot restore D3D12 counter memory protection: " << GetLastError() << '\n';
            std::terminate();
        }
        address_ = nullptr;
    }
    ~IsolatedVtableEntry() { detach(); }
};

}
