#include "world_panel.h"
#include "outlast_body_ik.h"
#include "outlast_menu_state.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
float distance(Vec3 a,Vec3 b) {
    const auto d=add(a,scale(b,-1));return std::sqrt(dot(d,d));
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Actual native hero fixture is required");
        require(!outlast_interactive_movie(1,0,0x803),"Passive retail HUD enabled the controller menu");
        require(outlast_interactive_movie(1,1,0x8863),"Actual pause movie was not interactive");
        require(outlast_interactive_movie(1,1,0x886b),"Opening pause movie was not interactive");
        require(outlast_interactive_movie(1,1,0x10000),"Mouse-input consuming movie was not interactive");
        require(!outlast_interactive_movie(0,1,0x8863),"Hidden movie enabled the controller menu");
        require(!outlast_interactive_movie(1,0,0x8863),"Input-disabled movie enabled the controller menu");
        require(!outlast_interactive_movie(1,1,0x28863),"Mouse-disabled movie enabled the controller menu");
        require(!outlast_interactive_movie(1,1,0x803),"Nonconsuming HUD enabled the controller menu");
        const std::array<EyeFov,2> fovs{{{-.8f,.8f,.8f,-.8f},{-.8f,.8f,.8f,-.8f}}};
        for (const float pitch:{-1.570796327f,-1.2f,0.f,1.2f,1.570796327f})
            for (const float roll:{-.8f,0.f,.8f}) {
                const auto q=bone_multiply(Quaternion{std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)},
                    {0,0,std::sin(roll*.5f),std::cos(roll*.5f)});
                std::array<EyePose,2> eyes{};
                for (unsigned eye=0;eye<2;++eye) eyes[eye]={rotate(q,{eye?.032f:-.032f,0,0}),q};
                WorldPanel panel;
                panel.locate(eyes,fovs,16.f/9);
                const auto ui=panel.locate_ui(eyes,fovs,16.f/9);
                for (const auto& eye:ui)
                    for (unsigned row=0;row<4;++row)
                        require(eye[row*4+2]==0,"Retail UI depth moved a button off the controller ray plane");
                const auto forward=rotate(q,{0,0,-1}),right_axis=rotate(q,{1,0,0});
                const auto yaw=forward.x*forward.x+forward.z*forward.z>1e-6f?
                    std::atan2(-forward.x,-forward.z):std::atan2(-right_axis.z,right_axis.x);
                const Quaternion heading{0,std::sin(yaw/2),0,std::cos(yaw/2)};
                const auto hit=panel.intersect({{0,0,0},heading});
                require(hit.has_value(),"Menu placement inherited head pitch or roll");
                require(std::abs(hit->point.y)<.0001f && std::abs(hit->distance-2)<.0001f,
                    "Menu did not stay at horizontal eye height");
                require(std::abs(hit->u-.5f)<.0001f && std::abs(hit->v-.5f)<.0001f,
                    "Independent center ray did not reach the native menu center");
                const auto right=panel.intersect({rotate(heading,{.25f,0,0}),heading});
                require(right && right->u>hit->u,"Menu ray horizontal mapping is reversed");
                const auto upper=panel.intersect({{0,.2f,0},heading});
                require(upper && upper->v<hit->v,"Menu ray vertical mapping is reversed");
                require(!panel.intersect({{0,0,0},{0,1,0,0}}),"Backward ray clicked a menu");
                require(!panel.intersect({{100,0,0},{0,0,0,1}}),"Out-of-bounds ray clicked a menu");
                OutlastRecenter origin;
                origin.recenter(eyes);
                require(origin.initialized(),"Vertical head orientation prevented recentering");
            }
        NativeSkeleton skeleton{};
        std::ifstream input(argv[1],std::ios::binary);
        UINT64 magic=0;UINT count=0;std::array<INT,20> bones{};
        input.read(reinterpret_cast<char*>(&magic),8);input.read(reinterpret_cast<char*>(&count),4);
        require(magic==0x314b49524c4full && count==73,"Incorrect retained native hero fixture");
        input.read(reinterpret_cast<char*>(bones.data()),sizeof(bones));
        skeleton.parents.resize(count);skeleton.atoms.resize(count);
        input.read(reinterpret_cast<char*>(skeleton.parents.data()),count*sizeof(INT));
        input.read(reinterpret_cast<char*>(skeleton.world.data()),sizeof(skeleton.world));
        input.read(reinterpret_cast<char*>(skeleton.atoms.data()),count*sizeof(BoneAtom));
        require(bool(input),"Truncated native hero fixture");
        auto reference_path=std::filesystem::path(argv[1]);reference_path.replace_extension(".reference.bin");
        std::vector<unsigned char> reference(count*0x50),asset(0x100),component(0x800);
        std::ifstream reference_input(reference_path,std::ios::binary);
        reference_input.read(reinterpret_cast<char*>(reference.data()),std::streamsize(reference.size()));
        require(bool(reference_input),"Missing native reference joints");
        auto* data=reference.data();
        std::memcpy(asset.data()+0xc4,&data,sizeof(data));
        skeleton.asset=asset.data();skeleton.component=component.data();
        NativeControllerFingers fingers(skeleton,bones[9],bones[13]);
        const auto original=skeleton.atoms;
        auto opened=skeleton,closed=skeleton;
        for (unsigned side=0;side<2;++side) {
            fingers.apply(opened,original,side,0,0,false);
            fingers.apply(closed,original,side,1,1,false);
            const auto wrist=std::size_t(bones[side?13:9]);
            for (unsigned i=0;i<15;++i) {
                const auto index=wrist+i+1,parent=std::size_t(skeleton.parents[index]);
                const auto expected=skeleton_field<Vec3>(reference.data(),index*0x50+0x20);
                require(std::abs(distance(opened.atoms[index].position,opened.atoms[parent].position)-
                    std::sqrt(dot(expected,expected)))<.0001f,"Opening a hand stretched a finger");
                require(std::abs(distance(closed.atoms[index].position,closed.atoms[parent].position)-
                    std::sqrt(dot(expected,expected)))<.0001f,"Making a fist stretched a finger");
            }
            for (unsigned finger=1;finger<5;++finger) {
                const auto tip=wrist+3* finger+3;
                require(distance(closed.atoms[tip].position,closed.atoms[wrist].position)<
                    distance(opened.atoms[tip].position,opened.atoms[wrist].position)-.1f,
                    "Analog fist did not close toward the native palm");
            }
            auto trigger=skeleton;
            fingers.apply(trigger,original,side,1,0,false);
            for (unsigned i=6;i<15;++i)
                require(distance(trigger.atoms[wrist+i+1].position,opened.atoms[wrist+i+1].position)<.0001f,
                    "Index trigger moved the three squeeze-driven fingers");
            auto walking=skeleton;
            for (unsigned i=1;i<=15;++i)
                walking.atoms[wrist+i].rotation=bone_normalize({.1f,.2f,.3f,.9f});
            const auto native_walk=walking.atoms;
            fingers.apply(walking,native_walk,side,0,0,false);
            for (unsigned i=1;i<=15;++i) {
                const auto& a=walking.atoms[wrist+i];const auto& b=opened.atoms[wrist+i];
                require(distance(a.position,b.position)<.0001f &&
                    std::abs(std::abs(a.rotation.x*b.rotation.x+a.rotation.y*b.rotation.y+
                        a.rotation.z*b.rotation.z+a.rotation.w*b.rotation.w)-1)<.00001f,
                    "Native locomotion leaked into controller-driven fingers");
            }
            for (bool camera:{false,true}) {
                auto held=skeleton;
                fingers.apply(held,original,side,.4f,.7f,camera);
                require(std::memcmp(&held.atoms[wrist],&original[wrist],sizeof(BoneAtom))==0,
                    "Grabbing the camera moved the tracked wrist");
            }
        }
        std::cout<<"PASS: controller_interaction_checks="<<checks
            <<"; upright_pitch_roll_and_vertical_recenter; menu_ray_UV_and_misses"
            <<"; actual_native_73_bone_fingers; analog_fists; independent_index"
            <<"; gait_invariance; immutable_lengths; camera_does_not_move_wrist\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
