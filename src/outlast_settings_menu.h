#pragma once
#include "outlast_config.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>
#include <mutex>
#include <iomanip>

namespace olvr {
class OutlastSettingsMenu {
public:
    using Option=unsigned;
    static constexpr unsigned hit_distortion=outlast_setting_index("vr.hit_distortion"),
        bloom=outlast_setting_index("vr.bloom"),film_grain=outlast_setting_index("vr.film_grain"),
        body_follow=outlast_setting_index("vr.body_follow"),body_deadzone=outlast_setting_index("vr.body_follow_deadzone"),
        pickup_radius=outlast_setting_index("vr.hand_pickup_radius"),vignette=outlast_setting_index("vr.vignette_strength"),
        count=outlast_menu_settings.size(),visible_rows=7;
    static constexpr unsigned width=512,height=512,first_row=96,row_height=42,
        page_top=first_row+visible_rows*row_height+12;
private:
    std::array<std::atomic<float>,outlast_settings.size()> values_{};
    std::atomic<unsigned> visibility_{0}; // Bit 0: visible; bit 1: retain the closing key.
    std::atomic<bool> pending_capture_{false};
    unsigned selected_=0,revision_=1;
    bool mouse_down_=true,controller_down_=true;
    std::mutex input_mutex_;
    std::vector<unsigned> pending_keys_;
    unsigned held_keys_=0;
    void press(unsigned key) {
        if (key==0) toggle();
        if (!visible()) return;
        if (key==1) {visibility_=2;++revision_;return;}
        if (key==2) {selected_=(selected_+count-1)%count;++revision_;}
        if (key==3) {selected_=(selected_+1)%count;++revision_;}
        if (key==4) adjust(-1);
        if (key==5 || key==6) adjust(1);
    }
public:
    OutlastSettingsMenu() {configure({});}
    void configure(const OutlastConfig& config) {
        for (unsigned i=0;i<outlast_settings.size();++i) values_[i]=outlast_settings[i].value(config);
        ++revision_;
    }
    bool visible() const {return (visibility_.load()&1u)!=0;}
    bool captures_input() const {return visibility_.load()!=0 || pending_capture_.load();}
    float value(Option option) const {return values_.at(option).load();}
    bool enabled(Option option) const {return value(option)!=0;}
    float deadzone() const {return value(body_deadzone);}
    float radius() const {return value(pickup_radius);}
    float vignette_strength() const {return value(vignette);}
    unsigned revision() const {return revision_;}
    unsigned selected() const {return outlast_menu_settings[selected_];}
    unsigned first_visible() const {return selected_/visible_rows*visible_rows;}
    std::string label(Option option) const {
        const auto& setting=outlast_settings.at(option);
        return std::string(setting.label);
    }
    std::string formatted(Option option) const {
        const auto& setting=outlast_settings.at(option);
        const float current=value(option);
        if (setting.boolean()) return current?"ON":"OFF";
        if (setting.path()) return current?"Custom":"Default";
        if (setting.choice()) return std::string(outlast_movement_references.at(unsigned(current)));
        std::ostringstream text;text.imbue(std::locale::classic());
        const auto unit=setting.unit;
        const float scale=unit=="%" || unit=="m"?100.f:1.f;
        text<<std::fixed<<std::setprecision(unit.empty()?2:0)<<current*scale;
        if (!unit.empty()) text<<' '<<(unit=="m"?"cm":unit);
        return text.str();
    }
    void toggle() {
        visibility_=visible()?2u:1u;
        mouse_down_=controller_down_=true;++revision_;
    }
    void adjust(int direction=1) {
        const auto option=selected();
        const auto& setting=outlast_settings[option];
        if (!direction) return;
        const float current=value(option);
        values_[option]=setting.boolean()?float(!current):
            std::clamp(current+direction*setting.step,setting.minimum,setting.maximum);
        ++revision_;
    }
    bool message(UINT message,WPARAM key,LPARAM bits) {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (message==WM_KILLFOCUS) {
            held_keys_=0;pending_keys_.clear();pending_capture_=false;return false;
        }
        const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
        if (!down && message!=WM_KEYUP && message!=WM_SYSKEYUP) return false;
        constexpr int codes[]{VK_F10,VK_ESCAPE,VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN};
        const auto found=std::find(std::begin(codes),std::end(codes),int(key));
        if (found==std::end(codes)) return false;
        const bool capture=key==VK_F10 || captures_input();
        const auto index=unsigned(found-std::begin(codes));
        if (down) {
            if (capture && !(bits&(LPARAM(1)<<30))) {
                if (pending_keys_.size()==256) throw std::runtime_error("Settings keyboard queue is not being consumed");
                pending_keys_.push_back(index);
                pending_capture_=true;
            }
            held_keys_|=1u<<index;
        } else held_keys_&=~(1u<<index);
        return capture;
    }
    void poll_messages() {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (!(held_keys_&3u)) visibility_.fetch_and(1u);
        for (const auto key:pending_keys_) press(key);
        pending_keys_.clear();pending_capture_=false;
    }
    void point(float u,float v,bool down,bool controller,bool available=true) {
        auto& previous=controller?controller_down_:mouse_down_;
        if (!available) {previous=true;return;}
        const bool pressed=down && !previous;
        previous=down;
        if (!visible() || !std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1)
            return;
        if (count>visible_rows && v*height>=page_top && v*height<page_top+row_height && u>=.55f) {
            if (pressed) {
                const auto page=first_visible()/visible_rows,pages=(count+visible_rows-1)/visible_rows;
                selected_=(page+(u<.75f?pages-1:1))%pages*visible_rows;
                ++revision_;
            }
            return;
        }
        const int row=int(v*height)-int(first_row);
        if (row<0 || unsigned(row)/row_height>=visible_rows ||
            first_visible()+unsigned(row)/row_height>=count) return;
        if (!controller && !pressed) return;
        const unsigned target=first_visible()+unsigned(row)/row_height;
        if (target!=selected_) {selected_=target;++revision_;}
        if (pressed) adjust(u<.70f?-1:1);
    }
    std::vector<DWORD> pixels() const {
        struct Canvas {
            HDC dc=nullptr;
            HBITMAP bitmap=nullptr;
            HFONT font=nullptr;
            HGDIOBJ old_bitmap=nullptr,old_font=nullptr;
            ~Canvas() {
                if (old_font) SelectObject(dc,old_font);
                if (old_bitmap) SelectObject(dc,old_bitmap);
                if (font) DeleteObject(font);
                if (bitmap) DeleteObject(bitmap);
                if (dc) DeleteDC(dc);
            }
        } canvas;
        canvas.dc=CreateCompatibleDC(nullptr);
        if (!canvas.dc) throw std::runtime_error("Cannot create settings menu text canvas");
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-LONG(height);
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* data=nullptr;
        canvas.bitmap=CreateDIBSection(canvas.dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
        if (!canvas.bitmap || !data) throw std::runtime_error("Cannot allocate settings menu image");
        canvas.old_bitmap=SelectObject(canvas.dc,canvas.bitmap);
        canvas.font=CreateFontW(-21,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if (!canvas.font) throw std::runtime_error("Cannot create settings menu font");
        canvas.old_font=SelectObject(canvas.dc,canvas.font);
        auto* output=static_cast<DWORD*>(data);
        std::fill_n(output,width*height,0x00151a21);
        if (!SetBkMode(canvas.dc,TRANSPARENT)) throw std::runtime_error("Cannot configure settings text");
        auto text=[&](int x,int y,const std::string& value,COLORREF color=RGB(230,235,240)) {
            if (SetTextColor(canvas.dc,color)==CLR_INVALID ||
                !TextOutA(canvas.dc,x,y,value.c_str(),int(value.size())))
                throw std::runtime_error("Cannot render settings menu text");
        };
        text(24,18,"OUTLAST VR  /  SETTINGS",RGB(100,205,255));
        text(24,49,"F10 / Esc close     Arrows / Enter change");
        const auto first=first_visible(),last=std::min(first+visible_rows,count);
        for (unsigned i=first;i<last;++i) {
            const unsigned y=first_row+(i-first)*row_height;
            if (i==selected_) for (unsigned row=y;row<y+row_height-2;++row)
                std::fill_n(output+row*width+12,width-24,0x002a3c50);
            const auto option=outlast_menu_settings[i];
            text(24,int(y+9),label(option));
            text(380,int(y+9),formatted(option),RGB(100,205,255));
        }
        if (count>visible_rows) {
            text(24,page_top+8,std::to_string(first+1)+"-"+std::to_string(last)+" / "+std::to_string(count));
            text(280,page_top+8,"< Back");text(400,page_top+8,"More >");
        }
        text(24,452,"Changes apply immediately for this session.");
        text(24,480,"Point and trigger, or click a setting.");
        if (!GdiFlush()) throw std::runtime_error("Cannot finish rendering settings menu image");
        std::vector<DWORD> result(output,output+width*height);
        for (auto& pixel:result) pixel|=0xff000000;
        return result;
    }
};
}
