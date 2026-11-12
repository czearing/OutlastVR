#pragma once
#include "outlast_vr_hints.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace olvr {
class OutlastNativeCursor {
    void* owner_;
    static NativeUiString image_path() {
        static constexpr wchar_t path[]=L"_root.mouseCursor.my_cursor._visible";
        constexpr INT count=sizeof(path)/sizeof(path[0]);
        return {const_cast<wchar_t*>(path),count,count};
    }
public:
    explicit OutlastNativeCursor(std::uintptr_t owner):owner_(reinterpret_cast<void*>(owner)) {
        if (!owner_) throw std::invalid_argument("Missing native menu cursor owner");
    }
    bool visible() const {
        // Recovered from the retail execGetVariableBool / execSetVariableBool wrappers.
        using Get=INT(WINAPI*)(void*,const NativeUiString*);
        auto path=image_path();
        return reinterpret_cast<Get>((*static_cast<void***>(owner_))[0x2a0/sizeof(void*)])(owner_,&path)!=0;
    }
    void visible(bool value) const {
        using Set=void(WINAPI*)(void*,const NativeUiString*,INT);
        auto path=image_path();
        reinterpret_cast<Set>((*static_cast<void***>(owner_))[0x2d0/sizeof(void*)])(owner_,&path,value?1:0);
        if (visible()!=value) throw std::runtime_error("Native software cursor visibility did not persist");
    }
};
struct OutlastCursorMovie {
    std::uintptr_t owner=0,view=0;
    bool operator==(const OutlastCursorMovie& other) const {return owner==other.owner && view==other.view;}
};
class OutlastMenuCursor {
    std::vector<OutlastCursorMovie> hidden_;
public:
    bool pending() const {return !hidden_.empty();}
    template<class Read,class Write>
    void update(bool pointer,const std::vector<OutlastCursorMovie>& movies,Read&& read,Write&& write) {
        hidden_.erase(std::remove_if(hidden_.begin(),hidden_.end(),[&](const auto& movie) {
            return std::find(movies.begin(),movies.end(),movie)==movies.end();
        }),hidden_.end());
        for (const auto& movie:movies) {
            const bool hidden=std::find(hidden_.begin(),hidden_.end(),movie)!=hidden_.end();
            if (pointer && read(movie)) {
                write(movie,false);
                if (!hidden) hidden_.push_back(movie);
            } else if (!pointer && hidden) write(movie,true);
        }
        if (!pointer) hidden_.clear();
    }
};
}
