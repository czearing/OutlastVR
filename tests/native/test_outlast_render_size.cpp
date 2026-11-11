#include "outlast_render_size.h"
#include <iostream>

int main() {
    try {
        unsigned checks=0;
        auto require=[&](bool value) {++checks;if (!value) throw std::runtime_error("Native render-size routing failed");};
        UINT x=3840,y=2160;
        unsigned char game_viewport[0x100]{},other[0x100]{};
        for (UINT width:{256u,1024u,1832u,2016u,2448u,4096u,8192u})
            for (UINT height:{256u,1024u,1920u,2240u,4096u,8192u}) {
                require(olvr::outlast_render_size(game_viewport+0x60,game_viewport,x,y,width,height) &&
                    x==width && y==height);
                require(!olvr::outlast_render_size(other,game_viewport,x,y,1,1) && x==width && y==height);
                require(!olvr::outlast_render_size(nullptr,game_viewport,x,y,1,1) && x==width && y==height);
                require(!olvr::outlast_render_size(game_viewport+0x60,nullptr,x,y,1,1) && x==width && y==height);
            }
        for (unsigned fault=0;fault<5;++fault) {
            const UINT before_x=x,before_y=y;
            bool rejected=false;
            try {
                olvr::outlast_render_size(game_viewport+0x60,game_viewport,x,y,
                    fault==0?0: fault==2?8193: fault==4?UINT(-1):1024,
                    fault==1?0: fault==3?8193:1024);
            } catch (const std::runtime_error&) {rejected=true;}
            require(rejected && x==before_x && y==before_y);
        }
        std::cout<<"PASS: native_render_size_checks="<<checks
            <<"; headset_extent_routes_to_engine; unrelated_viewports_preserved; invalid_extents_rejected\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
