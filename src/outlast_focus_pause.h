#pragma once
#include <optional>
#include <stdexcept>

namespace olvr {
struct FocusPauseChange {
    void* engine=nullptr;
    int requested=0,applied=0;
    bool invoke=true;
};
class OutlastFocusPause {
    std::optional<FocusPauseChange> last_;
public:
    FocusPauseChange request(void* engine,int requested,bool xr_focused) {
        if (!engine) throw std::invalid_argument("Native focus pause requires its engine");
        const bool clear_previous=last_ && last_->engine==engine && last_->applied!=0;
        last_=FocusPauseChange{engine,requested,xr_focused?0:requested,
            !xr_focused || !requested || clear_previous};
        return *last_;
    }
    std::optional<FocusPauseChange> refresh(bool xr_focused) {
        if (!last_) return {};
        const int applied=xr_focused?0:last_->requested;
        if (applied==last_->applied) return {};
        last_->applied=applied;
        last_->invoke=true;
        return last_;
    }
};
}
