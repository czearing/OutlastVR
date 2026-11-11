#pragma once
#include <cstdint>

namespace olvr {

inline bool native_draw_jump_ready(std::uint8_t basic,std::uint8_t special,bool tracked) {
    return tracked && basic==2 && special==3;
}

struct NativeDrawSelection {
    std::uint64_t frame=0;
    bool predicate_driven=false;

    bool selects(std::uint64_t current,bool contact_pose_ready) {
        if (!current) return false;
        if (!frame && predicate_driven && contact_pose_ready) frame=current;
        return frame && frame==current;
    }
};

}
