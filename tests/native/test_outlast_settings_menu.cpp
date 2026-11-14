#include "outlast_settings_menu.h"
#include "outlast_desktop_cursor.h"
#include <iostream>
using namespace olvr;
namespace {
bool input_owned=true;
bool WINAPI owned_menu() {return input_owned;}
bool WINAPI owned_client(HWND) {return true;}
}
int main() {
    try {
        auto require=[](bool pass,const char* message) {
            if (!pass) throw std::runtime_error(message);
        };
        OutlastConfig config;
        OutlastSettingsMenu menu;menu.configure(config);
        std::array<bool,7> keys{};
        auto press=[&](unsigned key) {
            menu.keyboard({},true);keys={};keys[key]=true;menu.keyboard(keys,true);
        };
        press(0);
        require(menu.visible(),"F10 did not open settings");
        menu.keyboard(keys,true);
        require(menu.visible(),"Held F10 repeatedly toggled settings");
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
        press(1);require(!menu.visible(),"Escape did not close settings");
        require(menu.captures_input(),"Closing Escape leaked into the native pause menu");
        menu.keyboard(keys,true);
        require(menu.captures_input(),"Held closing Escape released native input ownership");
        menu.keyboard({},true);
        require(!menu.captures_input(),"Native input did not recover after the closing key was released");
        menu.keyboard({},false);keys={};keys[0]=true;menu.keyboard(keys,true);
        require(!menu.visible(),"Held F10 from another application opened the game menu");
        press(0);require(menu.visible(),"Fresh focused F10 did not recover");
        const auto instance=GetModuleHandleW(nullptr);
        WNDCLASSW type{};type.lpfnWndProc=&DefWindowProcW;type.hInstance=instance;
        type.lpszClassName=L"OutlastVRSettingsInputTest";
        require(RegisterClassW(&type)!=0,"Cannot register settings input test window");
        const auto window=CreateWindowExW(0,type.lpszClassName,L"",WS_OVERLAPPED,
            0,0,32,32,nullptr,nullptr,instance,nullptr);
        require(window!=nullptr,"Cannot create hidden settings input test window");
        {
            OutlastDesktopCursor cursor(0x1000,0x100);
            OutlastWindowCursor hook(window,cursor,&owned_client,&owned_menu);
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
