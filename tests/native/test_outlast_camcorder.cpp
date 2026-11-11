#include "outlast_camcorder.h"
#include <iostream>
#include <string>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool close(Vec3 a,Vec3 b,float tolerance=1e-5f) {
    const auto d=add(a,scale(b,-1));return dot(d,d)<tolerance*tolerance;
}
MotionFrame tracked(Vec3 position) {
    MotionFrame frame{};frame.focused=true;
    for (auto& hand:frame.hands) {
        hand.grip_tracked=true;
        hand.grip.position={position.x,position.y,position.z};
    }
    return frame;
}
}
int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-hmd-lens";
        const bool fault_gc=argc==2 && std::string(argv[1])=="--fault-gc-reset";
        if (argc!=1 && !fault && !fault_gc) throw std::invalid_argument("Unknown camcorder control");
        const EyePose head{{0,1.7f,0},{0,0,0,1}};
        const auto chest=camcorder_holster(head,{0,0,0,1});
        require(close(chest.position,{.11f,1.38f,-.16f}),"Chest attachment is not measured from the head");
        OutlastCamcorder camera;
        auto frame=tracked(chest.position);
        CamcorderNativeState native{true,true,false,false};
        auto output=camera.update(frame,head,{0,0,0,1},native,1);
        require(output.visible && !output.held && !output.request_toggle,"Unpressed chest camera unexpectedly acquired");
        frame.hands[1].squeeze=1;
        output=camera.update(frame,head,{0,0,0,1},native,2);
        require(output.held && output.hand==1 && output.request_toggle,"Chest squeeze did not acquire camera");
        output=camera.update(frame,head,{0,0,0,1},native,3);
        require(!output.request_toggle,"Unacknowledged native raise was repeated");
        native.raised=true;
        frame.hands[1].grip.position={.4f,1.55f,-.5f};
        output=camera.update(frame,head,{0,0,0,1},native,4);
        require(output.held && !output.request_toggle,"Moving held camera lost ownership");
        require(close(rotate(output.pose.orientation,{0,0,-1}),{0,0,-1}),
            "Unavailable aim did not retain the standard forward fallback");
        frame.hands[1].aim_tracked=true;
        frame.hands[1].aim.orientation={0,.707106781f,0,.707106781f};
        output=camera.update(frame,head,{0,0,0,1},native,4);
        require(close(rotate(output.pose.orientation,{0,0,-1}),{-1,0,0}),
            "Camera ignored actual aim when grip and aim differ");
        frame.hands[1].squeeze=0;
        output=camera.update(frame,head,{0,0,0,1},native,5);
        require(!output.held && output.request_toggle && close(output.pose.position,chest.position),
            "Released camera did not return to chest/request lowering");
        native.raised=false;
        camera.update(frame,head,{0,0,0,1},native,6);
        frame.hands[1].squeeze=1;
        output=camera.update(frame,head,{0,0,0,1},native,7);
        require(!output.held && !output.request_toggle,"Remote squeeze acquired the chest camera");
        frame.hands[1].squeeze=0;
        camera.update(frame,head,{0,0,0,1},native,8);
        frame.hands[1].grip.position={chest.position.x,chest.position.y,chest.position.z};
        frame.hands[1].squeeze=1;
        output=camera.update(frame,head,{0,0,0,1},native,9);
        require(output.held,"Camera could not be reacquired after release");
        frame.focused=false;
        output=camera.update(frame,head,{0,0,0,1},native,10);
        require(!output.visible && !output.held && !output.request_toggle,"Focus loss retained camera actions");
        frame.focused=true;
        output=camera.update(frame,head,{0,0,0,1},native,11);
        require(!output.held,"Held squeeze reacquired on focus regain without release");
        frame.hands[1].squeeze=0;
        camera.update(frame,head,{0,0,0,1},native,12);
        frame.hands[1].squeeze=1;
        native.available=false;
        output=camera.update(frame,head,{0,0,0,1},native,13);
        require(!output.held && !output.visible && !output.request_toggle,"Grab bypassed native inventory");
        native.available=true;
        output=camera.update(frame,head,{0,0,0,1},native,14);
        require(!output.held,"Inventory regain reused an old grab edge");
        frame.hands[1].squeeze=0;
        camera.update(frame,head,{0,0,0,1},native,15);
        frame.hands[1].squeeze=1;
        output=camera.update(frame,head,{0,0,0,1},native,16);
        require(output.request_toggle,"Fresh grab failed after inventory regain");
        output=camera.update(frame,head,{0,0,0,1},native,2000017);
        require(output.request_rejected && !output.request_toggle && !output.held,
            "Rejected native request retained a working camera or was not surfaced/bounded");
        for (UINT64 clock=2000018;clock<2000118;++clock) {
            output=camera.update(frame,head,{0,0,0,1},native,clock);
            require(!output.request_toggle && !output.request_rejected,"Rejected request retried or spammed its diagnostic");
        }
        frame.hands[1].grip_tracked=false;
        output=camera.update(frame,head,{0,0,0,1},native,2000118);
        require(!output.held,"Tracking loss retained camera ownership");
        frame.hands[1].grip_tracked=true;
        output=camera.update(frame,head,{0,0,0,1},native,2000119);
        require(!output.held,"Tracking regain reused a held squeeze");
        unsigned invalid=0;
        try {camera.update(frame,head,{0,0,0,1},native,1);}
        catch (const std::invalid_argument&) {++invalid;}
        try {camcorder_holster(head,{.2f,0,0,.9797959f});}
        catch (const std::invalid_argument&) {++invalid;}
        try {camcorder_offset(head,{0,0,0},{0,0,0,2});}
        catch (const std::invalid_argument&) {++invalid;}
        require(invalid==3,"Invalid clock/torso/rotation was accepted");
        for (unsigned holding=0;holding<2;++holding) {
            OutlastCamcorder other;
            auto hands=tracked(chest.position);
            CamcorderNativeState available{true,true,false,false};
            other.update(hands,head,{0,0,0,1},available,1);
            hands.hands[holding].squeeze=1;
            const auto acquired=other.update(hands,head,{0,0,0,1},available,2);
            require(acquired.held && acquired.hand==int(holding),"One hand cannot acquire the camcorder");
            const auto expected=camcorder_grip(hands.hands[holding].grip,holding);
            require(close(acquired.pose.position,expected.position),"Holding-hand attachment is reversed");
            available.transitioning=true;
            if (fault_gc) other=OutlastCamcorder{};
            const auto during_gc=other.update(hands,head,{0,0,0,1},available,3);
            require(during_gc.held && during_gc.hand==int(holding) && !during_gc.request_toggle,
                "Native GC erased the held camera's grip edge or repeated its pending raise");
            available.transitioning=false;available.raised=true;
            const auto after_gc=other.update(hands,head,{0,0,0,1},available,4);
            require(after_gc.held && after_gc.hand==int(holding) && !after_gc.request_toggle,
                "Native GC did not retain the acknowledged holding hand");
            available.raised=false;available.controls_allowed=false;
            const auto cutscene=other.update(hands,head,{0,0,0,1},available,5);
            require(!cutscene.held && !cutscene.visible && !cutscene.request_toggle,
                "Scripted input lock did not relinquish camera ownership");
            available.controls_allowed=true;
            const auto regained=other.update(hands,head,{0,0,0,1},available,6);
            require(!regained.held && !regained.request_toggle,
                "Scripted input unlock reused an old squeeze");
        }
        const std::array<EyeFov,2> fovs{{{-.8f,.7f,.8f,-.7f},{-.7f,.8f,.8f,-.7f}}};
        const std::array<EyePose,2> eyes{{{{-.032f,1.7f,0},{0,0,0,1}},
                                       {{.032f,1.7f,0},{0,0,0,1}}}};
        for (unsigned frame_index=0;frame_index<60;++frame_index) {
            const float yaw=frame_index*.031f;
            EyePose screen{{.1f+frame_index*.001f,1.6f,-.4f},{0,std::sin(yaw/2),0,std::cos(yaw/2)}};
            const auto projected=camcorder_screen_constants(screen,eyes,fovs);
            for (unsigned eye=0;eye<2;++eye) for (float px:{-1.f,0.f,1.f}) for (float py:{-1.f,0.f,1.f}) {
                const double local_x=px*.045,local_y=py*.0253125;
                const double x=screen.position.x+std::cos(yaw)*local_x-eyes[eye].position.x;
                const double y=screen.position.y+local_y-eyes[eye].position.y;
                const double z=screen.position.z-std::sin(yaw)*local_x;
                const double left=std::tan(double(fovs[eye].left)),right=std::tan(double(fovs[eye].right));
                const double up=std::tan(double(fovs[eye].up)),down=std::tan(double(fovs[eye].down));
                const double clip_x=2*x/(right-left)+(right+left)*z/(right-left);
                const double clip_y=2*y/(up-down)+(up+down)*z/(up-down);
                const auto& m=projected[eye];
                require(std::abs(m[0]*px+m[1]*py+m[3]-clip_x)<1e-5 &&
                        std::abs(m[4]*px+m[5]*py+m[7]-clip_y)<1e-5 &&
                        std::abs(m[12]*px+m[13]*py+m[15]+z)<1e-5,
                    "Physical viewfinder plane does not match independent per-eye projection");
            }
        }
        for (unsigned degree=0;degree<360;++degree) {
            const float angle=degree*.017453292519943295f;
            const TrackingFrame world{{100,200,300},{0,1,0},{0,0,1},{-1,0,0},100,{0,0,0}};
            const auto orientation=bone_multiply({0,std::sin(angle/2),0,std::cos(angle/2)},
                bone_multiply({std::sin(.3f),0,0,std::cos(.3f)},{0,0,std::sin(.2f),std::cos(.2f)}));
            const EyePose optical{{.2f,-.1f,-.4f},orientation};
            const auto rotator=camcorder_native_rotation(world,optical);
            constexpr double radians=6.2831853071795864769/65536;
            const double pitch=rotator[0]*radians,yaw=rotator[1]*radians,roll=rotator[2]*radians;
            const Vec3 native_forward{float(std::cos(pitch)*std::cos(yaw)),
                float(std::cos(pitch)*std::sin(yaw)),float(std::sin(pitch))};
            const Vec3 native_up{float(-std::cos(roll)*std::sin(pitch)*std::cos(yaw)-std::sin(roll)*std::sin(yaw)),
                float(std::cos(yaw)*std::sin(roll)-std::cos(roll)*std::sin(pitch)*std::sin(yaw)),
                float(std::cos(roll)*std::cos(pitch))};
            require(close(native_forward,map_tracking(world,rotate(orientation,{0,0,-1})),.0003f) &&
                close(native_up,map_tracking(world,rotate(orientation,{0,1,0})),.0003f),
                "Independent native Rotator reconstruction differs from the tracked optical axes");
            auto hand=frame.hands[1].grip;
            hand.orientation={0,std::sin(angle/2),0,std::cos(angle/2)};
            const auto body=camcorder_grip(hand,1);
            auto lens=camcorder_offset(body,{0,.015f,-.09f});
            if (fault) lens=head;
            const auto position=hand.position;
            const Vec3 base{position.x,position.y,position.z};
            const Vec3 local{-.045f,.03f,-.09f};
            const Vec3 expected{base.x+std::cos(angle)*local.x+std::sin(angle)*local.z,
                base.y+local.y,base.z-std::sin(angle)*local.x+std::cos(angle)*local.z};
            require(close(lens.position,expected),"Viewfinder source is not at the independent tracked lens");
            const auto screen=camcorder_offset(body,{-.075f,.015f,.015f});
            const auto forward=rotate(lens.orientation,{0,0,-1});
            const auto origin_delta=add(screen.position,scale(lens.position,-1));
            require(std::abs(dot(origin_delta,forward)+.105f)<1e-5f,"Screen/lens geometry drifted");
        }
        std::cout<<"PASS: camcorder_checks="<<checks
            <<"; chest_grab; either_hand; native_inventory_gate; release_to_holster; tracked_lens_not_HMD;"
            <<" bounded_native_request; focus_tracking_release_required; physical_screen_geometry\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
