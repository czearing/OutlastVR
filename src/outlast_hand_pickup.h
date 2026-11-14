#pragma once
#include "outlast_hand_collision.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace olvr {
template<class T> T pickup_field(const void* object,std::size_t offset) {
    T result;
    std::memcpy(&result,static_cast<const BYTE*>(object)+offset,sizeof(T));
    return result;
}
template<class T> void pickup_write(void* object,std::size_t offset,const T& value) {
    std::memcpy(static_cast<BYTE*>(object)+offset,&value,sizeof(T));
}
template<class T> T pickup_virtual(void* object,std::size_t offset) {
    return reinterpret_cast<T>(pickup_field<void**>(object,0)[offset/sizeof(void*)]);
}

// Retail admission: REA ev_7d8da84b1272027c9081f41732a19e0fcf34f57ccf48a20b9615bef17cc8cba6.
// Only selection changes: nearest hand-reachable eligible actor, never the headset's cone.
inline UINT64 outlast_hand_pickup(void* hero,Vec3 hand,Vec3 eye,float radius,
    NativeWorldTrace trace,void* world) {
    if (!hero || !world || !trace || !finite(hand) || !finite(eye) ||
        !std::isfinite(radius) || radius<=0)
        throw std::invalid_argument("Invalid native hand pickup request");
    const auto count=pickup_field<INT>(hero,0x2c2c);
    auto* const* actors=pickup_field<void* const*>(hero,0x2c24);
    if (count<0 || count>4096 || (count && !actors))
        throw std::runtime_error("Invalid native hand pickup candidate array");
    using HeroReady=INT(WINAPI*)(void*,INT);
    using ItemReady=INT(WINAPI*)(void*);
    using ItemUse=INT(WINAPI*)(void*,void*);
    using ItemRefused=void(WINAPI*)(void*);
    using Start=INT(WINAPI*)(void*,INT,void*);
    using Orient=void(WINAPI*)(void*,INT,const Vec3*,const Vec3*,bool);
    if (!pickup_virtual<HeroReady>(hero,0xb00)(hero,0x31)) return 0;
    const auto root=pickup_field<Vec3>(hero,0x80);
    const float minimum=pickup_field<float>(hero,0x4390),maximum=pickup_field<float>(hero,0x4394);
    const float high_min=pickup_field<float>(hero,0x4398),high_max=pickup_field<float>(hero,0x439c);
    const float low_min=pickup_field<float>(hero,0x43a0),low_max=pickup_field<float>(hero,0x43a4);
    if (!finite(root) || !std::isfinite(minimum+maximum+high_min+high_max+low_min+low_max) ||
        minimum<0 || maximum<minimum || high_max<high_min || low_max<low_min)
        throw std::runtime_error("Invalid native hand pickup admission ranges");
    struct Candidate {void* actor;Vec3 position;float distance;bool low;};
    std::vector<Candidate> candidates;
    for (INT i=0;i<count;++i) {
        auto* actor=actors[i];
        if (!actor) continue;
        const auto position=pickup_field<Vec3>(actor,0x80);
        if (!finite(position)) throw std::runtime_error("Nonfinite native pickup actor position");
        const auto reach=add(position,scale(hand,-1)),delta=add(position,scale(root,-1));
        const float distance=dot(reach,reach),horizontal=delta.x*delta.x+delta.y*delta.y;
        const bool low=delta.z>=low_min && delta.z<=low_max;
        if (distance<=radius*radius && horizontal>=minimum*minimum && horizontal<=maximum*maximum &&
            (low || (delta.z>=high_min && delta.z<=high_max)))
            candidates.push_back({actor,position,distance,low});
    }
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {
        return a.distance<b.distance;
    });
    const Vec3 extent{};
    const auto eye_hand=add(hand,scale(eye,-1));
    if (!candidates.empty() && dot(eye_hand,eye_hand)>1e-6f) {
        NativeHandHit hit{};
        if (!trace(world,&hit,hero,&hand,&eye,139999,&extent,nullptr)) return 0;
    }
    for (const auto& candidate:candidates) {
        if (!pickup_virtual<ItemReady>(candidate.actor,0x730)(candidate.actor)) continue;
        NativeHandHit hit{};
        const auto target=add(candidate.position,{0,0,5});
        if (!trace(world,&hit,hero,&target,&hand,139999,&extent,nullptr)) continue;
        if (!pickup_virtual<ItemUse>(candidate.actor,0x738)(candidate.actor,hero)) {
            pickup_virtual<ItemRefused>(candidate.actor,0x740)(candidate.actor);
            continue;
        }
        if (!pickup_virtual<Start>(hero,0xb08)(hero,0x31,candidate.actor)) return 0;
        pickup_write(hero,0x3648,candidate.actor);
        const auto pawn_flags=pickup_field<UINT>(hero,0x288);
        pickup_write(hero,0x2f88,(pickup_field<UINT>(hero,0x2f88)&~0x10u)|(pawn_flags&0x10u));
        pickup_write(hero,0x2f8c,(pickup_field<UINT>(hero,0x2f8c)&~0x80u)|(candidate.low?0x80u:0));
        auto direction=add(candidate.position,scale(root,-1));
        direction.z=0;
        const float length=std::sqrt(dot(direction,direction));
        if (length>1e-4f) direction=scale(direction,1/length);
        else direction={};
        pickup_virtual<Orient>(hero,0xa28)(hero,0x31,&root,&direction,false);
        return 1;
    }
    return 0;
}
}
