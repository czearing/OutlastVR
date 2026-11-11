#pragma once
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <cstddef>

namespace olvr {

class OutlastRendererOwnership {
    struct MainContext {OutlastRendererOwnership* owner;void* viewport;bool panel;};
    struct FamilyContext {OutlastRendererOwnership* owner;const void* family;void* viewport;bool panel;};
    struct DispatchContext {OutlastRendererOwnership* owner;void* renderer;void* viewport;bool panel;};
    struct Presentation {void* viewport;bool panel;};
    static inline thread_local MainContext* main_=nullptr;
    static inline thread_local FamilyContext* family_=nullptr;
    static inline thread_local DispatchContext* dispatch_=nullptr;
    std::mutex mutex_;
    std::unordered_map<void*,Presentation> renderers_;
public:
    class MainScope {
        MainContext context_;
        MainContext* previous_;
    public:
        MainScope(OutlastRendererOwnership& owner,void* viewport,bool panel=false)
            :context_{&owner,viewport,panel},previous_(main_) {
            if (!viewport) throw std::invalid_argument("Main viewport identity cannot be null");
            main_=&context_;
        }
        MainScope(const MainScope&)=delete;
        MainScope& operator=(const MainScope&)=delete;
        ~MainScope() {
            if (main_!=&context_) std::terminate();
            main_=previous_;
        }
    };
    class FamilyScope {
        FamilyContext context_;
        FamilyContext* previous_;
    public:
        FamilyScope(OutlastRendererOwnership& owner,const void* family)
            :context_{&owner,family,main_ && main_->owner==&owner?main_->viewport:nullptr,
                main_ && main_->owner==&owner && main_->panel},
             previous_(family_) {
            if (!family) throw std::invalid_argument("Enqueued family identity cannot be null");
            family_=&context_;
        }
        FamilyScope(const FamilyScope&)=delete;
        FamilyScope& operator=(const FamilyScope&)=delete;
        ~FamilyScope() {
            if (family_!=&context_) std::terminate();
            family_=previous_;
        }
    };
    bool constructed(void* renderer,const void* family) {
        if (!family_ || family_->owner!=this || !family_->viewport || family_->family!=family)
            return false;
        if (!renderer) throw std::invalid_argument("Owned family returned a null renderer");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!renderers_.emplace(renderer,Presentation{family_->viewport,family_->panel}).second)
            throw std::logic_error("Renderer was constructed twice without native dispatch/destruction");
        return true;
    }
    class DispatchScope {
        DispatchContext context_;
        DispatchContext* previous_;
    public:
        DispatchScope(OutlastRendererOwnership& owner,void* renderer)
            :context_{&owner,renderer,nullptr,false},previous_(dispatch_) {
            if (!renderer) throw std::invalid_argument("Native dispatcher renderer cannot be null");
            {
                std::lock_guard<std::mutex> lock(owner.mutex_);
                const auto found=owner.renderers_.find(renderer);
                if (found!=owner.renderers_.end()) {
                    context_.viewport=found->second.viewport;
                    context_.panel=found->second.panel;
                    // The original dispatcher frees the renderer before returning.
                    owner.renderers_.erase(found);
                }
            }
            dispatch_=&context_;
        }
        DispatchScope(const DispatchScope&)=delete;
        DispatchScope& operator=(const DispatchScope&)=delete;
        ~DispatchScope() {
            if (dispatch_!=&context_) std::terminate();
            dispatch_=previous_;
        }
        bool owned() const noexcept {return context_.viewport!=nullptr;}
    };
    bool owns_render(const void* renderer) const noexcept {
        return renderer && dispatch_ && dispatch_->owner==this &&
            dispatch_->renderer==renderer && dispatch_->viewport;
    }
    void* current_viewport(const void* renderer) const noexcept {
        return owns_render(renderer)?dispatch_->viewport:nullptr;
    }
    bool world_panel(const void* renderer) const noexcept {
        return owns_render(renderer) && dispatch_->panel;
    }
    std::size_t pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        return renderers_.size();
    }
};

}
