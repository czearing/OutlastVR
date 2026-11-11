#include "outlast_night_vision.h"
#include "outlast_motion_controls.h"
#include <iostream>

using namespace olvr;
int main() {
    try {
        auto require=[](bool v) {if (!v) throw std::runtime_error("Night-vision input regression");};
        OutlastNightVision filter;
        MotionFrame motion{};motion.focused=true;motion.hands[1].grip_tracked=true;
        motion.hands[1].stick_click=true;
        require(!filter.update(motion,true,1000));
        motion.hands[1].stick_click=false;
        require(!filter.update(motion,true,2000));
        motion.hands[1].stick_click=true;
        require(filter.update(motion,true,3000));
        require(filter.update(motion,true,4000));
        motion.hands[1].stick_click=false;filter.update(motion,true,5000);
        motion.hands[1].stick_click=true;
        require(filter.update(motion,false,6000));
        require(filter.update(motion,true,7000));
        motion.hands[1].stick_click=false;filter.update(motion,true,8000);
        motion.hands[1].stick_click=true;
        require(!filter.update(motion,true,9000));
        motion.focused=false;filter.update(motion,true,10000);
        motion.focused=true;
        require(!filter.update(motion,true,11000));
        motion.hands[1].stick_click=false;filter.update(motion,true,12000);
        motion.hands[1].stick_click=true;
        require(!filter.update(motion,true,500000));
        motion.hands[1].stick_click=false;filter.update(motion,true,501000);
        motion.hands[1].grip_tracked=false;
        motion.hands[1].stick_click=true;
        require(!filter.update(motion,true,502000));
        motion.hands[1].grip_tracked=true;
        require(!filter.update(motion,true,503000));
        motion.hands[1].stick_click=false;filter.update(motion,true,504000);
        motion.hands[1].stick_click=true;
        require(filter.update(motion,true,505000));
        require(filter.update(motion,true,1));
        OutlastMotionControls controls;
        controls.publish(motion,1000);
        XINPUT_STATE output{},physical{};
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,1001,false,true,false);
        require(!(output.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB));
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,1002,false,true,true);
        require((output.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB)!=0);
        physical.Gamepad.wButtons=XINPUT_GAMEPAD_RIGHT_THUMB;
        controls.state(0,&output,ERROR_SUCCESS,physical,1003,false,true,false);
        require((output.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB)!=0);
        std::cout<<"PASS: thumbstick edge toggles, menu/focus/tracking/stale input isolation and experimental native routing\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
