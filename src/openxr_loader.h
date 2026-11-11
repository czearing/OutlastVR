#pragma once
#include "openxr_stereo_session.h"
#include <filesystem>

namespace olvr {

class OpenXRLoader {
    HMODULE module_ = nullptr;
    PFN_xrGetInstanceProcAddr resolve_ = nullptr;
public:
    explicit OpenXRLoader(const std::filesystem::path& library) {
        if (!library.is_absolute() || !std::filesystem::is_regular_file(library))
            throw std::invalid_argument("OpenXR loader must be an existing explicitly configured absolute DLL path");
        module_ = LoadLibraryExW(library.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            throw std::runtime_error("Cannot load configured OpenXR loader; Win32=" + std::to_string(GetLastError()));
        const auto address = GetProcAddress(module_, "xrGetInstanceProcAddr");
        static_assert(sizeof(address) == sizeof(resolve_));
        std::memcpy(&resolve_, &address, sizeof(resolve_));
        if (!resolve_) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw std::runtime_error("Configured DLL does not export xrGetInstanceProcAddr");
        }
    }
    ~OpenXRLoader() { if (module_) FreeLibrary(module_); }
    OpenXRLoader(const OpenXRLoader&) = delete;
    OpenXRLoader& operator=(const OpenXRLoader&) = delete;
    PFN_xrGetInstanceProcAddr resolve() const { return resolve_; }
};

// Declaration order keeps the loader alive until all runtime objects have been destroyed.
class LoadedOpenXRStereoSession {
    OpenXRLoader loader_;
    OpenXRStereoSession session_;
public:
    explicit LoadedOpenXRStereoSession(const std::filesystem::path& library,
                                      const char* required_runtime = nullptr,
                                      bool motion_controls = true)
        : loader_(library), session_(loader_.resolve(), required_runtime,motion_controls) {}
    OpenXRStereoSession& session() { return session_; }
};

} // namespace olvr
