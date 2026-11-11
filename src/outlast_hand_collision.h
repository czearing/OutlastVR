#pragma once
#include "xr_view_constants.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace olvr {
struct NativeHandHit {
    std::uintptr_t next=0,actor=0;
    Vec3 location{},normal{};
    float time=1;
    INT item=-1;
    std::array<unsigned char,0x30> detail{};
    INT level=-1;
    UINT flags=0;
};
static_assert(sizeof(NativeHandHit)==0x68);
static_assert(offsetof(NativeHandHit,time)==0x28);
static_assert(offsetof(NativeHandHit,normal)==0x1c);
using NativeWorldTrace=bool(WINAPI*)(void*,NativeHandHit*,void*,const Vec3*,const Vec3*,UINT,const Vec3*,void*);

struct HandContact {
    Vec3 position{};
    float controller_distance=0;
    bool blocked=false;
};
enum class HandContactReset { None,Distant,Occluded };
struct HandContactDiagnostic {
    Vec3 previous_position{},previous_anchor{};
    bool valid=false,blocked=false,revalidated=false;
    HandContactReset reset=HandContactReset::None;
};

class OutlastHandCollision {
    std::array<Vec3,2> previous_{};
    std::array<Vec3,2> anchors_{};
    std::array<bool,2> blocked_{};
    std::array<bool,2> valid_{};
public:
    void reset(unsigned hand) {valid_.at(hand)=false;}
    void reset() {valid_={};}
    template<class Trace>
    HandContact update(unsigned hand,Vec3 anchor,Vec3 desired,float units,Trace&& trace,
                       HandContactDiagnostic* diagnostic=nullptr) {
        if (hand>=2 || !finite(anchor) || !finite(desired) || !std::isfinite(units) || units<=0)
            throw std::invalid_argument("Invalid tracked hand collision request");
        const float radius=.08f*units,skin=.001f*units;
        const Vec3 extent{radius,radius,radius};
        const auto controller=desired;
        if (diagnostic) *diagnostic={previous_[hand],anchors_[hand],valid_[hand],blocked_[hand],
            false,HandContactReset::None};
        if (valid_[hand] && blocked_[hand]) {
            const auto body_move=add(anchor,scale(anchors_[hand],-1));
            const auto tether=add(previous_[hand],scale(anchor,-1));
            const auto reach=add(desired,scale(anchor,-1));
            if (dot(tether,tether)>std::max(dot(reach,reach),units*units)) {
                valid_[hand]=false;
                if (diagnostic) diagnostic->reset=HandContactReset::Distant;
            } else if (dot(body_move,body_move)>skin*skin) {
                NativeHandHit hit{};
                if (diagnostic) diagnostic->revalidated=true;
                if (!trace(hit,previous_[hand],anchor,extent)) {
                    valid_[hand]=false;
                    if (diagnostic) diagnostic->reset=HandContactReset::Occluded;
                }
            }
        }
        Vec3 start=valid_[hand]?previous_[hand]:anchor;
        bool blocked=false;
        for (unsigned iteration=0;iteration<3;++iteration) {
            const auto delta=add(desired,scale(start,-1));
            const float length=std::sqrt(dot(delta,delta));
            if (length<skin) break;
            NativeHandHit hit{};
            const bool clear=trace(hit,desired,start,extent);
            if (clear) {start=desired;break;}
            if (!std::isfinite(hit.time) || hit.time<0 || hit.time>1 || !finite(hit.normal) ||
                std::abs(dot(hit.normal,hit.normal)-1)>.01f)
                throw std::runtime_error("Native hand sweep returned an invalid contact");
            blocked=true;
            start=add(start,scale(delta,std::max(0.f,hit.time-skin/length)));
            const auto remaining=add(desired,scale(start,-1));
            const float inward=std::min(0.f,dot(remaining,hit.normal));
            desired=add(desired,scale(hit.normal,-inward));
        }
        previous_[hand]=start;anchors_[hand]=anchor;blocked_[hand]=blocked;valid_[hand]=true;
        const auto offset=add(start,scale(controller,-1));
        return {start,std::sqrt(dot(offset,offset)),blocked};
    }
};
}
