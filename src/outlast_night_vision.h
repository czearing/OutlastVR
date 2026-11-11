#pragma once
#include "openxr_motion_input.h"

namespace olvr {

class OutlastNightVision {
    bool enabled_=false,blocked_=true,down_=false;
    UINT64 clock_=0;
public:
    bool update(const MotionFrame& motion,bool gameplay,UINT64 now) {
        const bool current=motion.focused && motion.hands[1].grip_tracked &&
            (!clock_ || (now>=clock_ && now-clock_<=250000));
        clock_=now;
        const bool pressed=motion.hands[1].stick_click;
        if (!current || !gameplay) blocked_=true;
        else if (!pressed) blocked_=false;
        if (current && gameplay && !blocked_ && pressed && !down_) enabled_=!enabled_;
        down_=pressed;
        return enabled_;
    }
};

}
