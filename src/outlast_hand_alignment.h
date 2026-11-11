#pragma once
#include "xr_view_constants.h"
#include <cwchar>

namespace olvr {
inline constexpr float outlast_default_hand_wristward_m=.02f;
inline float outlast_hand_offset_cm(const wchar_t* text) {
    if (!text || !*text) throw std::invalid_argument("Empty hand offset; expected centimeters in [-6,6]");
    wchar_t* end=nullptr;
    const float centimeters=std::wcstof(text,&end);
    if (end==text || *end || !std::isfinite(centimeters) || std::abs(centimeters)>6)
        throw std::invalid_argument("Invalid OLVR_HAND_WRISTWARD_CM; expected finite centimeters in [-6,6]");
    return centimeters*.01f;
}
inline EyePose outlast_aligned_hand(EyePose grip,float wristward_m=outlast_default_hand_wristward_m) {
    const auto q=grip.orientation;
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if (!finite(grip.position) || !std::isfinite(norm) || std::abs(norm-1)>.001f ||
        !std::isfinite(wristward_m) || std::abs(wristward_m)>.06f)
        throw std::invalid_argument("Invalid controller-local hand alignment");
    if (wristward_m!=0) grip.position=add(grip.position,rotate(q,{0,wristward_m,0}));
    return grip;
}
}
