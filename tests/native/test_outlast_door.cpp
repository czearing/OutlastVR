#include "outlast_door.h"
#include <array>
#include <iostream>
using namespace olvr;
namespace {
std::array<BYTE,0x4500> hero{};
std::array<BYTE,0x1a10> door{};
std::array<void*,0xb08/8> hero_table{};
std::array<void*,0x760/8> door_table{};
unsigned opened=0;
bool selected=true,allowed=true,context_allowed=true;
void require(bool pass,const char* message) {if (!pass) throw std::runtime_error(message);}
void* WINAPI find(void* owner) {
    require(owner==hero.data(),"Selector lost the native hero");
    return selected?door.data():nullptr;
}
INT WINAPI context(void*) {return context_allowed;}
INT WINAPI gate(void*,UINT action) {require(action==0x1c,"Changed native admission action");return allowed;}
float WINAPI angle(void* actor) {return pickup_field<float>(actor,0x1908)*90.f;}
void WINAPI open(void* actor,void* owner,float speed,float degrees,INT silent) {
    require(actor==door.data() && owner==hero.data() && speed==0 && degrees==0 && silent==0,
        "Changed native door open arguments or silenced its events");
    ++opened;pickup_write(actor,0x190c,1.f);
}
}
int main() {
    try {
        hero_table[0xb00/8]=reinterpret_cast<void*>(&gate);
        door_table[0x758/8]=reinterpret_cast<void*>(&angle);
        pickup_write(hero.data(),0,hero_table.data());
        pickup_write(door.data(),0,door_table.data());
        pickup_write(hero.data(),0x43c4,30.f);
        const auto request=[] {return outlast_open_door_without_player_animation(hero.data(),&find,&open,&context);};
        const auto original_hero=hero;
        require(request()==1 && opened==1 && hero==original_hero &&
            pickup_field<float>(door.data(),0x1908)==0 && pickup_field<float>(door.data(),0x190c)==1,
            "Ordinary door did not use native actor movement without touching the player");
        for (const UINT flags:{2u,0x100000u,0x20000u}) {
            pickup_write(door.data(),0x2c4,flags);
            require(!request(),"Locked/scripted door bypassed its native admission");
        }
        pickup_write(door.data(),0x2c4,UINT(0));
        pickup_write(door.data(),0x19a0,hero.data());
        require(!request(),"Occupied door was stolen from its owning interaction");
        pickup_write(door.data(),0x19a0,static_cast<void*>(nullptr));
        pickup_write(door.data(),0x308,BYTE(1));
        require(!request(),"Unsupported door type entered ordinary hinge handling");
        pickup_write(door.data(),0x308,BYTE(0));
        allowed=false;require(request()==0,"Movement admission was bypassed");allowed=true;
        pickup_write(hero.data(),0x288,UINT(0x10));
        context_allowed=false;require(request()==0,"Native context admission was bypassed");
        context_allowed=true;pickup_write(hero.data(),0x288,UINT(0));
        pickup_write(door.data(),0x1908,.5f);
        require(request()==0,"Door beyond the original admission angle was opened");
        pickup_write(door.data(),0x1908,0.f);
        selected=false;require(request()==0,"Missing native selection opened a door");
        require(opened==1 && hero==original_hero,"Rejected door changed the hero or opened an actor");
        std::cout<<"PASS: native_door_actor_open; no_player_animation_or_reposition; authored_speed_and_events; "
            "native_selection; lock_script_owner_type_and_movement_guards; no_door_teleport\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
