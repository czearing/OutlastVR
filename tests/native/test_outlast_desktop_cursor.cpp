#include "outlast_desktop_cursor.h"
#include <iostream>

namespace {
unsigned checks=0,clips=0,positions=0;
const RECT* last_rectangle=nullptr;
int last_x=0,last_y=0;
BOOL result=TRUE;
BOOL WINAPI clip(const RECT* rectangle) {++clips;last_rectangle=rectangle;return result;}
BOOL WINAPI position(int x,int y) {++positions;last_x=x;last_y=y;return result;}
BOOL WINAPI get_position(POINT* point) {if (point) *point={-300,700};return result;}
void require(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("Desktop cursor ownership/forwarding regression");
}
}
int main() {
    try {
        constexpr std::uintptr_t base=0x140000000ull,extent=0x3000000;
        olvr::OutlastDesktopCursor policy(base,extent);
        const RECT rectangle{100,100,900,700};
        for (const auto caller:{base,base+12345,base+extent-1}) {
            require(policy.owns(caller));
            require(policy.clip(&clip,caller,&rectangle)==TRUE && clips==0);
            require(policy.position(&position,caller,500,400)==TRUE && positions==0);
        }
        for (const auto caller:{std::uintptr_t(0),base-1,base+extent,base+extent+1}) {
            require(!policy.owns(caller));
            const auto before_clips=clips,before_positions=positions;
            result=FALSE;
            require(policy.clip(&clip,caller,&rectangle)==FALSE && clips==before_clips+1 &&
                last_rectangle==&rectangle);
            require(policy.position(&position,caller,-1920,1440)==FALSE &&
                positions==before_positions+1 && last_x==-1920 && last_y==1440);
        }
        const auto before=clips;
        require(policy.clip(&clip,base,nullptr)==FALSE && clips==before+1 && last_rectangle==nullptr);
        unsigned rejected=0;
        for (const auto range:{std::pair<std::uintptr_t,std::size_t>{0,1},{base,0},
                {std::numeric_limits<std::uintptr_t>::max(),2}}) {
            try {olvr::OutlastDesktopCursor invalid(range.first,range.second);}
            catch (const std::invalid_argument&) {++rejected;}
        }
        require(rejected==3);
        const auto native=olvr::outlast_menu_source_point(.5f,.55f,3840,2160);
        require(native.x==1920 && native.y==1187);
        const auto first=olvr::outlast_menu_source_point(0,0,3840,2160);
        const auto last=olvr::outlast_menu_source_point(1,1,3840,2160);
        require(first.x==0 && first.y==0 && last.x==3839 && last.y==2159);
        const auto reduced=olvr::outlast_menu_source_point(.5f,.55f,1286,760);
        require(reduced.x==643 && reduced.y==417 && native.y!=reduced.y);
        for (const auto uv:{std::pair<float,float>{-.01f,.5f},{1.01f,.5f},{.5f,-.01f},
            {.5f,1.01f},{std::numeric_limits<float>::quiet_NaN(),.5f}}) {
            bool invalid=false;
            try {olvr::outlast_menu_source_point(uv.first,uv.second,3840,2160);}
            catch (const std::invalid_argument&) {invalid=true;}
            require(invalid);
        }
        for (const auto size:{std::pair<UINT,UINT>{0,2160},{3840,0},{32769,2160},{3840,32769}}) {
            bool invalid=false;
            try {olvr::outlast_menu_source_point(.5f,.5f,size.first,size.second);}
            catch (const std::invalid_argument&) {invalid=true;}
            require(invalid);
        }
        olvr::ControllerMenuPress press;
        require(!press.update(true,true,1,1));
        require(!press.update(true,true,1,0));
        require(press.update(true,true,1,1));
        require(press.update(true,true,1,.5f));
        require(!press.update(false,false,1,1));
        require(!press.update(true,true,1,1));
        require(!press.update(true,true,1,0));
        require(!press.update(true,false,1,1));
        require(!press.update(true,true,1,1));
        require(!press.update(true,true,1,0));
        require(press.update(true,true,1,1));
        require(!press.update(true,true,0,1));
        require(!press.update(true,true,0,0));
        require(press.update(true,true,0,1));
        require(!press.update(true,false,0,1));
        require(!press.update(true,true,0,1));
        POINT virtual_position{};
        require(!policy.menu_position());
        policy.menu_pointer(true,{840,320});
        require(policy.menu_position() && policy.menu_position()->x==840 && policy.menu_position()->y==320);
        require(policy.get_position(&get_position,base,&virtual_position)==TRUE &&
            virtual_position.x==840 && virtual_position.y==320);
        require(policy.get_position(&get_position,base-1,&virtual_position)==FALSE &&
            virtual_position.x==-300 && virtual_position.y==700);
        policy.menu_pointer(false);
        require(!policy.menu_position());
        require(policy.get_position(&get_position,base,&virtual_position)==FALSE &&
            virtual_position.x==-300 && virtual_position.y==700);
        policy.menu_pointer(true,{-1,-1});
        require(policy.menu_position() && policy.menu_position()->x==-1 && policy.menu_position()->y==-1);
        policy.menu_pointer(false);
        const GUID mouse{0x6f1d2b60,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0}};
        auto keyboard=mouse;++keyboard.Data1;
        require(olvr::outlast_desktop_mouse(mouse) && !olvr::outlast_desktop_mouse(keyboard));
        for (const DWORD flags:{DWORD(DISCL_EXCLUSIVE|DISCL_FOREGROUND),
                DWORD(DISCL_EXCLUSIVE|DISCL_BACKGROUND),DWORD(DISCL_NONEXCLUSIVE|DISCL_FOREGROUND)}) {
            const auto changed=olvr::outlast_mouse_cooperation(flags);
            require(!(changed&DISCL_EXCLUSIVE) && (changed&DISCL_NONEXCLUSIVE));
            require((changed&~DWORD(DISCL_EXCLUSIVE|DISCL_NONEXCLUSIVE))==
                (flags&~DWORD(DISCL_EXCLUSIVE|DISCL_NONEXCLUSIVE)));
        }
        std::cout<<"PASS: desktop_cursor_checks="<<checks
            <<"; native_clip_and_warp_suppressed; external_callers_and_errors_preserved"
            <<"; release_preserved; no_real_cursor_moves_or_confinement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
