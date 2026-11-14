#include "outlast_settings_menu.h"
#include "outlast_desktop_cursor.h"
#include "outlast_desktop_mirror.h"
#include <iostream>
using namespace olvr;
namespace {
bool input_owned=true;
OutlastSettingsMenu* active_menu=nullptr;
bool WINAPI owned_menu() {return input_owned || (active_menu && active_menu->captures_input());}
bool WINAPI owned_client(HWND) {return true;}
bool WINAPI settings_key(UINT message,WPARAM key,LPARAM bits) {
    return active_menu && active_menu->message(message,key,bits);
}
}
int main() {
    try {
        auto require=[](bool pass,const char* message) {
            if (!pass) throw std::runtime_error(message);
        };
        OutlastConfig config;
        OutlastSettingsMenu menu;menu.configure(config);
        constexpr int codes[]{VK_F10,VK_ESCAPE,VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN};
        auto press=[&](unsigned key) {
            menu.message(WM_KEYDOWN,codes[key],0);
            menu.message(WM_KEYUP,codes[key],0);
            menu.poll_messages();
        };
        press(0);
        require(menu.visible(),"F10 did not open settings");
        menu.message(WM_KEYDOWN,VK_F10,LPARAM(1)<<30);menu.poll_messages();
        require(menu.visible(),"Held F10 repeatedly toggled settings");
        menu.message(WM_KEYUP,VK_F10,0);menu.poll_messages();
        require(!menu.enabled(OutlastSettingsMenu::hit_distortion),"Nausea unexpectedly enabled by default");
        press(6);
        require(menu.enabled(OutlastSettingsMenu::hit_distortion),"Keyboard toggle not immediately visible");
        for (unsigned i=0;i<OutlastSettingsMenu::body_deadzone;++i) press(3);
        require(menu.selected()==OutlastSettingsMenu::body_deadzone,"Row navigation skipped an option");
        for (unsigned i=0;i<50;++i) press(4);
        require(menu.deadzone()==10,"Deadzone lower bound lost");
        for (unsigned i=0;i<50;++i) press(5);
        require(menu.deadzone()==90,"Deadzone upper bound lost");
        press(3);
        for (unsigned i=0;i<50;++i) press(4);
        require(menu.radius()==.1f,"Pickup radius lower bound lost");
        for (unsigned i=0;i<50;++i) press(5);
        require(menu.radius()==.6f,"Pickup radius upper bound lost");
        const float row=(float(OutlastSettingsMenu::first_row)+OutlastSettingsMenu::row_height*1.5f)/
            OutlastSettingsMenu::height;
        const bool bloom=menu.enabled(OutlastSettingsMenu::bloom);
        menu.point(.9f,row,false,true);menu.point(.9f,row,true,true);
        require(menu.enabled(OutlastSettingsMenu::bloom)!=bloom,"Controller click did not toggle bloom");
        menu.point(.9f,row,true,true);
        require(menu.enabled(OutlastSettingsMenu::bloom)!=bloom,"Held trigger repeatedly toggled a row");
        menu.point(.9f,row,false,false);menu.point(.9f,row,true,false);
        require(menu.enabled(OutlastSettingsMenu::bloom)==bloom,"Mouse click did not toggle bloom");
        menu.point(.9f,row,false,true,false);
        menu.point(.9f,row,true,true);
        require(menu.enabled(OutlastSettingsMenu::bloom)==bloom,"Lost tracking rearmed a held trigger");
        menu.point(.9f,row,false,true);menu.point(.9f,row,true,true);
        require(menu.enabled(OutlastSettingsMenu::bloom)!=bloom,"Fresh trigger failed after tracking recovery");
        const auto image=menu.pixels();
        require(image.size()==menu.width*menu.height,"Menu raster has the wrong extent");
        unsigned text=0;
        for (auto color:image) {
            require((color>>24)==255,"Menu text is not opaque");
            if ((color&0xffffff)==0x64cdff) ++text;
        }
        require(text>100,"Live settings labels are absent from actual raster");
        menu.message(WM_KEYDOWN,VK_ESCAPE,0);menu.poll_messages();
        require(!menu.visible(),"Escape did not close settings");
        require(menu.captures_input(),"Closing Escape leaked into the native pause menu");
        menu.poll_messages();
        require(menu.captures_input(),"Held closing Escape released native input ownership");
        menu.message(WM_KEYUP,VK_ESCAPE,0);
        menu.poll_messages();
        require(!menu.captures_input(),"Native input did not recover after the closing key was released");
        menu.message(WM_KEYDOWN,VK_F10,0);menu.message(WM_KILLFOCUS,0,0);menu.poll_messages();
        require(!menu.visible(),"Losing focus retained a stale opening key");
        menu.message(WM_KEYDOWN,VK_F10,LPARAM(1)<<30);menu.poll_messages();
        require(!menu.visible(),"A held F10 opened settings after focus recovery");
        menu.message(WM_KEYUP,VK_F10,0);
        press(0);require(menu.visible(),"Fresh focused F10 did not recover");
        press(1);menu.poll_messages();
        menu.message(WM_SYSKEYDOWN,VK_F10,0);menu.message(WM_SYSKEYUP,VK_F10,0);
        menu.message(WM_SYSKEYDOWN,VK_F10,0);menu.message(WM_SYSKEYUP,VK_F10,0);
        menu.poll_messages();
        require(!menu.visible(),"Two fast F10 taps lost their ordering");
        std::istringstream configured("[vr]\nbloom=false\nbody_follow_deadzone=55\nhand_pickup_radius=.45\n"
            "movement_reference=off_hand\nworld_scale=1.5\n");
        const auto defaults=parse_outlast_config(configured);
        menu.configure(defaults);
        for (unsigned i=0;i<outlast_settings.size();++i)
            require(menu.value(i)==outlast_settings[i].value(defaults),"Menu diverged from a parsed INI setting");
        require(menu.formatted(OutlastSettingsMenu::bloom)=="OFF" &&
            menu.formatted(OutlastSettingsMenu::body_deadzone)=="55 deg" &&
            menu.formatted(OutlastSettingsMenu::pickup_radius)=="45 cm" &&
            menu.formatted(outlast_setting_index("vr.movement_reference"))=="off_hand",
            "Configured menu values are incorrectly formatted");
        press(0);
        const auto initial=menu.selected();
        std::set<unsigned> visited;
        for (unsigned row=0;row<menu.count;++row) {
            require(outlast_settings[menu.selected()].in_menu(),"An advanced or non-live setting cluttered the player menu");
            require(visited.insert(menu.selected()).second,"Settings navigation repeated a row before wrapping");
            press(3);
        }
        require(menu.selected()==initial,"Player menu did not wrap after the visible options");
        for (unsigned option=0;option<outlast_settings.size();++option)
            require(bool(visited.count(option))==outlast_settings[option].in_menu(),
                "Player menu omitted a live option or exposed an advanced setting");
        require(menu.value(outlast_setting_index("vr.world_scale"))==1.5f,
            "Browsing the player menu changed a startup-only preference");
        press(1);menu.poll_messages();
        const auto instance=GetModuleHandleW(nullptr);
        WNDCLASSW type{};type.lpfnWndProc=&DefWindowProcW;type.hInstance=instance;
        type.lpszClassName=L"OutlastVRSettingsInputTest";
        require(RegisterClassW(&type)!=0,"Cannot register settings input test window");
        const auto window=CreateWindowExW(0,type.lpszClassName,L"",WS_OVERLAPPED,
            0,0,32,32,nullptr,nullptr,instance,nullptr);
        require(window!=nullptr,"Cannot create hidden settings input test window");
        {
            OutlastDesktopCursor cursor(0x1000,0x100);
            OutlastSettingsMenu transport;active_menu=&transport;
            OutlastWindowCursor hook(window,cursor,&owned_client,&owned_menu,&settings_key);
            MSG message{};
            while (PeekMessageW(&message,window,0,0,PM_REMOVE)) {}
            for (const auto code:{WM_KEYDOWN,WM_KEYUP,WM_CHAR,WM_LBUTTONDOWN,WM_LBUTTONUP,WM_MOUSEMOVE}) {
                require(PostMessageW(window,UINT(code),0,0)!=FALSE,"Cannot queue isolated settings input");
                require(PeekMessageW(&message,window,0,0,PM_REMOVE)!=FALSE && message.message==WM_NULL,
                    "Settings keyboard or mouse click leaked to native gameplay");
            }
            require(PostMessageW(window,WM_APP+1,0,0)!=FALSE &&
                PeekMessageW(&message,window,0,0,PM_REMOVE)!=FALSE && message.message==WM_APP+1,
                "Settings blocked an unrelated native window message");
            input_owned=false;
            require(PostMessageW(window,WM_KEYDOWN,0,0)!=FALSE &&
                PeekMessageW(&message,window,0,0,PM_REMOVE)!=FALSE && message.message==WM_KEYDOWN,
                "Closing settings did not restore native keyboard messages");
            for (unsigned cycle=0;cycle<100;++cycle) {
                for (const auto code:{WM_SYSKEYDOWN,WM_SYSKEYUP}) {
                    require(PostMessageW(window,UINT(code),VK_F10,0)!=FALSE &&
                        PeekMessageW(&message,window,0,0,PM_REMOVE)!=FALSE && message.message==WM_NULL,
                        "F10 reached the native Windows system-menu handler");
                }
                transport.poll_messages();
                require(transport.visible()==(cycle%2==0),"Queued F10 toggle was missed or repeated");
            }
            transport.poll_messages();
            {
                OutlastDesktopMirror mirror(&settings_key);
                for (const auto code:{WM_SYSKEYDOWN,WM_SYSKEYUP})
                    SendMessageW(mirror.window(),UINT(code),VK_F10,0);
                transport.poll_messages();
                require(transport.visible(),"F10 in the spectator window did not open settings");
                for (const auto code:{WM_KEYDOWN,WM_KEYUP})
                    SendMessageW(mirror.window(),UINT(code),VK_ESCAPE,0);
                transport.poll_messages();
                require(!transport.visible(),"Escape in the spectator window did not close settings");
            }
            active_menu=nullptr;
        }
        require(DestroyWindow(window)!=FALSE && UnregisterClassW(type.lpszClassName,instance)!=FALSE,
            "Settings input hook did not release its hidden test window");
        std::cout<<"PASS: live_settings_keyboard_mouse_controller; bounded_values; "
            "focus_and_held_input_recovery; opaque_native_text_raster\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;
    }
}
