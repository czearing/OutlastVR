#include "outlast_hand_collision.h"
#include <iostream>

using namespace olvr;
int main() {
    try {
        unsigned checks=0,traces=0;
        auto require=[&](bool value,const char* message) {
            ++checks;
            if (!value) throw std::runtime_error(message);
        };
        OutlastHandCollision collision;
        auto wall=[&](NativeHandHit& hit,Vec3 end,Vec3 start,Vec3 extent) {
            ++traces;
            require(extent.x==8 && extent.y==8 && extent.z==8,"Hand volume changed native units");
            const float plane=100-extent.x;
            if (end.x<=plane) return true;
            hit.actor=1;hit.normal={-1,0,0};
            hit.time=start.x>=plane?0:(plane-start.x)/(end.x-start.x);
            return false;
        };
        const Vec3 anchor{0,0,120};
        auto result=collision.update(0,anchor,{30,0,120},100,wall);
        require(!result.blocked && std::abs(result.position.x-30)<.001f,"Unobstructed hand did not track controller");
        result=collision.update(0,anchor,{150,40,120},100,wall);
        require(result.blocked && result.position.x<=92 && result.position.x>91.8f,
            "Tracked hand tunneled through the native wall");
        require(std::abs(result.position.y-40)<.001f,"Native wall contact did not slide tangentially");
        for (unsigned i=0;i<120;++i) {
            result=collision.update(0,anchor,{150,float(i),120},100,wall);
            require(result.position.x<=92 && std::abs(result.position.y-float(i))<.001f,
                "Sustained contact penetrated or stuck on the wall");
        }
        result=collision.update(0,anchor,{30,120,120},100,wall);
        require(!result.blocked && std::abs(result.position.x-30)<.001f,
            "Contact failed to release when the tracked hand moved away");
        collision.reset(0);
        result=collision.update(0,anchor,{150,0,120},100,wall);
        require(result.blocked && result.position.x<=92,"Tracking reacquisition bypassed the body-to-hand sweep");
        result=collision.update(1,anchor,{-30,0,120},100,wall);
        require(!result.blocked && result.position.x==-30,"One hand's contact corrupted the other hand");
        bool refused=false;
        try {
            collision.update(0,anchor,{150,0,120},100,
                [](NativeHandHit& hit,Vec3,Vec3,Vec3) {hit.time=.5f;return false;});
        } catch (const std::runtime_error&) {refused=true;}
        require(refused,"Invalid native collision normal was silently accepted");
        collision.reset();
        result=collision.update(0,{0,0,120},{150,0,120},100,wall);
        require(result.blocked,"Moving-body regression did not begin in real wall contact");
        bool tether_checked=false;
        HandContactDiagnostic diagnostic{};
        result=collision.update(0,{70,50,120},{100,50,120},100,
            [&](NativeHandHit& hit,Vec3 end,Vec3 start,Vec3 extent) {
                require(extent.x==8,"Moving-body tether changed hand sweep extent");
                if (end.y<25) {
                    tether_checked=true;hit.time=0;hit.normal={0,1,0};return false;
                }
                require(start.y==50,"Hand remained tethered to an old room after body locomotion");
                return true;
            },&diagnostic);
        require(tether_checked && !result.blocked && std::abs(result.position.y-50)<.001f &&
            std::abs(result.position.x-100)<.001f,
            "Moving around a corner left the constrained hand behind the body");
        require(diagnostic.valid && diagnostic.blocked && diagnostic.revalidated &&
            diagnostic.reset==HandContactReset::Occluded && diagnostic.previous_anchor.x==0,
            "Corner recovery diagnostic does not describe the actual old-contact reset");
        collision.reset();
        collision.update(0,anchor,{150,0,120},100,wall);
        result=collision.update(0,{500,150,120},{530,150,120},100,
            [](NativeHandHit&,Vec3,Vec3,Vec3) {return true;},&diagnostic);
        require(!result.blocked && result.position.x==530,"Travel retained an obsolete distant contact");
        require(diagnostic.valid && diagnostic.blocked && !diagnostic.revalidated &&
            diagnostic.reset==HandContactReset::Distant,"Distant contact reset diagnostic is incorrect");
        std::cout<<"PASS: native_hand_collision_checks="<<checks<<"; native_swept_volume; no_tunneling; "
            <<"tangential_slide; release; independent_hands; reacquisition_sweep; invalid_contact_rejected; "
            <<"moving_body_contact_revalidation; obsolete_room_tether_released; queries="<<traces<<'\n';
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
