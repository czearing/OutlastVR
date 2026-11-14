#pragma once
#include <cstring>
#include "d3d9_resource.h"
#include "MinHook.h"
#include <iostream>
#include <vector>
#include <mutex>

namespace olvr {

class NativeHookSet {
    mutable std::mutex mutex_;
    struct Hook {void* target;void* replacement;void* trampoline;bool enabled;};
    std::vector<Hook> hooks_;
    bool initialized_=false;
    static void check(MH_STATUS status,const char* operation) {
        if (status!=MH_OK)
            throw std::runtime_error(std::string(operation)+": "+MH_StatusToString(status));
    }
public:
    NativeHookSet() {
        check(MH_Initialize(),"Initialize genuine native hooks");initialized_=true;
    }
    NativeHookSet(const NativeHookSet&)=delete;
    NativeHookSet& operator=(const NativeHookSet&)=delete;
    template<class Function>
    void add(Function target,Function replacement,Function& original) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target || !replacement) throw std::invalid_argument("Native hook entry cannot be null");
        const auto address=reinterpret_cast<void*>(target);
        for (const auto& hook:hooks_) {
            if (hook.target!=address) continue;
            if (hook.replacement!=reinterpret_cast<void*>(replacement))
                throw std::logic_error("Two different replacements target the same native entry");
            original=reinterpret_cast<Function>(hook.trampoline);
            return;
        }
        check(MH_CreateHook(address,reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(&original)),
            "Create exact native hook");
        hooks_.push_back({address,reinterpret_cast<void*>(replacement),reinterpret_cast<void*>(original),false});
    }
    template<class Function> Function original_for(void* object,std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        void** table=nullptr;std::memcpy(&table,object,sizeof(table));
        for (const auto& hook:hooks_)
            if (hook.target==table[index]) return reinterpret_cast<Function>(hook.trampoline);
        throw std::logic_error("COM original entry was not registered by the runtime");
    }
    void enable_pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& hook:hooks_)
            if (!hook.enabled) check(MH_QueueEnableHook(hook.target),"Queue owned native hook");
        check(MH_ApplyQueued(),"Activate owned native hooks");
        for (auto& hook:hooks_) hook.enabled=true;
    }
    ~NativeHookSet() {
        std::lock_guard<std::mutex> lock(mutex_);
        bool failed=false;
        for (auto it=hooks_.rbegin();it!=hooks_.rend();++it) {
            const auto status=MH_RemoveHook(it->target);
            if (status!=MH_OK) {
                std::cerr<<"Native hook removal failed: "<<MH_StatusToString(status)<<'\n';failed=true;
            }
        }
        if (initialized_ && MH_Uninitialize()!=MH_OK) {
            std::cerr<<"Native hook context removal failed\n";failed=true;
        }
        if (failed) std::terminate();
    }
};

}
