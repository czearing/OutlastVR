#include "outlast_native_use.h"
#include "outlast_motion_controls.h"
#include <iostream>
#include <limits>
#include <type_traits>

using namespace olvr;
namespace {
unsigned checks=0;
unsigned completions=0;
void WINAPI complete_pickup(void* hero) {
    ++completions;
    auto* bytes=static_cast<unsigned char*>(hero);
    bytes[0x7bc]=0;bytes[0x7bd]=0;
    const void* selected=nullptr;
    std::memcpy(bytes+0x3648,&selected,sizeof(selected));
    bytes[0x101]=0x73;
}
void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void close(Vec3 actual,Vec3 expected) {
    require(std::abs(actual.x-expected.x)<.0001f && std::abs(actual.y-expected.y)<.0001f &&
        std::abs(actual.z-expected.z)<.0001f,"Controller use ray differs from the independent world-space oracle");
}
template<class F> void rejects(F&& function) {
    bool rejected=false;
    try {function();} catch (const std::invalid_argument&) {rejected=true;}
    require(rejected,"Malformed native use pose was accepted");
}
}

int main() {
    try {
        static_assert(!std::is_copy_constructible_v<OutlastNativeUsePose>);
        static_assert(!std::is_copy_assignable_v<OutlastNativeUsePose>);
        {
            std::array<unsigned char,0x4500> hero{};
            void* selected=reinterpret_cast<void*>(0x1234);
            for (unsigned basic=0;basic<4;++basic)
                for (unsigned special=0;special<64;++special) {
                    hero.fill(0);hero[0x7bc]=basic;hero[0x7bd]=special;
                    std::memcpy(hero.data()+0x3648,&selected,sizeof(selected));
                    const auto before=hero;
                    const auto calls=completions;
                    const bool eligible=basic==2 && special==0x31;
                    require(outlast_complete_controller_pickup(hero.data(),1,1,true,&complete_pickup)==eligible,
                        "Instant pickup completed an unrelated native interaction");
                    require(completions==calls+unsigned(eligible),
                        "Native pickup completion did not run exactly once");
                    require(eligible?(hero[0x7bd]==0 && hero[0x101]==0x73):hero==before,
                        "Native completion effects were overwritten or unrelated state was changed");
                }
            hero.fill(0);hero[0x7bc]=2;hero[0x7bd]=0x31;
            std::memcpy(hero.data()+0x3648,&selected,sizeof(selected));
            const auto before=hero;
            for (const auto gate:{0,1,2}) {
                const auto calls=completions;
                require(!outlast_complete_controller_pickup(hero.data(),gate==0?0:1,gate==1?0:1,
                    gate!=2,&complete_pickup) && hero==before && completions==calls,
                    "Query-only, refused or non-controller pickup ran inventory completion");
            }
            rejects([&] {outlast_complete_controller_pickup(nullptr,1,1,true,&complete_pickup);});
            rejects([&] {outlast_complete_controller_pickup(hero.data(),1,1,true,nullptr);});
            selected=nullptr;std::memcpy(hero.data()+0x3648,&selected,sizeof(selected));
            bool missing=false;
            try {outlast_complete_controller_pickup(hero.data(),1,1,true,&complete_pickup);}
            catch (const std::runtime_error&) {missing=true;}
            require(missing,"Admitted pickup without a selected item silently succeeded");
        }
        for (unsigned basic=0;basic<16;++basic)
            for (unsigned special=0;special<64;++special) {
                const bool expected=basic==2 && (special==0x31 || special==0x32);
                require(outlast_native_use_action_ready(false,{1000,basic,special},1001)==expected,
                    "Native action continuation enabled an unrelated gameplay state");
            }
        for (const unsigned special:{0x31u,0x32u}) {
            require(outlast_native_use_action_ready(false,{1000,2,special},251000),
                "Current native pickup/use animation lost its held action");
            require(!outlast_native_use_action_ready(false,{1000,2,special},251001) &&
                !outlast_native_use_action_ready(false,{1000,2,special},999),
                "Expired or regressed native interaction context retained controller actions");
        }
        require(outlast_native_use_action_ready(true,{},1000000),
            "Fresh body targeting incorrectly depended on an animation context");
        {
            OutlastMotionControls controls;
            MotionFrame frame{};frame.focused=true;frame.hands[1].aim_tracked=true;
            XINPUT_STATE output{},physical{};
            auto poll=[&](unsigned basic,unsigned special,bool body_ready,UINT64 clock) {
                controls.publish(frame,clock);
                controls.state(0,&output,ERROR_DEVICE_NOT_CONNECTED,physical,clock+1,false,
                    outlast_native_use_action_ready(body_ready,{clock,basic,special},clock+1));
                return (output.Gamepad.wButtons&XINPUT_GAMEPAD_X)!=0;
            };
            require(!poll(0,0,true,1000),"Released controller started a pickup");
            frame.hands[1].squeeze=1;
            require(poll(0,0,true,2000),"Fresh controller did not start a native pickup");
            require(poll(2,49,false,3000),
                "Observed retail basic 2 / special 49 interrupted the held pickup action");
            frame.hands[1].squeeze=0;
            require(!poll(2,49,false,4000),"Native pickup lost its controller release");
            frame.hands[1].squeeze=1;
            require(poll(2,49,false,5000),"Native pickup did not accept a fresh continuation press");
            require(poll(2,50,false,6000),"Native use-volume animation interrupted its action");
            require(!poll(2,31,false,7000),"Decimal 31 was mistaken for native special 0x31");
            require(!poll(2,49,false,8000),"Held input rearmed after an unrelated native state");
            frame.hands[1].squeeze=0;
            require(!poll(0,0,true,9000),"Native interaction return generated a phantom press");
            frame.hands[1].squeeze=1;
            require(poll(0,0,true,10000),"Controller did not recover after native interaction completion");
        }
        TrackingFrame tracking{{100,200,300},{0,1,0},{0,0,1},{-1,0,0},100,{9000,-7000,4000}};
        MotionFrame motion{};
        motion.focused=true;motion.hands[1].aim_tracked=true;
        motion.hands[1].aim.orientation.w=1;
        motion.hands[1].aim.position={.2f,-.1f,-.5f};
        for (const float yaw:{0.f,45.f,90.f,180.f,270.f,-30.f}) {
            const auto angle=yaw*3.14159265358979323846f/180;
            motion.hands[1].aim.orientation={0,std::sin(angle*.5f),0,std::cos(angle*.5f)};
            const auto ray=outlast_controller_use_ray(tracking,motion,1000,1001,true);
            require(ray.has_value(),"Fresh focused right controller lost native interaction");
            close(ray->origin,{150,220,290});
            close(ray->direction,{std::cos(angle),-std::sin(angle),0});
        }
        motion.hands[1].aim.orientation={0,0,0,1};
        require(outlast_controller_use_ray(tracking,motion,1000,251000,true).has_value(),
            "Inclusive native use freshness boundary changed");
        require(!outlast_controller_use_ray(tracking,motion,1000,251001,true) &&
            !outlast_controller_use_ray(tracking,motion,1000,999,true) &&
            !outlast_controller_use_ray(tracking,motion,1000,1001,false),
            "Expired, regressed or ineligible use acquired a controller ray");
        motion.focused=false;
        require(!outlast_controller_use_ray(tracking,motion,1000,1001,true),
            "Unfocused controller retained native use");
        motion.focused=true;motion.hands[1].aim_tracked=false;motion.hands[0].aim_tracked=true;
        require(!outlast_controller_use_ray(tracking,motion,1000,1001,true),
            "Missing right-controller aim fell back to the left hand or stale input");
        motion.hands[1].aim_tracked=true;
        auto invalid=motion;
        invalid.hands[1].aim.orientation={0,0,0,0};
        rejects([&] {outlast_controller_use_ray(tracking,invalid,1000,1001,true);});
        invalid=motion;invalid.hands[1].aim.position.x=std::numeric_limits<float>::infinity();
        rejects([&] {outlast_controller_use_ray(tracking,invalid,1000,1001,true);});
        invalid=motion;invalid.hands[1].aim.orientation.w=std::numeric_limits<float>::quiet_NaN();
        rejects([&] {outlast_controller_use_ray(tracking,invalid,1000,1001,true);});
        for (const float scale_value:{0.f,-1.f,std::numeric_limits<float>::infinity()}) {
            auto bad=tracking;bad.units_per_meter=scale_value;
            rejects([&] {outlast_controller_use_ray(bad,motion,1000,1001,true);});
        }
        auto bad=tracking;bad.up=bad.right;
        rejects([&] {outlast_controller_use_ray(bad,motion,1000,1001,true);});
        bad=tracking;bad.origin.z=std::numeric_limits<float>::quiet_NaN();
        rejects([&] {outlast_controller_use_ray(bad,motion,1000,1001,true);});
        std::array<unsigned char,0x4480> hero{};
        for (std::size_t index=0;index<hero.size();++index) hero[index]=static_cast<unsigned char>(index*37+19);
        const auto original=hero;
        const OutlastUseRay first{{1,2,3},{1,0,0}},second{{7,8,9},{0,0,-1}};
        {
            OutlastNativeUsePose outer(hero.data(),first);
            auto expected=original;
            std::memcpy(expected.data()+0x30e4,&first.origin,sizeof(Vec3));
            std::memcpy(expected.data()+0x2bd8,&first.direction,sizeof(Vec3));
            require(hero==expected,"Use scope changed root, camera rotation, inventory or another native field");
            try {
                OutlastNativeUsePose inner(hero.data(),second);
                throw std::runtime_error("Injected native callback failure");
            } catch (const std::runtime_error&) {}
            require(hero==expected,"Exception/nested use did not restore the outer controller scope");
        }
        require(hero==original,"Native use left temporary camera fields changed");
        {
            OutlastNativeUsePose scope(hero.data(),first);
            hero[0x7bd]=0x31;
        }
        auto committed=original;committed[0x7bd]=0x31;
        require(hero==committed,"Use-pose restoration reverted the native pickup animation/state transition");
        rejects([&] {OutlastNativeUsePose scope(nullptr,first);});
        rejects([&] {OutlastNativeUsePose scope(hero.data(),{{1,2,3},{0,0,0}});});
        require(hero==committed,"Rejected scope mutated native state");
        std::cout<<"PASS: native_use_checks="<<checks
            <<"; right_aim_world_space; no_pretranslation; freshness_focus_tracking_and_mode_gates"
            <<"; byte_exact_scope_restoration; nested_failure_cleanup; native_state_changes_preserved"
            <<"; no_native_pickup_execution_claim\n";
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
