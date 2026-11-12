#include "outlast_hand_collision.h"
#include <iostream>

using namespace olvr;
namespace {
bool WINAPI native_filter_fixture(void*,NativeHandHit* hit,void*,const Vec3* end,const Vec3* start,
                                 UINT flags,const Vec3* extent,void*) {
    // Replay the retail blocking-query predicate, not native engine execution.
    if ((flags&0x20) || !(flags&0x40)) {
        hit->actor=1;hit->time=0;hit->normal={};
        return false;
    }
    const float plane=100-extent->x;
    if (end->x<=plane) return true;
    hit->actor=2;hit->normal={-1,0,0};
    hit->time=start->x>=plane?0:(plane-start->x)/(end->x-start->x);
    return false;
}
}
int main(int argc,char** argv) {
    try {
        const bool fixed_root_fault=argc==2 && std::string(argv[1])=="--fault-root-anchor";
        const bool overlap_filter_fault=argc==2 && std::string(argv[1])=="--fault-nonblocking-volume";
        if (argc!=1 && !fixed_root_fault && !overlap_filter_fault)
            throw std::invalid_argument("Unknown hand collision fixture mode");
        unsigned checks=0,traces=0;
        auto require=[&](bool value,const char* message) {
            ++checks;
            if (!value) throw std::runtime_error(message);
        };
        OutlastHandCollision collision;
        const auto native_trace=[&](NativeHandHit& hit,Vec3 end,Vec3 start,Vec3 extent) {
            if (overlap_filter_fault)
                return native_filter_fixture(nullptr,&hit,nullptr,&end,&start,0x20bf,&extent,nullptr);
            return outlast_trace_hand_volume(&native_filter_fixture,nullptr,hit,nullptr,end,start,extent);
        };
        for (unsigned hand=0;hand<2;++hand) {
            auto contact=collision.update(hand,{0,0,120},{30,0,120},100,native_trace);
            require(!contact.blocked && contact.controller_distance<.001f,
                "Non-blocking room volume stopped controller hand tracking");
            contact=collision.update(hand,{0,0,120},{150,40,120},100,native_trace);
            require(contact.blocked && contact.position.x<=92 && contact.position.x>91.8f &&
                std::abs(contact.position.y-40)<.001f,
                "Ignoring a room trigger also bypassed the solid wall behind it");
            contact=collision.update(hand,{0,0,120},{30,40,120},100,native_trace);
            require(!contact.blocked && contact.controller_distance<.001f,
                "Tracked hand did not release a solid contact inside a non-blocking room volume");
        }
        collision.reset();
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
        unsigned overlap_cases=0;
        for (const float units:{50.f,100.f,200.f})
            for (unsigned hand=0;hand<2;++hand)
                for (unsigned direction=0;direction<6;++direction)
                    for (const bool overlap_normal:{false,true}) {
                    collision.reset();
                    const float factor=units/100;
                    const Vec3 origin{10000,-20000,30000};
                    const auto point=[&](Vec3 v) {return add(origin,scale(v,factor));};
                    const Vec3 center=point({30,0,120}),body=point({0,0,120});
                    bool obstacle=false;
                    auto box=[&](NativeHandHit& hit,Vec3 end,Vec3 start,Vec3 extent) {
                        const auto motion=add(end,scale(start,-1));
                        require(dot(motion,motion)>0,"Hand recovery relied on an unsupported stationary native sweep");
                        if (!obstacle) return true;
                        const float low[]{center.x-10*factor-extent.x,
                            center.y-10*factor-extent.y,center.z-10*factor-extent.z};
                        const float high[]{center.x+10*factor+extent.x,
                            center.y+10*factor+extent.y,center.z+10*factor+extent.z};
                        const float a[]{start.x,start.y,start.z},b[]{end.x,end.y,end.z};
                        bool inside=true;
                        float enter=0,leave=1;
                        Vec3 normal{-1,0,0};
                        for (unsigned axis=0;axis<3;++axis) {
                            inside=inside && a[axis]>=low[axis] && a[axis]<=high[axis];
                            const float delta=b[axis]-a[axis];
                            if (std::abs(delta)<1e-6f) {
                                if (a[axis]<low[axis] || a[axis]>high[axis]) return true;
                                continue;
                            }
                            const float first=(low[axis]-a[axis])/delta;
                            const float last=(high[axis]-a[axis])/delta;
                            const float near_time=std::min(first,last),far_time=std::max(first,last);
                            if (near_time>enter) {
                                enter=near_time;normal={};
                                if (axis==0) normal.x=delta>0?-1.f:1.f;
                                if (axis==1) normal.y=delta>0?-1.f:1.f;
                                if (axis==2) normal.z=delta>0?-1.f:1.f;
                            }
                            leave=std::min(leave,far_time);
                            if (enter>leave) return true;
                        }
                        if (!inside && (leave<0 || enter>1)) return true;
                        hit.actor=1;hit.time=inside?0:enter;hit.normal=inside && !overlap_normal?Vec3{}:normal;
                        return false;
                    };
                    collision.update(hand,body,center,units,box);
                    obstacle=true;
                    Vec3 escape{};
                    const float distance=direction%2?-50.f:50.f;
                    if (direction/2==0) escape.x=distance;
                    if (direction/2==1) escape.y=distance;
                    if (direction/2==2) escape.z=distance;
                    const auto desired=add(center,scale(escape,factor));
                    const auto recovered=collision.update(hand,body,desired,units,box);
                    const auto error=add(recovered.position,scale(desired,-1));
                    require(!recovered.blocked && dot(error,error)<.001f,
                        "A moving obstacle permanently trapped a hand after the controller left the solid");
                    ++overlap_cases;
                    collision.reset(hand);
                    const auto embedded=collision.update(hand,center,center,units,box);
                    require(embedded.position.x==center.x,"Stationary embedded hand moved without tracking input");
                    const auto still_inside=collision.update(hand,center,
                        add(center,{factor,0,0}),units,box);
                    require(still_inside.blocked && still_inside.position.x==center.x,
                        "Overlap recovery ignored a controller that was still inside the solid");
                    const auto free=collision.update(hand,center,desired,units,box);
                    require(!free.blocked && dot(add(free.position,scale(desired,-1)),
                        add(free.position,scale(desired,-1)))<.001f,
                        "An overlapping body anchor trapped a physically free tracked hand");
                }
        unsigned room_scale_cases=0;
        for (const float units:{50.f,100.f,200.f})
            for (unsigned yaw=0;yaw<4;++yaw) {
            collision.reset();
            TrackingFrame tracking{};
            const float c=float(std::cos(yaw*1.5707963267948966));
            const float s=float(std::sin(yaw*1.5707963267948966));
            tracking.right={c,s,0};tracking.up={0,0,1};tracking.backward={s,-c,0};
            tracking.units_per_meter=units;
            const Vec3 root{10000,-20000,30000};
            auto upper_body=outlast_hand_collision_anchor(root,1.6f*units,
                tracking,{2,0,0},.1f);
            require(std::abs(upper_body.z-root.z-1.3f*units)<.01f,
                "Collision anchor diverged from standing and tracked body-height compensation");
            const auto target=add(upper_body,scale(tracking.right,-.4f*units));
            if (fixed_root_fault) {upper_body.x=root.x;upper_body.y=root.y;}
            const auto contact=collision.update(0,upper_body,target,units,
                [&](NativeHandHit& hit,Vec3 end,Vec3 start,Vec3 extent) {
                    const float low=units-extent.x,high=1.1f*units+extent.x;
                    const float from=dot(add(start,scale(root,-1)),tracking.right);
                    const float to=dot(add(end,scale(root,-1)),tracking.right);
                    if (from>high && to>high) return true;
                    hit.time=(low-from)/(to-from);hit.normal=scale(tracking.right,-1);return false;
                });
            require(!contact.blocked && contact.controller_distance<.01f,
                "Room-scale head/body movement left the tracked hand anchored across a wall");
            ++room_scale_cases;
        }
        std::cout<<"PASS: native_hand_collision_checks="<<checks<<"; native_swept_volume; no_tunneling; "
            <<"tangential_slide; release; independent_hands; reacquisition_sweep; invalid_contact_rejected; "
            <<"moving_body_contact_revalidation; obsolete_room_tether_released; overlap_escape_cases="
            <<overlap_cases<<"; room_scale_cases="<<room_scale_cases<<"; queries="<<traces<<'\n';
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
