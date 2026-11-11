#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <stdexcept>
#include <string>

namespace olvr {

inline void gpu_check(HRESULT status, const char* operation) {
    if (FAILED(status))
        throw std::runtime_error(std::string(operation) + " HRESULT=" +
                                 std::to_string(static_cast<long>(status)));
}

template<class T> class Com {
public:
    ~Com() { reset(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T** put() {
        if (value_)
            throw std::logic_error("COM output would overwrite an owned reference");
        return &value_;
    }
    void reset() noexcept {
        if (value_) {
            auto* previous = value_;
            value_ = nullptr;
            previous->Release();
        }
    }
private:
    T* value_ = nullptr;
};

} // namespace olvr
