#include "outlast_motion_controls.h"
#include <iostream>
#include <thread>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    try {
        OutlastMotionControls controls;
        XINPUT_STATE output{},physical{};
        require(controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,1)==ERROR_SUCCESS &&
            output.Gamepad.wButtons==0,"Virtual pad absent before first focused frame");
        MotionFrame frame{};
        frame.focused=true;frame.time=1000;frame.move={1,-1};frame.turn={-.5f,1};
        frame.hands[1].primary=true;frame.hands[1].trigger=.8f;
        frame.hands[1].aim_tracked=true;
        frame.hands[0].squeeze=.8f;frame.hands[1].squeeze=.8f;
        frame.hands[0].stick_click=true;
        controls.publish(frame,1000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,1001);
        require(output.Gamepad.sThumbLX==32767 && output.Gamepad.sThumbLY==-32768 &&
            output.Gamepad.sThumbRX==-16384 && output.Gamepad.sThumbRY==0,
            "Native movement/turn axes differ or VR drives native pitch");
        const WORD buttons=XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_LEFT_SHOULDER|
            XINPUT_GAMEPAD_DPAD_UP;
        require(output.Gamepad.wButtons==buttons,"Native action mapping differs");
        const auto packet=output.dwPacketNumber;
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,1002);
        require(output.dwPacketNumber==packet,"Unchanged input advanced packet number");
        frame.hands[1].trigger=.5f;controls.publish(frame,2000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,2001);
        require((output.Gamepad.wButtons&XINPUT_GAMEPAD_X)!=0,"Trigger hysteresis released early");
        frame.hands[1].trigger=.4f;controls.publish(frame,3000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,3001);
        require((output.Gamepad.wButtons&XINPUT_GAMEPAD_X)==0,"Trigger release latched");
        physical.Gamepad.wButtons=XINPUT_GAMEPAD_DPAD_LEFT;
        physical.Gamepad.sThumbRY=1234;
        controls.state(0,&output,ERROR_SUCCESS,physical,3002);
        require((output.Gamepad.wButtons&XINPUT_GAMEPAD_DPAD_LEFT) && output.Gamepad.sThumbRY==1234,
            "Physical controller zero input was lost");
        physical.dwPacketNumber=777;
        controls.state(1,&output,ERROR_SUCCESS,physical,3003);
        require(std::memcmp(&output,&physical,sizeof(output))==0,"Unrelated controller changed");
        require(controls.state(2,&output,ERROR_DEVICE_NOT_CONNECTED,physical,3003)==ERROR_DEVICE_NOT_CONNECTED,
            "Unrelated controller falsely connected");
        for (const auto clock:{UINT64(253001),UINT64(2999)}) {
            controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock);
            require(output.Gamepad.wButtons==0 && output.Gamepad.sThumbLX==0 &&
                !controls.sample(clock).focused,"Expired or regressed timestamp retained input");
        }
        controls.publish({},4000);
        controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4001);
        require(output.Gamepad.wButtons==0 && output.Gamepad.sThumbRX==0,
            "Skipped/unfocused frame retained input");
        {
            OutlastMotionControls menu;
            MotionFrame mapped{};mapped.focused=true;
            mapped.hands[1].primary=true;mapped.hands[1].trigger=1;
            mapped.hands[1].aim_tracked=true;
            menu.publish(mapped,4100);
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4101,true);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_A,
                "Menu ray trigger leaked gameplay interaction or suppressed native confirmation");
            const auto menu_packet=output.dwPacketNumber;
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4102,true);
            require(output.dwPacketNumber==menu_packet,"Unchanged menu input advanced its packet");
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4103,false);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_A,
                "Held menu click became gameplay interaction on scene transition");
            mapped.hands[1].trigger=.5f;menu.publish(mapped,4200);
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4201);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_A,
                "Half-released menu click bypassed gameplay release hysteresis");
            mapped.hands[1].trigger=0;menu.publish(mapped,4300);
            mapped.hands[1].trigger=1;menu.publish(mapped,4400);
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4401);
            require(output.Gamepad.wButtons==(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X),
                "A fresh gameplay trigger remained blocked after menu release");
            XINPUT_STATE external{};external.Gamepad.wButtons=XINPUT_GAMEPAD_X;
            menu.state(0,&output,ERROR_SUCCESS,external,4402,true);
            require(output.Gamepad.wButtons==(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X),
                "Menu isolation changed a real controller's interaction button");
            menu.publish({},4500);
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4501,true);
            require(output.Gamepad.wButtons==0,"Unfocused menu retained virtual input");
            menu.publish(mapped,4600);
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4601,true);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_A,
                "Input polling before pointer reacquisition leaked a held menu trigger");
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4602,false);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_A,
                "Reacquired held menu trigger became a gameplay action after closing");
            mapped.hands[1].trigger=0;menu.publish(mapped,4700);
            mapped.hands[1].trigger=1;menu.publish(mapped,4800);
            menu.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4801);
            require(output.Gamepad.wButtons==(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X),
                "Reacquisition latch blocked a genuinely fresh gameplay interaction");
        }
        for (unsigned interruption=0;interruption<4;++interruption) {
            OutlastMotionControls interaction;
            MotionFrame held{};held.focused=true;held.hands[1].aim_tracked=true;
            interaction.publish(held,1000);
            held.hands[1].trigger=1;interaction.publish(held,1100);
            interaction.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,1101);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_X,"Fresh tracked use did not start");
            auto lost=held;
            UINT64 clock=1200;
            if (interruption==0) lost.hands[1].aim_tracked=false;
            if (interruption==1) lost.focused=false;
            if (interruption==2) clock=252000;
            interaction.publish(lost,clock);
            interaction.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+1,false,interruption!=3);
            require(output.Gamepad.wButtons==0,"Tracking/focus/time/native-pose interruption retained virtual use");
            interaction.publish(held,clock+100);
            interaction.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+101);
            require(output.Gamepad.wButtons==0,"Reacquired held trigger became a phantom interaction");
            held.hands[1].trigger=.5f;interaction.publish(held,clock+200);
            interaction.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+201);
            require(output.Gamepad.wButtons==0,"Half release rearmed an interrupted native use");
            held.hands[1].trigger=.4f;interaction.publish(held,clock+300);
            held.hands[1].trigger=1;interaction.publish(held,clock+400);
            interaction.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+401);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_X,"Fresh release/press did not recover native use");
            XINPUT_STATE external{};external.Gamepad.wButtons=XINPUT_GAMEPAD_X;
            interaction.state(0,&output,ERROR_SUCCESS,external,clock+402,false,false);
            require(output.Gamepad.wButtons==XINPUT_GAMEPAD_X,"Use readiness changed a physical controller's input");
        }
        {
            OutlastMotionControls bindings;
            MotionFrame mapped{};mapped.focused=true;mapped.hands[0].squeeze=1;
            mapped.hands[1].aim_tracked=true;
            auto buttons=[&] {
                bindings.publish(mapped,4500);
                bindings.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,4501);
                return output.Gamepad.wButtons;
            };
            require(buttons()==0,"Left grip still sprints instead of remaining available for grabbing");
            mapped.hands[1].squeeze=1;
            require(buttons()==0,"Right grip still toggles the camera without a physical chest grab");
            mapped.hands[0].stick_click=true;
            require(buttons()==XINPUT_GAMEPAD_LEFT_SHOULDER,
                "Left thumbstick click did not sprint exclusively");
            mapped.hands[0].stick_click=false;mapped.hands[0].primary=true;
            require(buttons()==XINPUT_GAMEPAD_BACK,"Left primary did not open notes exclusively");
            mapped.hands[0].primary=false;mapped.hands[1].trigger=1;
            require(buttons()==XINPUT_GAMEPAD_X,"Right trigger did not retain native interaction");
        }
        {
            OutlastMotionControls feedback;
            MotionFrame tracked{};tracked.focused=true;
            tracked.hands[0].grip_tracked=tracked.hands[1].grip_tracked=true;
            feedback.publish(tracked,10000);
            XINPUT_VIBRATION motors{32768,65535};
            require(feedback.vibration(0,&motors,ERROR_DEVICE_NOT_CONNECTED,10001)==ERROR_SUCCESS &&
                feedback.rumble(10002)==std::array<float,2>{1,1},
                "Actual low/high-frequency motor output did not reach both tracked VR hands");
            motors={65535,0};
            feedback.vibration(0,&motors,ERROR_SUCCESS,10003);
            require(feedback.rumble(10004)==std::array<float,2>{1,1},
                "Low-frequency gamepad motor incorrectly selected only the left hand");
            motors={};
            require(feedback.vibration(1,&motors,ERROR_DEVICE_NOT_CONNECTED,10005)==ERROR_DEVICE_NOT_CONNECTED &&
                feedback.rumble(10006)==std::array<float,2>{1,1},
                "Unrelated pad changed the virtual controller's rumble");
            require(feedback.vibration(0,&motors,ERROR_BAD_ARGUMENTS,10007)==ERROR_BAD_ARGUMENTS &&
                feedback.vibration(0,nullptr,ERROR_SUCCESS,10007)==ERROR_BAD_ARGUMENTS &&
                feedback.rumble(10008)==std::array<float,2>{1,1},
                "Invalid vibration calls changed the pending feedback");
            feedback.vibration(0,&motors,ERROR_SUCCESS,10009);
            require(feedback.rumble(10010)==std::array<float,2>{0,0},"Native zero motors failed to stop rumble");
            motors={65535,0};feedback.vibration(0,&motors,ERROR_SUCCESS,10011);
            tracked.hands[0].grip_tracked=false;feedback.publish(tracked,10012);
            require(feedback.rumble(10013)==std::array<float,2>{0,1},
                "Lost controller tracking retained vibration on the missing hand");
            require(feedback.rumble(260013)==std::array<float,2>{0,0} &&
                feedback.rumble(10011)==std::array<float,2>{0,0},
                "Expired/regressed input retained native feedback");
            feedback.publish(tracked,260014);
            require(feedback.rumble(260015)==std::array<float,2>{0,0},
                "Render pause resumed an old native vibration command");
            feedback.vibration(0,&motors,ERROR_SUCCESS,260016);feedback.publish({},260017);
            feedback.publish(tracked,260018);
            require(feedback.rumble(260019)==std::array<float,2>{0,0},
                "Focus regain replayed stale native rumble");
        }
        std::array<unsigned char,500*4> pads{};
        for (const bool poll_first:{false,true}) {
            OutlastMotionControls ordered;
            MotionFrame input{};input.focused=true;
            UINT64 clock=10000000;
            ordered.publish(input,clock);
            ordered.physical_stance(true,-.5f,false,false,clock+1);
            ordered.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+2);
            ordered.physical_stance(true,-.5f,true,false,clock+3);
            unsigned crouch_edges=0;
            for (unsigned tick=0;tick<120;++tick) {
                clock+=10000;
                input.hands[1].secondary=tick<90;
                ordered.publish(input,clock);
                if (poll_first) {
                    ordered.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+1);
                    crouch_edges+=(output.Gamepad.wButtons&XINPUT_GAMEPAD_B)!=0;
                }
                ordered.physical_stance(true,tick<100?-.5f:0.f,true,false,clock+2);
                ordered.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+3);
                crouch_edges+=(output.Gamepad.wButtons&XINPUT_GAMEPAD_B)!=0;
            }
            require(crouch_edges==0,"Poll ordering let manual crouch reverse the physical/manual OR stance");
            ordered.physical_stance(true,0,false,false,clock+4);
            ordered.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+5);
            require((output.Gamepad.wButtons&XINPUT_GAMEPAD_B)!=0,
                "Native polling consumed manual stance intent before reconciliation");
        }
        const INT kind=3;std::memcpy(pads.data()+8,&kind,4);
        const auto original=pads;
        require(outlast_virtual_pad_presence(pads.data(),4),"Native connection not seeded");
        UINT connected=0;std::memcpy(&connected,pads.data()+0x1f0,4);
        require(connected==1,"Native cached-disconnection gate remains closed");
        auto expected=original;std::memcpy(expected.data()+0x1f0,&connected,4);
        require(pads==expected,"Connection changed unrelated native pad bytes");
        require(!outlast_virtual_pad_presence(nullptr,0),"Empty startup inventory falsely available");
        bool rejected=false;
        try {outlast_virtual_pad_presence(nullptr,4);} catch (const std::runtime_error&) {rejected=true;}
        require(rejected,"Invalid native inventory accepted");
        OutlastMotionControls stance;
        stance.publish(frame,5000);
        auto button=[&](UINT64 now) {
            stance.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,now);
            return (output.Gamepad.wButtons&XINPUT_GAMEPAD_B)!=0;
        };
        require(stance.physical_stance(true,-.5f,false,false,5001) && button(5002),
            "Physical squat did not issue a native crouch edge");
        require(!button(5003),"Physical squat retained its native crouch pulse");
        require(stance.physical_stance(true,-.5f,false,true,5004) && !button(5005),
            "Native crouch transition repeated its toggle");
        require(stance.physical_stance(true,-.5f,true,false,5006) && !button(5007),
            "Accepted native crouch was not acknowledged");
        require(stance.physical_stance(true,-.25f,true,false,5008) && !button(5009),
            "Physical crouch hysteresis released early");
        require(stance.physical_stance(true,0,true,false,5010) && button(5011),
            "Physical standing did not request native uncrouch");
        require(!stance.physical_stance(true,0,false,false,5012) && !button(5013),
            "Physical stance ownership survived completed standing");
        require(stance.physical_stance(true,-.5f,true,false,5014) && !button(5015) &&
            !stance.physical_stance(true,0,true,false,5016) && !button(5017),
            "Physical bending took ownership of or undid manual crouching");
        require(stance.physical_stance(true,-.5f,false,false,5018) && button(5019),
            "Second physical crouch edge absent");
        stance.publish(frame,1605020);
        require(stance.physical_stance(true,-.5f,false,false,1605020) && button(1605021),
            "Rejected crouch did not make its bounded retry");
        stance.publish(frame,3205022);
        stance.physical_stance(true,-.5f,false,false,3205022);
        require(!button(3205023) && stance.take_stance_warning() && !stance.take_stance_warning(),
            "Rejected crouch did not stop retrying and surface its diagnostic");
        stance.publish({},3205024);
        require(!stance.physical_stance(true,-.5f,false,false,3205025) && !button(3205026),
            "Focus loss retained physical stance commands");
        {
            OutlastMotionControls combined;
            MotionFrame input{};input.focused=true;
            UINT64 clock=4000000;
            auto update=[&](float height,bool crouched,bool transition=false) {
                clock+=1000;
                combined.publish(input,clock);
                const bool physical_owned=combined.physical_stance(true,height,crouched,transition,clock+1);
                combined.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+2);
                return std::pair<bool,bool>{physical_owned,(output.Gamepad.wButtons&XINPUT_GAMEPAD_B)!=0};
            };
            require(update(0,false)==std::pair<bool,bool>{false,false},"Standing initialization toggled crouch");
            input.hands[1].secondary=true;
            require(update(0,false)==std::pair<bool,bool>{false,true},"Manual crouch edge absent");
            for (unsigned i=0;i<120;++i)
                require(!update(0,i>1,i<=1).second,"Held manual crouch repeated or reversed its toggle");
            input.hands[1].secondary=false;
            require(!update(-.5f,true).second,"Physical squat reversed existing manual crouch");
            input.hands[1].secondary=true;
            require(!update(-.5f,true).second,"Manual toggle stood up while the headset remained crouched");
            input.hands[1].secondary=false;
            require(update(0,true).second,"Standing after clearing manual crouch did not uncrouch");
            require(update(0,false)==std::pair<bool,bool>{false,false},"Combined stance did not acknowledge standing");
            require(update(-.5f,false).second,"Physical crouch after manual release absent");
            require(!update(-.5f,true).second,"Physical crouch acknowledgment repeated B");
            input.hands[1].secondary=true;
            require(!update(-.5f,true).second,"Manual crouch latched during physical crouch emitted an opposite edge");
            input.hands[1].secondary=false;
            require(update(0,true)==std::pair<bool,bool>{false,false},
                "Physical standing undid latched manual crouch");
            input.hands[1].secondary=true;
            require(update(0,true).second,"Manual standing after combined crouch absent");
            require(!update(0,false).second,"Held manual standing reversed the acknowledged stance");
            input.hands[1].secondary=false;
            require(update(-.5f,false).second,"Focus-loss fixture failed to request crouch");
            combined.publish({},clock+3);
            combined.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+4);
            require(!(output.Gamepad.wButtons&XINPUT_GAMEPAD_B),"Focus loss retained a stance edge");
            require(!update(-.5f,true).second,"Focus regain replayed acknowledged crouch");
            require(update(0,true).second,"Focus regain lost physical stance ownership");
        }
        {
            OutlastMotionControls standalone;
            MotionFrame input{};input.focused=true;input.hands[1].secondary=true;
            for (unsigned i=0;i<120;++i) {
                standalone.publish(input,5000000+i);
                standalone.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,5000000+i);
                require(bool(output.Gamepad.wButtons&XINPUT_GAMEPAD_B)==(i==0),
                    "Manual crouch outside an eligible body state repeated while held");
            }
        }
        std::thread writer([&] {for (unsigned i=0;i<10000;++i) controls.publish(i%2?frame:MotionFrame{},5000+i);});
        for (unsigned i=0;i<10000;++i) {
            const auto snapshot=controls.sample(15000);
            require(snapshot.focused? snapshot.move.x==1 && snapshot.hands[1].primary:
                snapshot.move.x==0 && !snapshot.hands[1].primary,"Torn game/render input snapshot");
        }
        writer.join();
        std::cout<<"PASS: native_motion_checks="<<checks
            <<"; startup_presence; native_actions_axes; physical_controllers_preserved; "
            <<"focus_skip_expiry_neutral; hysteresis; coherent_thread_publication; "
            <<"physical_stance_edges; manual_crouch_preserved; bounded_rejection_retry; "
            <<"thumbstick_sprint; native_motor_feedback; rumble_focus_tracking_expiry_neutral; "
            <<"manual_crouch_single_edge; physical_manual_OR; focus_regain_stance_acknowledgment\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
