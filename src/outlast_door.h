#pragma once
#include "outlast_hand_pickup.h"
#include <optional>

namespace olvr {
using OutlastDoorFind=void*(WINAPI*)(void*);
using OutlastDoorAdmit=UINT64(WINAPI*)(void*);
using OutlastDoorOpen=void(WINAPI*)(void*,void*,float,float,INT);
using OutlastDoorContext=INT(WINAPI*)(void*);

inline std::optional<UINT64> outlast_open_door_without_player_animation(void* hero,
    OutlastDoorFind find,OutlastDoorOpen open,OutlastDoorContext context) {
    if (!hero || !find || !open || !context)
        throw std::invalid_argument("Incomplete native door opening request");
    if (pickup_field<BYTE>(hero,0x7bc)>1 || pickup_field<BYTE>(hero,0x7bd)!=0)
        return {};
    auto* door=find(hero);
    if (!door) return UINT64(0);
    if (pickup_field<BYTE>(door,0x308)!=0 || pickup_field<BYTE>(door,0x30c)==2 ||
        pickup_field<void*>(door,0x19a0) || (pickup_field<UINT>(door,0x2c4)&0x170002u) ||
        (pickup_field<BYTE>(door,0x194c)&2))
        return {};
    const float angle=pickup_virtual<float(WINAPI*)(void*)>(door,0x758)(door);
    const float limit=pickup_field<float>(hero,0x43c4);
    if (!std::isfinite(angle) || !std::isfinite(limit))
        throw std::runtime_error("Invalid native door admission angle");
    if (angle>limit || pickup_field<BYTE>(door,0x30b)!=0 ||
        ((pickup_field<UINT>(hero,0x288)&0x10) && !context(hero)) ||
        !pickup_virtual<INT(WINAPI*)(void*,UINT)>(hero,0xb00)(hero,0x1c))
        return UINT64(0);
    // REA D4BC40 admission guards, then D4A770: native actor tick, collision, sound and events;
    // deliberately never enter the hero special move or its repositioning/animation calls.
    open(door,hero,0.f,0.f,0);
    return UINT64(1);
}
}
