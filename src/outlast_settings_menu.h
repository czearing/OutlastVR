#pragma once
#include "outlast_config.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

namespace olvr {
class OutlastSettingsMenu {
public:
    enum Option : unsigned {hit_distortion,bloom,film_grain,body_follow,body_deadzone,
        pickup_radius,vignette,count};
    static constexpr unsigned width=512,height=512,first_row=96,row_height=42;
private:
    std::atomic<unsigned> enabled_{(1u<<bloom)|(1u<<film_grain)|(1u<<body_follow)};
    std::atomic<float> deadzone_{35},radius_{.30f},vignette_{0};
    std::atomic<unsigned> visibility_{0}; // Bit 0: visible; bit 1: retain the closing key.
    unsigned selected_=0,revision_=1;
    std::array<bool,7> keys_{};
    bool mouse_down_=true,controller_down_=true;
public:
    void configure(const OutlastConfig& config) {
        enabled_=(config.comfort.hit_distortion?1u<<hit_distortion:0)|
            (config.comfort.bloom?1u<<bloom:0)|(config.comfort.film_grain?1u<<film_grain:0)|
            (config.comfort.body_follow?1u<<body_follow:0);
        deadzone_=config.comfort.body_follow_deadzone;
        radius_=config.comfort.hand_pickup_radius;vignette_=config.comfort.vignette_strength;
        ++revision_;
    }
    bool visible() const {return (visibility_.load()&1u)!=0;}
    bool captures_input() const {return visibility_.load()!=0;}
    bool enabled(Option option) const {return (enabled_.load()&(1u<<option))!=0;}
    float deadzone() const {return deadzone_.load();}
    float radius() const {return radius_.load();}
    float vignette_strength() const {return vignette_.load();}
    unsigned revision() const {return revision_;}
    unsigned selected() const {return selected_;}
    void toggle() {
        visibility_=visible()?2u:1u;
        mouse_down_=controller_down_=true;++revision_;
    }
    void adjust(int direction=1) {
        if (!direction) return;
        if (selected_==body_deadzone)
            deadzone_=std::clamp(deadzone()+direction*5.f,10.f,90.f);
        else if (selected_==pickup_radius)
            radius_=std::clamp(radius()+direction*.05f,.1f,.6f);
        else if (selected_==vignette)
            vignette_=std::clamp(vignette_strength()+direction*.1f,0.f,1.f);
        else enabled_.fetch_xor(1u<<selected_);
        ++revision_;
    }
    void keyboard(const std::array<bool,7>& down,bool focused) {
        if (!focused || (!down[0] && !down[1])) visibility_.fetch_and(1u);
        std::array<bool,7> edge{};
        for (unsigned i=0;i<down.size();++i) edge[i]=focused && down[i] && !keys_[i];
        keys_=focused?down:std::array<bool,7>{true,true,true,true,true,true,true};
        if (edge[0]) toggle();
        if (!visible()) return;
        if (edge[1]) {visibility_=2;++revision_;return;}
        if (edge[2]) {selected_=(selected_+count-1)%count;++revision_;}
        if (edge[3]) {selected_=(selected_+1)%count;++revision_;}
        if (edge[4]) adjust(-1);
        if (edge[5] || edge[6]) adjust(1);
    }
    void point(float u,float v,bool down,bool controller,bool available=true) {
        auto& previous=controller?controller_down_:mouse_down_;
        if (!available) {previous=true;return;}
        const bool pressed=down && !previous;
        previous=down;
        if (!visible() || !std::isfinite(u) || !std::isfinite(v) || u<0 || u>1 || v<0 || v>1)
            return;
        const int row=int(v*height)-int(first_row);
        if (row<0 || unsigned(row)/row_height>=count) return;
        if (!controller && !pressed) return;
        const unsigned target=unsigned(row)/row_height;
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
        const char* labels[]{"Hit nausea / rotation / zoom","Bloom","Film grain","Body follows head",
            "Body follow deadzone","Hand pickup distance","Movement vignette"};
        for (unsigned i=0;i<count;++i) {
            const unsigned y=first_row+i*row_height;
            if (i==selected_) for (unsigned row=y;row<y+row_height-2;++row)
                std::fill_n(output+row*width+12,width-24,0x002a3c50);
            text(24,int(y+9),labels[i]);
            std::string value=enabled(Option(i))?"ON":"OFF";
            if (i==body_deadzone) value=std::to_string(int(std::lround(deadzone())))+" deg";
            if (i==pickup_radius) value=std::to_string(int(std::lround(radius()*100)))+" cm";
            if (i==vignette) value=std::to_string(int(std::lround(vignette_strength()*100)))+" %";
            text(408,int(y+9),value,RGB(100,205,255));
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
