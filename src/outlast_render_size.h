#pragma once
#include <windows.h>
#include <stdexcept>

namespace olvr {
inline bool outlast_render_size(const void* client,const void* game_viewport,UINT& native_width,UINT& native_height,
                                UINT width,UINT height) {
    if (!game_viewport || client!=static_cast<const unsigned char*>(game_viewport)+0x60) return false;
    if (!width || !height || width>8192 || height>8192)
        throw std::runtime_error("Headset-sized native viewport is outside the validated stereo extent");
    native_width=width;native_height=height;
    return true;
}
}
