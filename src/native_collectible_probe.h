#pragma once
#include "native_hook_set.h"
#include "outlast_native_image.h"
#include "outlast_runtime_api.h"
#include <atomic>
#include <fstream>
#include <mutex>

namespace olvr {

class NativeCollectibleProbe {
public:
    struct Snapshot {
        std::uintptr_t hero=0,item=0,selected=0;
        Vec3 root{},view{},forward{},target{};
        UINT flags=0;
        BYTE basic=0,special=0;
        bool steam_disabled=false;
    };
private:
    using Dispatch=UINT64(WINAPI*)(void*);
    using Notify=void(WINAPI*)(void*);
    using Collect=void(WINAPI*)(void*,void*);
    inline static NativeCollectibleProbe* instance_=nullptr;
    std::filesystem::path evidence_;
    std::uintptr_t base_;
    std::ofstream trace_;
    std::mutex mutex_;
    Snapshot latest_{};
    std::atomic<UINT64> world_{0},notify_world_{0};
    std::atomic<bool> collected_{false};
    Dispatch original_dispatch_=nullptr;
    Notify original_notify_=nullptr;
    Collect original_collect_=nullptr;
    NativeHookSet hooks_;

    template<class T> static T read(const void* object,std::size_t offset) {
        if (!object) throw std::invalid_argument("Null native collectible observer input");
        T value{};std::memcpy(&value,static_cast<const BYTE*>(object)+offset,sizeof(value));return value;
    }
    std::string native_name(UINT64 tag) const {
        const auto index=UINT(tag),number=UINT(tag>>32);
        const auto* image=reinterpret_cast<const void*>(base_);
        const auto count=read<INT>(image,0x1ff8fa8);
        const auto* names=read<void* const*>(image,0x1ff8fa0);
        if (count<0 || count>1000000 || index>=UINT(count) || !names)
            throw std::runtime_error("Invalid observed native animation name");
        const auto* entry=names[index];
        const bool wide=(read<BYTE>(entry,8)&1)!=0;
        std::string result;
        for (unsigned offset=0;offset<512;++offset) {
            const auto character=wide?read<wchar_t>(entry,0x14+2*offset):read<BYTE>(entry,0x14+offset);
            if (!character) return result+(number?"_"+std::to_string(number-1):"");
            if (character<32 || character>126)
                throw std::runtime_error("Non-ASCII native animation identity requires explicit decoding");
            if (character=='"' || character=='\\') result+='\\';
            result+=char(character);
        }
        throw std::runtime_error("Unterminated observed native animation name");
    }
    Snapshot snapshot(void* hero) const {
        Snapshot result{};
        if (!hero) return result;
        result.hero=reinterpret_cast<std::uintptr_t>(hero);
        result.root=read<Vec3>(hero,0x80);
        result.view=read<Vec3>(hero,0x30e4);
        result.forward=read<Vec3>(hero,0x2bd8);
        result.basic=read<BYTE>(hero,0x7bc);result.special=read<BYTE>(hero,0x7bd);
        result.selected=read<std::uintptr_t>(hero,0x3648);
        const auto* image=reinterpret_cast<const void*>(base_);
        result.steam_disabled=read<UINT>(image,0x1ea89c0)==0 &&
            read<UINT>(image,0x201bd68)==0 && read<UINT>(image,0x201bd6c)==0;
        const auto count=read<INT>(hero,0x2c2c);
        const auto* items=read<void* const*>(hero,0x2c24);
        if (count<0 || count>4096 || (count && !items))
            throw std::runtime_error("Invalid native collectible candidate array");
        const auto* document_class=read<const void*>(reinterpret_cast<const void*>(base_),0x2020cb8);
        float nearest=0;
        for (INT index=0;index<count;++index) {
            auto* item=items[index];
            if (!item || !document_class || read<const void*>(item,0x50)!=document_class) continue;
            const auto target=read<Vec3>(item,0x80),delta=add(target,scale(result.root,-1));
            const auto distance=dot(delta,delta);
            if (!finite(target) || !std::isfinite(distance))
                throw std::runtime_error("Invalid observed native collectible location");
            if (result.item && distance>=nearest) continue;
            nearest=distance;result.item=reinterpret_cast<std::uintptr_t>(item);
            result.target=target;result.flags=read<UINT>(item,0x250);
        }
        return result;
    }
    struct Inventory {INT count=0;bool contains=false;};
    static Inventory inventory(const void* profile,std::size_t offset,UINT64 tag) {
        const auto* values=read<const UINT64*>(profile,offset);
        const auto count=read<INT>(profile,offset+8),capacity=read<INT>(profile,offset+12);
        if (count<0 || count>capacity || capacity>4096 || (count && !values))
            throw std::runtime_error("Invalid native collectible profile array");
        Inventory result{count,false};
        for (INT index=0;index<count;++index) {
            UINT64 value{};std::memcpy(&value,values+index,sizeof(value));
            result.contains=result.contains || value==tag;
        }
        return result;
    }
    void vector(Vec3 value) {trace_<<'['<<value.x<<','<<value.y<<','<<value.z<<']';}
    void flush() {
        trace_<<'\n';trace_.flush();
        if (!trace_) throw std::runtime_error("Cannot retain native collectible observer evidence");
    }
    [[noreturn]] void failure(const std::exception& error) const {
        std::ofstream output(evidence_/L"capture-error.txt",std::ios::app);
        output<<"Native collectible observer: "<<error.what()<<'\n';output.close();
        OutputDebugStringA(error.what());
        RaiseFailFastException(nullptr,nullptr,0);std::terminate();
    }
    static UINT64 WINAPI dispatch(void* controller) {
        auto& self=*instance_;
        try {
            const auto result=self.original_dispatch_(controller);
            const auto state=self.snapshot(read<void*>(controller,0xa4c));
            std::lock_guard<std::mutex> lock(self.mutex_);
            self.latest_=state;
            self.trace_<<"{\"kind\":\"state\",\"world\":"<<self.world_.load()
                <<",\"hero\":"<<state.hero<<",\"item\":"<<state.item
                <<",\"selected\":"<<state.selected<<",\"flags\":"<<state.flags
                <<",\"basic\":"<<UINT(state.basic)<<",\"special\":"<<UINT(state.special)
                <<",\"steam_disabled\":"<<(state.steam_disabled?"true":"false")
                <<",\"root\":";self.vector(state.root);
            self.trace_<<",\"native_view\":";self.vector(state.view);
            self.trace_<<",\"native_forward\":";self.vector(state.forward);
            self.trace_<<",\"target\":";self.vector(state.target);
            if (state.special==19) {
                auto* hero=reinterpret_cast<const void*>(state.hero);
                auto* slot=read<const void*>(hero,0x794);
                self.trace_<<",\"state19_animation\":\""<<self.native_name(read<UINT64>(hero,0x3b54))<<'"';
                self.trace_<<",\"native_slot\":"<<reinterpret_cast<std::uintptr_t>(slot);
                const auto count=slot?read<INT>(slot,0x130):0,index=slot?read<INT>(slot,0x148):-1;
                self.trace_<<",\"slot_count\":"<<count<<",\"slot_index\":"<<index;
                if (count<0 || count>256) throw std::runtime_error("Invalid observed native custom-animation slot");
                if (slot && index>=0 && index<count) {
                    const auto* children=read<const BYTE*>(slot,0x128);
                    if (!children) throw std::runtime_error("Native custom-animation slot lost its child array");
                    const auto* node=read<const void*>(children,std::size_t(index)*32+8);
                    self.trace_<<",\"native_sequence\":"<<reinterpret_cast<std::uintptr_t>(node);
                    if (node) {
                        self.trace_<<",\"active_animation\":\""<<self.native_name(read<UINT64>(node,0x128))
                            <<"\",\"animation_asset\":"<<read<UINT64>(node,0x144)
                            <<",\"sequence_words_0x130\":[";
                        for (unsigned word=0;word<12;++word) self.trace_<<(word?",":"")<<read<UINT>(node,0x130+word*4);
                        self.trace_<<']';
                    }
                }
            }
            self.trace_<<'}';self.flush();
            return result;
        } catch (const std::exception& error) {self.failure(error);}
    }
    static void WINAPI notify(void* hero) {
        auto& self=*instance_;
        try {
            const auto before=self.snapshot(hero);
            self.original_notify_(hero);
            const auto after=self.snapshot(hero);
            std::lock_guard<std::mutex> lock(self.mutex_);
            self.trace_<<"{\"kind\":\"notify\",\"world\":"<<self.world_.load()
                <<",\"hero\":"<<before.hero<<",\"selected_before\":"<<before.selected
                <<",\"selected_after\":"<<after.selected<<",\"special_before\":"<<UINT(before.special)
                <<",\"special_after\":"<<UINT(after.special)<<'}';self.flush();
            self.notify_world_.store(self.world_.load());
        } catch (const std::exception& error) {self.failure(error);}
    }
    static void WINAPI collect(void* item,void* hero) {
        auto& self=*instance_;
        try {
            const auto tag=read<UINT64>(item,0x26c);
            auto* controller=read<void*>(hero,0x2bbc);
            auto* profile=read<void*>(controller,0xa5c);
            auto* hud=read<void*>(controller,0xa54);
            const auto before=inventory(profile,0x70,tag),unread_before=inventory(profile,0x80,tag);
            const auto flags_before=read<UINT>(item,0x250);
            self.original_collect_(item,hero);
            const auto after=inventory(profile,0x70,tag),unread_after=inventory(profile,0x80,tag);
            const auto flags_after=read<UINT>(item,0x250);
            std::lock_guard<std::mutex> lock(self.mutex_);
            self.trace_<<"{\"kind\":\"collect\",\"world\":"<<self.world_.load()
                <<",\"hero\":"<<reinterpret_cast<std::uintptr_t>(hero)
                <<",\"item\":"<<reinterpret_cast<std::uintptr_t>(item)<<",\"tag\":"<<tag
                <<",\"flags_before\":"<<flags_before<<",\"flags_after\":"<<flags_after
                <<",\"collected_before\":"<<before.count<<",\"collected_after\":"<<after.count
                <<",\"unread_before\":"<<unread_before.count<<",\"unread_after\":"<<unread_after.count
                <<",\"had_tag_before\":"<<(before.contains?"true":"false")
                <<",\"has_tag_after\":"<<(after.contains?"true":"false")
                <<",\"unread_had_tag_before\":"<<(unread_before.contains?"true":"false")
                <<",\"unread_has_tag_after\":"<<(unread_after.contains?"true":"false")
                <<",\"hud_tag\":"<<read<UINT64>(hud,0x718)<<'}';self.flush();
            self.collected_.store(after.contains && unread_after.contains && (flags_after&1));
        } catch (const std::exception& error) {self.failure(error);}
    }
public:
    explicit NativeCollectibleProbe(const std::filesystem::path& evidence):
        evidence_(evidence),base_(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))) {
        if (instance_) throw std::runtime_error("Duplicate native collectible observer");
        std::array<wchar_t,32768> path{};
        const auto length=GetModuleFileNameW(nullptr,path.data(),UINT(path.size()));
        if (!length || length>=path.size()) throw std::runtime_error("Cannot identify native collectible image");
        const OutlastNativeImage image(std::wstring(path.data(),length));
        for (const auto rva:{0xd8d920u,0xce9680u,0xd8ff70u}) {
            const auto body=image.body(rva,32);
            if (std::memcmp(reinterpret_cast<const void*>(base_+rva),body.data(),body.size())!=0)
                throw std::runtime_error("Native collectible observer entry was already modified");
        }
        trace_.open(evidence_/L"native-collectible-observer.jsonl",std::ios::binary);
        trace_.precision(9);
        trace_<<"{\"kind\":\"observer\",\"fixture_version\":1,\"module_base\":"<<base_
            <<",\"dispatch_rva\":14211360,\"notify_rva\":13538944,\"collect_rva\":14221168"
              ",\"writes_native_state\":false,\"original_calls_per_hook\":1}";flush();
        hooks_.add(reinterpret_cast<Dispatch>(base_+0xd8d920),&dispatch,original_dispatch_);
        hooks_.add(reinterpret_cast<Notify>(base_+0xce9680),&notify,original_notify_);
        hooks_.add(reinterpret_cast<Collect>(base_+0xd8ff70),&collect,original_collect_);
        instance_=this;hooks_.enable_pending();
    }
    Snapshot sample(UINT64 world) {
        world_.store(world);
        std::lock_guard<std::mutex> lock(mutex_);return latest_;
    }
    bool collected() const {return collected_.load();}
    UINT64 notified_at() const {return notify_world_.load();}
};

}
