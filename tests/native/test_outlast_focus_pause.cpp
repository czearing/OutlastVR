#include "outlast_focus_pause.h"
#include <iostream>

int main() {
    try {
        unsigned checks=0;
        auto require=[&](bool value) {
            ++checks;
            if (!value) throw std::runtime_error("Native/XR focus pause regression");
        };
        olvr::OutlastFocusPause policy;
        int engine=0,other=0;
        require(!policy.refresh(true) && !policy.refresh(false));
        for (const int requested:{0,1,7}) for (const bool focused:{false,true}) {
            const auto change=policy.request(&engine,requested,focused);
            require(change.engine==&engine && change.requested==requested &&
                change.applied==(focused?0:requested));
            require(!policy.refresh(focused));
            const auto switched=policy.refresh(!focused);
            require(bool(switched)==(requested!=0));
            if (switched) require(switched->engine==&engine && switched->requested==requested &&
                switched->applied==(focused?requested:0) && switched->invoke);
            require(!policy.refresh(!focused));
        }
        policy.request(&engine,1,true);
        require(!policy.request(&engine,1,true).invoke);
        for (unsigned tick=0;tick<10000;++tick) require(!policy.refresh(true));
        const auto lost=policy.refresh(false);
        require(lost && lost->applied==1 && lost->invoke);
        const auto regained=policy.refresh(true);
        require(regained && regained->applied==0 && regained->invoke);
        require(policy.request(&other,0,true).invoke);
        require(!policy.refresh(false) && !policy.refresh(true));
        const auto background=policy.request(&other,1,false);
        require(background.engine==&other && background.applied==1);
        require(policy.refresh(true)->engine==&other);
        require(!policy.request(&engine,1,true).invoke);
        bool rejected=false;
        try {policy.request(nullptr,1,true);}
        catch (const std::invalid_argument&) {rejected=true;}
        require(rejected && !policy.refresh(true));
        std::cout<<"PASS: focus_pause_checks="<<checks
            <<"; desktop loss ignored only with current XR focus; XR transitions restore native request"
            <<"; no unsolicited engine call or repeated unpause; foreground request preserved\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
