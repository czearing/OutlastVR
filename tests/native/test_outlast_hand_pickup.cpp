#include "outlast_hand_pickup.h"
#include <iostream>
using namespace olvr;
namespace {
std::array<BYTE,0x4500> hero{};
std::array<std::array<BYTE,0x100>,2> items{};
std::array<void*,0xb10/8> hero_table{};
std::array<void*,0x748/8> item_table{};
bool ready=true,begin_ok=true,hand_blocked=false,near_blocked=false;
std::array<bool,2> eligible{true,true},inventory{true,true};
unsigned starts=0,orientations=0,refusals=0,traces=0;
void* selected=nullptr;
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
unsigned item_index(void* actor) {
    require(actor==items[0].data() || actor==items[1].data(),"Unexpected native actor dispatch");
    return actor==items[1].data();
}
INT WINAPI hero_ready(void* actor,INT move) {
    require(actor==hero.data() && move==0x31,"Wrong native pickup eligibility call");return ready;
}
INT WINAPI item_ready(void* actor) {return eligible[item_index(actor)];}
INT WINAPI item_use(void* actor,void* pawn) {
    require(pawn==hero.data(),"Native inventory eligibility lost its owning hero");
    return inventory[item_index(actor)];
}
void WINAPI item_refused(void*) {++refusals;}
INT WINAPI begin(void* pawn,INT move,void* actor) {
    require(pawn==hero.data() && move==0x31,"Wrong native special-move dispatch");
    ++starts;selected=actor;return begin_ok;
}
void WINAPI orient(void* pawn,INT move,const Vec3* root,const Vec3* direction,bool teleport) {
    require(pawn==hero.data() && move==0x31 && root->x==0 && root->y==0 && root->z==0 &&
        std::abs(direction->x-1)<.00001f && direction->y==0 && direction->z==0 && !teleport,
        "Hand pickup moved the native root or changed native orientation semantics");
    ++orientations;
}
bool WINAPI trace(void* world,NativeHandHit*,void* pawn,const Vec3* end,const Vec3* start,
    UINT flags,const Vec3* extent,void*) {
    require(world==hero.data() && pawn==hero.data() && flags==139999 &&
        extent->x==0 && extent->y==0 && extent->z==0,"Native pickup line-of-sight flags changed");
    ++traces;
    if (start->z==170) return !hand_blocked;
    return !(end->x==95 && near_blocked);
}
}
int main() {
    try {
        hero_table[0xb00/8]=reinterpret_cast<void*>(&hero_ready);
        hero_table[0xb08/8]=reinterpret_cast<void*>(&begin);
        hero_table[0xa28/8]=reinterpret_cast<void*>(&orient);
        item_table[0x730/8]=reinterpret_cast<void*>(&item_ready);
        item_table[0x738/8]=reinterpret_cast<void*>(&item_use);
        item_table[0x740/8]=reinterpret_cast<void*>(&item_refused);
        pickup_write(hero.data(),0,hero_table.data());
        for (auto& item:items) pickup_write(item.data(),0,item_table.data());
        pickup_write(items[0].data(),0x80,Vec3{110,0,50});
        pickup_write(items[1].data(),0x80,Vec3{95,0,50});
        void* actors[]{items[0].data(),items[1].data()};
        pickup_write(hero.data(),0x2c24,actors+0);pickup_write(hero.data(),0x2c2c,INT(2));
        pickup_write(hero.data(),0x4394,200.f);
        pickup_write(hero.data(),0x4398,-200.f);pickup_write(hero.data(),0x439c,200.f);
        pickup_write(hero.data(),0x43a0,-100.f);pickup_write(hero.data(),0x43a4,-1.f);
        pickup_write(hero.data(),0x30e4,Vec3{0,0,170});
        pickup_write(hero.data(),0x2bd8,Vec3{-1,0,0});
        auto attempt=[&](Vec3 hand=Vec3{90,0,50},float radius=30.f) {
            return outlast_hand_pickup(hero.data(),hand,{0,0,170},radius,&trace,hero.data());
        };
        require(attempt()==1 && selected==items[1].data() && starts==1 && orientations==1,
            "Looking away prevented nearest-hand pickup or native list order overrode hand distance");
        require(pickup_field<Vec3>(hero.data(),0x2bd8).x==-1 &&
            pickup_field<void*>(hero.data(),0x2c24)==actors && actors[0]==items[0].data() &&
            actors[1]==items[1].data(),"Hand selection modified native gaze or the native cached array");
        near_blocked=true;
        require(attempt()==1 && selected==items[0].data(),"Occluded nearest item hid a reachable second item");
        near_blocked=false;eligible[1]=false;
        require(attempt()==1 && selected==items[0].data(),"Ineligible near item bypassed native pickup rules");
        eligible[1]=true;inventory[1]=false;
        require(attempt()==1 && selected==items[0].data() && refusals==1,
            "Native inventory refusal did not retain its normal notification");
        inventory[1]=true;hand_blocked=true;
        auto before=starts;
        require(attempt()==0 && starts==before,"A hand through a wall grabbed an item");
        hand_blocked=false;
        require(attempt({0,0,50})==0 && starts==before,"Remote items were admitted by native body range");
        ready=false;
        require(attempt()==0 && starts==before,"Native scripted ownership was bypassed");
        ready=true;begin_ok=false;
        require(attempt()==0 && starts==before+1,"Failed native animation admission tried another item");
        begin_ok=true;
        pickup_write(hero.data(),0x4394,80.f);
        before=starts;
        require(attempt()==0 && starts==before,"Hand reach bypassed the native body-reach constraint");
        std::cout<<"PASS: native_hand_pickup; gaze_independent; nearest_eligible; "
            "hand_and_item_occlusion; bounded_hand_and_body_reach; inventory_refusal; "
            "native_special_move_ownership; immutable_native_candidate_array\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;
    }
}
