#include "native_probe_progress.h"
#include "native_draw_selection.h"
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    try {
        for (unsigned basic=0;basic<256;++basic)
            for (unsigned special=0;special<256;++special)
                for (const bool tracked:{false,true})
                    if (olvr::native_draw_jump_ready(std::uint8_t(basic),std::uint8_t(special),tracked)!=
                        (tracked && basic==2 && special==3))
                        throw std::runtime_error("Airborne GPU selector admitted an unrelated or untracked native pose");
        for (unsigned world=0;world<=900;++world)
            for (const bool startup:{false,true}) {
                const bool expected=world==0?startup:
                    (world>=180 && world<=184) || (world>=360 && world<=364) ||
                    (world>=540 && world<=544) || (world>=800 && world<=804);
                if (olvr::native_probe_jump_pressed(world,startup)!=expected)
                    throw std::runtime_error("Jump schedule overwrote startup confirmation or changed gameplay pulses");
            }
        olvr::NativeDrawSelection fixed{885,false},contact{0,true},disabled{};
        if (fixed.selects(884,true) || !fixed.selects(885,false) || fixed.selects(886,true) ||
            contact.selects(0,true) || contact.selects(885,false) ||
            !contact.selects(930,true) || !contact.selects(930,false) || contact.selects(931,true) ||
            disabled.selects(885,true))
            throw std::runtime_error("Native GPU frame selection lost fixed/disabled behavior or contact-frame latching");
        olvr::NativeProbeProgress missed;
        std::vector<unsigned> attempts;
        for (unsigned frame=1;frame<=2400;++frame)
            if (missed.observe(frame,true)) attempts.push_back(frame);
        if (attempts!=std::vector<unsigned>{150,300,420,540,660} ||
            missed.gameplay_frames()!=0 || missed.complete() || !missed.expired(2400))
            throw std::runtime_error("Unconfirmed Enter was accepted or retry count/delay/deadline changed");
        olvr::NativeProbeProgress delayed;
        attempts.clear();
        for (unsigned frame=1;frame<=1399;++frame)
            if (delayed.observe(frame,frame<500)) attempts.push_back(frame);
        if (attempts!=std::vector<unsigned>{150,300,420} || !delayed.complete() ||
            delayed.gameplay_frames()!=900 || delayed.continue_attempts()!=2)
            throw std::runtime_error("Delayed Continue did not recover or stop input after confirmed gameplay");
        for (unsigned frame=1400;frame<=2500;++frame)
            if (delayed.observe(frame,true)) throw std::runtime_error("Reopened gameplay menu received automated Enter");
        if (delayed.startup_only())
            throw std::runtime_error("Reopened gameplay menu reactivated startup-only pad");
        if (delayed.gameplay_frames()!=900 || delayed.expired(2500))
            throw std::runtime_error("Menu frames were falsely counted as gameplay or completed capture expired");
        olvr::NativeProbeProgress normal;
        if (!normal.startup_only()) throw std::runtime_error("Startup pad was not available before gameplay");
        unsigned inputs=0;
        for (unsigned frame=1;frame<=1050;++frame) inputs+=normal.observe(frame,frame<=150);
        if (inputs!=1 || !normal.complete() || normal.continue_attempts()!=0)
            throw std::runtime_error("Ordinary Continue required redundant menu input");
        if (normal.startup_only()) throw std::runtime_error("Startup pad remained active after gameplay");
        std::cout<<"PASS: native probe progress; observed gameplay required; bounded delayed retries;"
            <<" missed-input deadline retained; no gameplay or reopened-menu input; 900 world frames required;"
            <<" fixed_and_contact_driven_GPU_frame_selection; jump_fixture_preserves_startup_confirmation\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
