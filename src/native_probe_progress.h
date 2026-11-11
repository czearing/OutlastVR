#pragma once
#include <cstdint>

namespace olvr {
inline bool native_probe_jump_pressed(std::uint32_t world,bool startup_pressed) {
    if (!world) return startup_pressed;
    return (world>=180 && world<185) || (world>=360 && world<365) ||
        (world>=540 && world<545) || (world>=800 && world<805);
}

class NativeProbeProgress {
    bool first_enter_sent_=false,gameplay_observed_=false;
    std::uint32_t continue_attempts_=0,last_attempt_=0,gameplay_frames_=0;
public:
    bool observe(std::uint32_t submitted,bool world_panel) {
        if (first_enter_sent_ && !world_panel) {
            gameplay_observed_=true;
            ++gameplay_frames_;
        }
        if (!first_enter_sent_ && submitted>=150) {
            first_enter_sent_=true;
            return true;
        }
        if (!gameplay_observed_ && world_panel && submitted>=300 && continue_attempts_<4 &&
            (!continue_attempts_ || submitted-last_attempt_>=120)) {
            ++continue_attempts_;
            last_attempt_=submitted;
            return true;
        }
        return false;
    }
    std::uint32_t gameplay_frames() const {return gameplay_frames_;}
    std::uint32_t continue_attempts() const {return continue_attempts_;}
    bool startup_only() const {return !gameplay_observed_;}
    bool complete() const {return gameplay_frames_>=900;}
    bool expired(std::uint32_t submitted) const {return submitted>=2400 && !complete();}
};
}
