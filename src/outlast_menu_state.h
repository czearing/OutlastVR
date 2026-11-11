#pragma once
#include <cstdint>

namespace olvr {
inline bool outlast_interactive_movie(int visible,int input,std::uint32_t flags) noexcept {
    return visible!=0 && input!=0 && (flags&0x18000)!=0 && (flags&0x20000)==0;
}
}
