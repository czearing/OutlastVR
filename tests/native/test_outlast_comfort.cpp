#include "outlast_comfort.h"
#include "outlast_motion_controls.h"
#include "outlast_native_use.h"
#include <iostream>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool condition,const char* message) {
    ++checks;if (!condition) throw std::runtime_error(message);
}
void check_vector(Vec3 actual,Vec3 expected,const char* message) {
    const auto delta=add(actual,scale(expected,-1));
    require(dot(delta,delta)<.000001f,message);
}
}
int main(int argc,char** argv) {
    try {
        const std::string fault=argc==2?argv[1]:"";
        require(argc<=2 && (fault.empty() || fault=="--fault-snap" || fault=="--fault-seated"),
            "Unknown comfort control");
        OutlastComfortSettings options;options.snap_turn_degrees=fault=="--fault-snap"?0.f:30.f;
        OutlastComfort comfort;
        MotionFrame motion{};motion.focused=true;motion.move_active=motion.turn_active=true;
        EyePose head{{.4f,-.2f,.3f},{0,0,0,1}};
        comfort.update(options,motion,head,true,1000);
        motion.turn.x=1;comfort.update(options,motion,head,true,2000);
        check_vector(comfort.pose(head).position,head.position,"Snap turn orbited the roomscale head");
        const Vec3 right_forward{.5f,0,-.8660254f};
        check_vector(rotate(comfort.pose(head).orientation,{0,0,-1}),right_forward,
            "One right snap did not turn exactly thirty degrees");
        const EyePose hand{add(head.position,{.2f,-.3f,-.4f}),{0,0,0,1}};
        check_vector(add(comfort.pose(hand).position,scale(comfort.pose(head).position,-1)),
            rotate(comfort.pose(head).orientation,{.2f,-.3f,-.4f}),
            "Controller and HMD did not use the same snap transform");
        auto stepped=head;stepped.position.x+=.1f;
        check_vector(add(comfort.pose(stepped).position,scale(comfort.pose(head).position,-1)),
            rotate(comfort.pose(head).orientation,{.1f,0,0}),"Roomscale movement used the old heading");
        for (unsigned i=0;i<30;++i) comfort.update(options,motion,head,true,3000+i*1000);
        check_vector(rotate(comfort.pose(head).orientation,{0,0,-1}),right_forward,"Held turn repeated its snap");
        comfort.update(options,motion,head,false,40000);
        comfort.update(options,motion,head,true,41000);
        check_vector(rotate(comfort.pose(head).orientation,{0,0,-1}),right_forward,"Closing a menu replayed held turn");
        motion.turn.x=0;comfort.update(options,motion,head,true,42000);
        motion.turn.x=-1;comfort.update(options,motion,head,true,43000);
        check_vector(rotate(comfort.pose(head).orientation,{0,0,-1}),{0,0,-1},"Fresh opposite turn failed to rearm");
        {
            OutlastComfort interrupted;
            motion.turn.x=0;interrupted.update(options,motion,head,true,1000);
            motion.turn.x=1;interrupted.update(options,motion,head,true,300000);
            interrupted.update(options,motion,head,true,300001);
            check_vector(rotate(interrupted.pose(head).orientation,{0,0,-1}),{0,0,-1},
                "A time gap replayed a held snap turn");
            motion.turn.x=0;interrupted.update(options,motion,head,true,300002);
            motion.turn.x=1;interrupted.update(options,motion,head,true,300003);
            check_vector(rotate(interrupted.pose(head).orientation,{0,0,-1}),right_forward,
                "Snap turn did not recover after an interrupted release");
            motion.turn.x=0;interrupted.update(options,motion,head,true,300004);
            motion.turn.x=1;interrupted.update(options,motion,head,true,290000);
            check_vector(rotate(interrupted.pose(head).orientation,{0,0,-1}),right_forward,
                "A regressed clock applied an armed snap turn");
        }
        comfort.reset();options.seated=fault!="--fault-seated";motion.turn.x=0;
        for (const float height:{-.6f,-.2f,.4f}) {
            head.position.y=height;
            comfort.update(options,motion,head,true,44000);
            check_vector(comfort.pose(head).position,{head.position.x,0,head.position.z},
                "Seated mode changed native eye height with physical posture");
            auto controller=head;controller.position.y-=.4f;
            check_vector(add(comfort.pose(controller).position,scale(comfort.pose(head).position,-1)),
                {0,-.4f,0},"Seated height adjustment detached the controller");
        }
        OutlastRecenter recenter;
        recenter.recenter({EyePose{{-.032f,1.7f,0},{0,0,0,1}},EyePose{{.032f,1.7f,0},{0,0,0,1}}});
        options.movement_reference=MovementReference::main_hand;
        require(!comfort.movement_yaw(options,motion,recenter,head),
            "Missing directional controller silently drove locomotion");
        motion.hands[1].grip_tracked=true;
        motion.hands[1].grip.orientation={0,-.70710678f,0,.70710678f};
        const auto yaw=comfort.movement_yaw(options,motion,recenter,head);
        require(yaw && std::abs(*yaw+1.57079633f)<.0001f,"Controller-relative movement heading is wrong");
        options.left_handed=true;
        require(!comfort.movement_yaw(options,motion,recenter,head),
            "Left-handed heading selected the other physical controller");
        motion.hands[0].grip_tracked=true;
        motion.hands[0].grip.orientation={0,.38268343f,0,.92387953f};
        const auto left_yaw=comfort.movement_yaw(options,motion,recenter,head);
        require(left_yaw && std::abs(*left_yaw-.78539816f)<.0001f,
            "Left main-hand movement used the wrong physical heading");
        options.movement_reference=MovementReference::off_hand;
        const auto off_yaw=comfort.movement_yaw(options,motion,recenter,head);
        require(off_yaw && std::abs(*off_yaw+1.57079633f)<.0001f,
            "Off-hand movement ignored handedness");
        options.movement_reference=MovementReference::head;
        auto looking_down=head;looking_down.orientation={.70710678f,0,0,.70710678f};
        const auto down_yaw=comfort.movement_yaw(options,motion,recenter,looking_down);
        require(down_yaw && std::abs(*down_yaw)<.0001f,
            "Looking vertically destabilized headset-relative movement");
        OutlastMotionControls controls;
        options.left_handed=true;controls.configure(options);
        motion={};motion.focused=true;motion.turn={0,1};
        motion.hands[0].aim_tracked=true;controls.publish(motion,50000);
        controls.movement_yaw(-1.57079633f);
        XINPUT_STATE output{},physical{};
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,50001);
        require(output.Gamepad.sThumbLX==32767 && std::abs(output.Gamepad.sThumbLY)<=1 &&
            output.Gamepad.sThumbRX==0,"Left-handed movement or snap/native-turn isolation failed");
        motion.hands[0].squeeze=1;controls.publish(motion,51000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,51001);
        require(output.Gamepad.wButtons&XINPUT_GAMEPAD_X,"Left-handed interaction did not map to native use");
        motion.turn={1,1};controls.movement_yaw(.5235987756f);
        controls.publish(motion,52000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,52001);
        require(std::abs(output.Gamepad.sThumbLX-8780)<=1 && output.Gamepad.sThumbLY==32767,
            "Rotated diagonal movement was clipped into the wrong direction");
        options.toggle_sprint=true;controls.configure(options);
        motion.hands[1].stick_click=true;controls.publish(motion,53000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,53001);
        require(output.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER,"Click-to-sprint did not start");
        motion.hands[1].stick_click=false;controls.publish(motion,54000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,54001);
        require(output.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER,"Sprint latch required holding the click");
        motion.turn={};controls.publish(motion,55000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,55001);
        require(!(output.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"Neutral stick did not end sprinting");
        motion.turn={0,1};
        auto sprint=[&](UINT64 now) {
            controls.publish(motion,now);
            controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,now+1);
            return (output.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER)!=0;
        };
        motion.hands[1].stick_click=true;
        require(sprint(56000),"Sprint did not restart after releasing movement");
        motion.hands[1].stick_click=false;require(sprint(56100),"Sprint latch dropped before the second click");
        motion.hands[1].stick_click=true;require(!sprint(56200),"Second sprint click did not toggle sprint off");
        motion.hands[1].stick_click=false;sprint(56300);
        motion.hands[1].stick_click=true;require(sprint(56400),"Fresh sprint click did not toggle sprint on");
        controls.movement_yaw(0.f,false);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,56401,true);
        require(!(output.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),"Sprint latch leaked into a menu");
        controls.movement_yaw(0.f,true);
        require(!sprint(56500),"Closing a menu restarted a held sprint click");
        motion.hands[1].stick_click=false;sprint(56600);
        motion.hands[1].stick_click=true;require(sprint(56700),"Sprint did not recover after a menu");
        controls.publish({},56800);
        require(!sprint(56900),"Focus recovery restarted a held sprint click");
        motion.hands[1].stick_click=false;sprint(57000);
        motion.hands[1].stick_click=true;require(sprint(57100),"Sprint did not recover after focus loss");
        controls.movement_yaw({});
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,57101);
        require(!output.Gamepad.sThumbLX && !output.Gamepad.sThumbLY &&
            !(output.Gamepad.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER),
            "Missing directional tracking retained virtual movement or sprint");
        TrackingFrame frame{{},{1,0,0},{0,1,0},{0,0,1},100,{}};
        motion.hands[0].aim_tracked=true;
        motion.hands[0].aim.position={.2f,.3f,.4f};
        const auto ray=outlast_controller_use_ray(frame,motion,51000,51001,true,0);
        require(ray.has_value(),"Left-hand native use ray was rejected");
        check_vector(ray->origin,{20,30,40},"Native use still originated at the right controller");
        motion={};motion.focused=true;
        controls.publish(motion,60000);
        motion.hands[0].stick_click=motion.hands[1].stick_click=true;
        for (unsigned i=0;i<=12;++i) {
            const auto clock=UINT64(70000)+i*100000;
            controls.publish(motion,clock);
            const bool recentered=controls.take_recenter(clock+1);
            require(recentered==(i==10),"Recenter chord fired early, repeated or was lost");
            const auto filtered=controls.sample(clock+1);
            require(!filtered.hands[0].stick_click && !filtered.hands[1].stick_click,
                "Recenter chord leaked sprint/night-vision clicks");
        }
        controls.publish({},1400000);controls.publish(motion,1400001);
        for (unsigned i=0;i<12;++i) {
            controls.publish(motion,1400002+i*100000);
            require(!controls.take_recenter(1400003+i*100000),
                "Focus regain turned a held chord into recentering");
        }
        for (bool left_handed:{false,true}) for (bool toggle_sprint:{false,true})
        for (unsigned released=0;released<2;++released) {
            OutlastMotionControls chord;
            OutlastComfortSettings chord_options;
            chord_options.left_handed=left_handed;chord_options.toggle_sprint=toggle_sprint;
            chord.configure(chord_options);
            MotionFrame held{};held.focused=true;
            held.move=held.turn={0,1};
            chord.publish(held,1000);
            held.hands[0].stick_click=held.hands[1].stick_click=true;
            chord.publish(held,2000);
            held.hands[released].stick_click=false;
            chord.publish(held,3000);
            const auto tail=chord.sample(3001);
            require(!tail.hands[0].stick_click && !tail.hands[1].stick_click,
                "Partially released recenter chord leaked sprint/night vision");
            XINPUT_STATE merged{},real{};
            real.Gamepad.wButtons=XINPUT_GAMEPAD_A;
            chord.state(0,&merged,ERROR_SUCCESS,real,3001);
            require((merged.Gamepad.wButtons&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_LEFT_SHOULDER|
                XINPUT_GAMEPAD_RIGHT_THUMB))==XINPUT_GAMEPAD_A,
                "Chord suppression leaked virtual actions or swallowed the physical gamepad");
            held.hands[released].stick_click=true;
            for (unsigned i=0;i<12;++i) {
                chord.publish(held,4000+i*100000);
                require(!chord.take_recenter(4001+i*100000),
                    "Partial chord release rearmed recenter without a full release");
            }
            held.hands[0].stick_click=held.hands[1].stick_click=false;
            chord.publish(held,1205000);
            held.hands[1-released].stick_click=true;
            chord.publish(held,1206000);
            require(chord.sample(1206001).hands[1-released].stick_click,
                "Full chord release did not recover an ordinary stick click");
        }
        for (bool left_handed:{false,true}) for (float height:{0.f,1.7f})
        for (float yaw:{-.8f,0.f,.8f}) {
            OutlastComfort crouched;
            OutlastComfortSettings crouch_options;
            crouch_options.snap_turn_degrees=30;crouch_options.left_handed=left_handed;
            MotionFrame turn{};turn.focused=true;turn.move_active=turn.turn_active=true;
            EyePose low{{.4f,height-.45f,.3f},
                multiply({0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)},
                    {std::sin(.2f),0,0,std::cos(.2f)})};
            crouched.update(crouch_options,turn,low,true,1000);
            const auto before=crouched.pose(low);
            (left_handed?turn.move:turn.turn).x=1;
            crouched.update(crouch_options,turn,low,true,2000);
            check_vector(crouched.pose(low).position,before.position,
                "Crouched snap turn introduced lateral camera motion or changed eye height");
            auto left=low,right=low;
            left.position=add(low.position,rotate(low.orientation,{-.032f,0,0}));
            right.position=add(low.position,rotate(low.orientation,{.032f,0,0}));
            const auto a=crouched.pose(left).position,b=crouched.pose(right).position;
            check_vector(scale(add(a,b),.5f),before.position,"Crouched snap shifted the binocular midpoint");
            const auto delta=add(b,scale(a,-1));
            require(std::abs(std::sqrt(dot(delta,delta))-.064f)<.00001f,
                "Crouched snap changed the physical IPD");
            low.orientation={0,std::sin((yaw+.5f)*.5f),0,std::cos((yaw+.5f)*.5f)};
            (left_handed?turn.move:turn.turn).x=0;
            crouched.update(crouch_options,turn,low,true,3000);
            check_vector(crouched.pose(low).position,before.position,
                "Rotating the crouched head caused camera drift");
        }
        options.vignette_strength=.8f;motion.turn={1,0};
        require(OutlastComfort::vignette(options,motion,true)==.8f &&
            OutlastComfort::vignette(options,motion,false)==0,
            "Movement vignette affected menus or ignored left-handed movement");
        std::cout<<"PASS: comfort_checks="<<checks
            <<"; snap_pivot_and_hysteresis; coherent_controller_roomscale; seated_height; "
            <<"controller_relative_movement; left_handed_use; recenter_chord; focus_rearm; vignette\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
