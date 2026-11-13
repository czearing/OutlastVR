#include "stereo_reconstruction.h"
#include <iostream>

using namespace olvr;

int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-scissor";
        if (argc>2 || (argc==2 && !fault)) throw std::invalid_argument("Unknown world-scissor control");
        const RECT viewport{8,4,136,100},source{50,35,90,65};
        const TrackingFrame frame{{10,-20,30},{1,0,0},{0,1,0},{0,0,-1},1,{-10,20,-30}};
        const EyeFov mono{-.35f,.4f,.4f,-.35f};
        std::size_t checks=0;
        const std::array<std::array<RECT,2>,7> clips{{
            {{{2,1,6,5},{2,1,6,5}}},
            {{{0,0,0,0},{1,2,4,6}}},
            {{{2,1,6,5},{0,3,3,8}}},
            {{{-10,-10,20,20},{-10,-10,20,20}}},
            {{{-5,1,-1,7},{9,0,12,8}}},
            {{{0,0,0,0},{0,0,0,0}}},
            {{{1,2,4,6},{0,0,0,0}}}
        }};
        for (const auto& clip:clips) {
            const auto actual=stereo_atlas_scissor(clip,8,8);
            RECT expected{};
            bool visible=false;
            for (LONG y=0;y<8;++y) for (LONG x=0;x<16;++x) {
                const auto& eye=clip[x/8];
                const auto local=x%8;
                if (local<eye.left || local>=eye.right || y<eye.top || y>=eye.bottom) continue;
                if (!visible) expected={x,y,x+1,y+1};
                else {
                    expected.left=std::min(expected.left,x);expected.top=std::min(expected.top,y);
                    expected.right=std::max(expected.right,x+1);expected.bottom=std::max(expected.bottom,y+1);
                }
                visible=true;++checks;
            }
            if (std::memcmp(&actual,&expected,sizeof(actual)))
                throw std::runtime_error("Hardware atlas scissor differs from independent per-eye pixel coverage");
        }
        const auto rejects_hardware=[](auto operation) {
            bool rejected=false;
            try {operation();} catch (const std::invalid_argument&) {rejected=true;}
            if (!rejected) throw std::runtime_error("Invalid hardware atlas scissor was accepted");
        };
        rejects_hardware([&] {stereo_atlas_scissor(clips[0],0,8);});
        rejects_hardware([&] {stereo_atlas_scissor(clips[0],8,0);});
        rejects_hardware([&] {stereo_atlas_scissor(clips[0],unsigned(std::numeric_limits<LONG>::max()),8);});
        rejects_hardware([&] {stereo_atlas_scissor(clips[0],8,std::numeric_limits<unsigned>::max());});
        rejects_hardware([&] {stereo_atlas_scissor({RECT{4,1,2,5},RECT{}},8,8);});
        for (const float native_depth:{.999f,1.f,1.02f}) for (const float near_plane:{.125f,2.f})
            for (const float yaw:{-.7f,-.35f,0.f,.4f,.7f}) {
                StereoReconstructionContext context{};
                context.original_camera=frame;
                const auto original=eye_constants(frame,{{0,0,0},{0,0,0,1}},mono,near_plane,10000,native_depth);
                std::copy_n(original.begin(),16,context.original_projection.begin());
                std::array<EyePose,2> poses{};
                std::array<EyeFov,2> fovs{};
                std::array<Matrix4,2> transforms{};
                for (unsigned eye=0;eye<2;++eye) {
                    poses[eye]={{eye?.032f:-.032f,.01f,.02f},
                        {0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)}};
                    fovs[eye]=eye?EyeFov{-.65f,.9f,.7f,-.75f}:EyeFov{-.85f,.7f,.75f,-.7f};
                    const auto constants=eye_constants(frame,poses[eye],fovs[eye],near_plane,10000,native_depth);
                    transforms[eye]=screen_input_delta(context,constants);
                }
                for (const RECT bounds:{source,RECT{8,35,90,65},RECT{50,35,136,65},
                    RECT{50,4,90,65},RECT{50,35,90,100},RECT{8,35,136,65},RECT{50,4,90,100}}) {
                const auto actual=fault?std::array<RECT,2>{bounds,bounds}:
                    stereo_world_scissors(context,transforms,bounds,viewport);
                const auto first_x=bounds.left==viewport.left?viewport.left-200:bounds.left+1;
                const auto last_x=bounds.right==viewport.right?viewport.right+200:bounds.right;
                const auto first_y=bounds.top==viewport.top?viewport.top-200:bounds.top+1;
                const auto last_y=bounds.bottom==viewport.bottom?viewport.bottom+200:bounds.bottom;
                for (int sx=first_x;sx<last_x;sx+=4)
                    for (int sy=first_y;sy<last_y;sy+=4)
                        for (const float distance:{near_plane,2.f,20.f,10000.f}) {
                            const float nx=float(sx-viewport.left)*2/(viewport.right-viewport.left)-1;
                            const float ny=1-float(sy-viewport.top)*2/(viewport.bottom-viewport.top);
                            const float rx=((std::tan(mono.right)-std::tan(mono.left))*nx+
                                std::tan(mono.right)+std::tan(mono.left))*.5f;
                            const float ry=((std::tan(mono.up)-std::tan(mono.down))*ny+
                                std::tan(mono.up)+std::tan(mono.down))*.5f;
                            const auto point=add(frame.origin,{rx*distance,ry*distance,distance});
                            for (unsigned eye=0;eye<2;++eye) {
                                const auto& pose=poses[eye];const auto& fov=fovs[eye];
                                const auto relative=add(point,scale(add(frame.origin,map_tracking(frame,pose.position)),-1));
                                const auto right=map_tracking(frame,rotate(pose.orientation,{1,0,0}));
                                const auto up=map_tracking(frame,rotate(pose.orientation,{0,1,0}));
                                const auto forward=map_tracking(frame,rotate(pose.orientation,{0,0,-1}));
                                const auto d=dot(relative,forward);
                                if (d<=0) continue;
                                const auto x=(2*dot(relative,right)/d-std::tan(fov.right)-std::tan(fov.left))/
                                    (std::tan(fov.right)-std::tan(fov.left));
                                const auto y=(2*dot(relative,up)/d-std::tan(fov.up)-std::tan(fov.down))/
                                    (std::tan(fov.up)-std::tan(fov.down));
                                const auto px=(x+1)*64+viewport.left,py=(1-y)*48+viewport.top;
                                if (px<viewport.left || px>=viewport.right || py<viewport.top || py>=viewport.bottom) continue;
                                if (px<actual[eye].left || px>=actual[eye].right ||
                                    py<actual[eye].top || py>=actual[eye].bottom)
                                    throw std::runtime_error("Mono-derived light scissor rejected an independently visible eye point");
                                ++checks;
                            }
                        }
                }
            }
        StereoReconstructionContext context{};
        context.original_camera=frame;
        const auto original=eye_constants(frame,{{0,0,0},{0,0,0,1}},mono,.125f,1000);
        std::copy_n(original.begin(),16,context.original_projection.begin());
        std::array<Matrix4,2> identity{identity_matrix(),identity_matrix()};
        const auto narrow=stereo_world_scissors(context,identity,source,viewport)[0];
        if (narrow.left<=viewport.left || narrow.right>=viewport.right ||
            narrow.top<=viewport.top || narrow.bottom>=viewport.bottom)
            throw std::runtime_error("World scissor unnecessarily disabled interior same-camera clipping");
        auto translated=identity;
        translated[0][3]=-.02f;translated[1][3]=.02f;
        const auto separate=stereo_world_scissors(context,translated,source,viewport);
        const LONG united_width=std::max(separate[0].right,separate[1].right)-
            std::min(separate[0].left,separate[1].left);
        if (separate[0].left==separate[1].left ||
            separate[0].right-separate[0].left+separate[1].right-separate[1].left>=2*united_width)
            throw std::runtime_error("Eye-specific scissor bounds retained their unnecessary union");
        const auto edge=stereo_world_scissors(context,identity,{8,35,90,65},viewport)[0];
        if (edge.left!=viewport.left || edge.top<=viewport.top || edge.bottom>=viewport.bottom ||
            edge.right>=viewport.right) throw std::runtime_error("Open light-bound side discarded unrelated clipping axes");
        identity[1][10]=-1;
        const auto behind=stereo_world_scissors(context,identity,source,viewport);
        if (behind[0].left>=behind[0].right || behind[0].top>=behind[0].bottom ||
            behind[1].left!=behind[1].right || behind[1].top!=behind[1].bottom)
            throw std::runtime_error("Forward eye was rejected when the other eye faced away");
        identity[0][10]=-1;
        const auto absent=stereo_world_scissors(context,identity,source,viewport);
        if (absent[0].left!=absent[0].right || absent[0].top!=absent[0].bottom ||
            absent[1].left!=absent[1].right || absent[1].top!=absent[1].bottom)
            throw std::runtime_error("Both backward eye cones were incorrectly visible");
        identity={identity_matrix(),identity_matrix()};
        identity[0][11]=identity[1][11]=10;
        const auto inside=stereo_world_scissors(context,identity,source,viewport)[0];
        if (std::memcmp(&inside,&viewport,sizeof(inside)))
            throw std::runtime_error("An eye inside the native light cone lost visible directions");
        const auto empty=stereo_world_scissors(context,identity,{50,35,50,35},viewport)[0];
        if (empty.left!=empty.right || empty.top!=empty.bottom) throw std::runtime_error("Empty caller clip became visible");
        bool refused=false;
        try {stereo_world_scissors(context,identity,{90,35,50,65},viewport);}
        catch (const std::invalid_argument&) {refused=true;}
        if (!refused) throw std::runtime_error("Invalid world scissor accepted");
        std::cout<<"PASS: conservative world light scissors; physical_eye_points="<<checks
            <<"; asymmetric eyes; yaw; IPD; offset viewport; native depth conventions; near/infinite endpoints;"
            <<" bounded interior retained; truncated bounds and eye-plane crossings conservative; empty clip preserved;"
            <<" hardware atlas union matches independent pixel coverage\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
