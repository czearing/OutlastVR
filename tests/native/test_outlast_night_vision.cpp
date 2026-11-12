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
        for (const unsigned hand:{0u,1u}) {
            OutlastNightVision native;
            MotionFrame tracked{};tracked.focused=true;tracked.hands[hand].grip_tracked=true;
            UINT64 time=0;
            const auto update=[&](BYTE mode,bool pressed) {
                tracked.hands[hand].stick_click=pressed;time+=1000;
                return native.update_native(tracked,mode,true,time,hand);
            };
            require(update(0,false)==false);
            require(update(0,true)==true);
            require(!update(3,true).has_value());
            require(!update(4,false).has_value());
            require(!update(255,true).has_value());
            require(update(0,true)==false);
            require(update(0,false)==false);
            require(update(0,true)==true);
            require(!update(3,true).has_value());
            require(update(2,true)==true);
            require(update(2,false)==true);
            require(update(2,true)==false);
            require(!update(4,true).has_value());
            require(update(1,true)==false);
            require(update(1,false)==false);
            require(update(1,true)==true);
        }
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
        filter.synchronize(false);
        require(!filter.update(motion,true,2000));
        motion.hands[1].stick_click=false;
        require(!filter.update(motion,true,3000));
        motion.hands[1].stick_click=true;
        require(filter.update(motion,true,4000));
        struct NativeEffects {unsigned mode=0;INT powered=0;} effects;
        const auto native_night=+[](void* object,INT powered) {
            auto& target=*static_cast<NativeEffects*>(object);
            target.mode=2;target.powered=powered;
        };
        const auto native_normal=+[](void* object) {static_cast<NativeEffects*>(object)->mode=0;};
        const auto native_camera=+[](void* object) {static_cast<NativeEffects*>(object)->mode=1;};
        outlast_native_night_effect(&effects,true,false,native_night,native_normal,native_camera);
        require(effects.mode==2 && effects.powered==1);
        outlast_native_night_effect(&effects,false,false,native_night,native_normal,native_camera);
        require(effects.mode==0);
        outlast_native_night_effect(&effects,false,true,native_night,native_normal,native_camera);
        require(effects.mode==1);
        std::array<UINT,3> lights{2,2,2};
        const auto light_switch=+[](void* light,UINT active) {*static_cast<UINT*>(light)=active;};
        outlast_native_night_lights({&lights[0],&lights[1],&lights[2]},true,light_switch);
        require(lights==std::array<UINT,3>{1,0,0});
        outlast_native_night_lights({&lights[0],&lights[1],&lights[2]},false,light_switch);
        require(lights==std::array<UINT,3>{0,0,1});
        bool light_rejected=false;
        try {outlast_native_night_lights({&lights[0],nullptr,&lights[2]},true,light_switch);}
        catch (const std::invalid_argument&) {light_rejected=true;}
        require(light_rejected && lights==std::array<UINT,3>{0,0,1});
        bool rejected=false;
        try {outlast_native_night_effect(nullptr,true,false,native_night,native_normal,native_camera);}
        catch (const std::invalid_argument&) {rejected=true;}
        require(rejected);
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
        std::cout<<"PASS: native night/camera/normal effect dispatch; scripted modes preserved; "
            <<"held input drained across native mode recovery for both hands; "
            <<"stock-state synchronization; thumbstick focus/tracking/menu isolation\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
