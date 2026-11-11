#include "outlast_native_view.h"
#include <iostream>

using namespace olvr;

namespace {
unsigned checks=0;
void close(float actual,float expected,const char* message,float tolerance=.0002f) {
    ++checks;
    if (!std::isfinite(actual) || std::abs(actual-expected)>tolerance)
        throw std::runtime_error(message);
}
void close(Vec3 actual,Vec3 expected,const char* message) {
    close(actual.x,expected.x,message);close(actual.y,expected.y,message);close(actual.z,expected.z,message);
}
template<class F> void refuses(F operation) {
    bool refused=false;
    try { operation(); } catch (const std::invalid_argument&) { refused=true; }
    ++checks;
    if (!refused) throw std::runtime_error("Invalid native view was accepted");
}
}

int main(int argc,char** argv) {
    try {
        const bool transpose=argc==2 && std::string(argv[1])=="--fault-transpose";
        const bool tilted=argc==2 && std::string(argv[1])=="--fault-upright";
        if (argc>2 || (argc==2 && !transpose && !tilted)) throw std::invalid_argument("Unknown native camera control");
        struct Basis { Vec3 right,up,forward; };
        const Basis bases[]{
            {{1,0,0},{0,1,0},{0,0,1}},
            {{0,1,0},{0,0,1},{1,0,0}},
            {{0,-1,0},{0,0,-1},{1,0,0}},
            {{0,0,1},{0,1,0},{-1,0,0}},
            {{.6f,.8f,0},{-.8f,.6f,0},{0,0,-1}}
        };
        for (const bool translated:{false,true}) {
            std::array<float,16> matrix{};
            matrix[0]=matrix[5]=matrix[10]=1.f/524288;
            matrix[15]=1;
            if (translated) for (const auto index:{3u,7u,11u}) matrix[index]=.5f;
            ++checks;
            if (!outlast_orthographic_view(matrix))
                throw std::runtime_error("Actual startup orthographic view was mistaken for a perspective camera");
            matrix[0]=std::numeric_limits<float>::quiet_NaN();
            refuses([&]{outlast_orthographic_view(matrix);});
        }
        {
            std::array<unsigned char,0x12c0> view{};
            const Vec3 origin{-16422.591797f,-820.887451f,-3929.191650f};
            const Vec3 pretranslation{16422.591797f,820.887451f,3929.191650f};
            const std::array<float,16> uploaded{
                .9f,0,0,0,0,-.011045f,-.998976f,-.999976f,
                0,1.599962f,-.006896f,-.006903f,-.003516f,-.000391f,-1.997878f,.000122f};
            std::memcpy(view.data()+0x200,uploaded.data(),sizeof(uploaded));
            std::memcpy(view.data()+0x3a0,&origin,sizeof(origin));
            std::memcpy(view.data()+0x280,&pretranslation,sizeof(pretranslation));
            const auto camera=outlast_view_camera(view.data(),view.size(),100);
            close(camera.tracking.origin,origin,"Continue camera origin changed");
            close(camera.near_plane,2,"Continue camera near plane changed",.001f);
            for (unsigned index:{12u,13u,15u}) {
                auto invalid=view;
                auto changed=uploaded;changed[index]+=1.f;
                std::memcpy(invalid.data()+0x200,changed.data(),sizeof(changed));
                refuses([&]{outlast_view_camera(invalid.data(),invalid.size(),100);});
            }
        }
        for (const auto basis : bases) for (const float depth : {1.f,1.02f,.999f})
            for (const float normalization : {1.f,3.f}) {
                std::array<unsigned char,0x12c0> view{};
                const Vec3 origin{170,-220,310},pretranslation{-160,200,-290};
                const Vec3 translated{10,-20,20};
                std::memcpy(view.data()+0x3a0,&origin,sizeof(origin));
                std::memcpy(view.data()+0x280,&pretranslation,sizeof(pretranslation));
                const float sx=1.3f,sy=1.7f,bx=.13f,by=-.07f,near_plane=2;
                const Vec3 axes[]{
                    {basis.right.x*sx+basis.forward.x*bx,basis.right.y*sx+basis.forward.y*bx,
                     basis.right.z*sx+basis.forward.z*bx},
                    {basis.up.x*sy+basis.forward.x*by,basis.up.y*sy+basis.forward.y*by,
                     basis.up.z*sy+basis.forward.z*by},
                    {basis.forward.x*depth,basis.forward.y*depth,basis.forward.z*depth},
                    basis.forward
                };
                std::array<float,16> matrix{};
                for (unsigned row=0;row<4;++row) {
                    matrix[row*4]=axes[row].x*normalization;
                    matrix[row*4+1]=axes[row].y*normalization;
                    matrix[row*4+2]=axes[row].z*normalization;
                    matrix[row*4+3]=-(axes[row].x*translated.x+axes[row].y*translated.y+
                                       axes[row].z*translated.z)*normalization;
                }
                matrix[11]-=near_plane*depth*normalization;
                if (!transpose)
                    for (unsigned row=0;row<4;++row) for(unsigned column=row+1;column<4;++column)
                        std::swap(matrix[row*4+column],matrix[column*4+row]);
                std::memcpy(view.data()+0x200,matrix.data(),sizeof(matrix));
                ++checks;
                if (outlast_orthographic_view(outlast_view_matrix(view.data(),view.size())))
                    throw std::runtime_error("Perspective camera was mistaken for startup UI");
                const auto camera=outlast_view_camera(view.data(),view.size(),100);
                close(camera.tracking.right,basis.right,"Native camera right axis mismatch");
                close(camera.tracking.up,basis.up,"Native camera up axis mismatch");
                close(camera.tracking.backward,{-basis.forward.x,-basis.forward.y,-basis.forward.z},
                      "Native camera backward axis mismatch");
                close(camera.tracking.origin,origin,"Native camera world origin mismatch");
                close(camera.tracking.pretranslation,pretranslation,"Native pretranslation mismatch");
                close(camera.near_plane,2,"Native near clipping plane mismatch");
                if (depth>1) close(camera.far_plane,102,"Native far clipping plane mismatch",.001f);
                close(camera.depth_scale,depth,"Native UE3 depth precision was not preserved");
                close(camera.original_fov.left,std::atan((-1-bx)/sx),"Native left FOV mismatch");
                close(camera.original_fov.right,std::atan((1-bx)/sx),"Native right FOV mismatch");
                close(camera.original_fov.up,std::atan((1-by)/sy),"Native up FOV mismatch");
                close(camera.original_fov.down,std::atan((-1-by)/sy),"Native down FOV mismatch");
                for (unsigned fault=0;fault<7;++fault) {
                    auto invalid=view;
                    auto changed=matrix;
                    for (unsigned row=0;row<4;++row) for(unsigned column=row+1;column<4;++column)
                        std::swap(changed[row*4+column],changed[column*4+row]);
                    if (fault==0) changed[3]+=20*normalization;
                    if (fault==1) changed[15]+=20*normalization;
                    if (fault==2) changed[11]=1000;
                    if (fault==3) changed[12]=changed[13]=changed[14]=0;
                    if (fault==4) changed[0]=std::numeric_limits<float>::quiet_NaN();
                    if (fault==5) {
                        changed[8]+=basis.right.x*normalization;
                        changed[9]+=basis.right.y*normalization;
                        changed[10]+=basis.right.z*normalization;
                    }
                    for (unsigned row=0;row<4;++row) for(unsigned column=row+1;column<4;++column)
                        std::swap(changed[row*4+column],changed[column*4+row]);
                    std::memcpy(invalid.data()+0x200,changed.data(),sizeof(changed));
                    refuses([&] { outlast_view_camera(invalid.data(),fault==6?0x280:invalid.size(),100); });
                }
            }
        unsigned upright_checks=0;
        for (const bool z_up:{false,true}) for (const float sign:{-1.f,1.f})
            for (const float yaw:{-2.7f,0.f,1.8f}) for (const float pitch:{-.8f,0.f,.8f})
                for (const float roll:{-2.8f,0.f,.5f}) {
                    const Vec3 up=z_up?Vec3{0,0,1}:Vec3{0,1,0};
                    const Vec3 forward=z_up?Vec3{std::cos(yaw),std::sin(yaw),0}:
                        Vec3{std::sin(yaw),0,std::cos(yaw)};
                    const Vec3 right=z_up?Vec3{-std::sin(yaw)*sign,std::cos(yaw)*sign,0}:
                        Vec3{std::cos(yaw)*sign,0,-std::sin(yaw)*sign};
                    const Quaternion qp{right.x*std::sin(pitch/2),right.y*std::sin(pitch/2),
                        right.z*std::sin(pitch/2),std::cos(pitch/2)};
                    const auto axis=rotate(qp,forward);
                    const Quaternion qr{axis.x*std::sin(roll/2),axis.y*std::sin(roll/2),
                        axis.z*std::sin(roll/2),std::cos(roll/2)};
                    auto tilt=[&](Vec3 value){return rotate(qr,rotate(qp,value));};
                    const TrackingFrame native{{-72,18,165},tilt(right),tilt(up),
                        tilt(scale(forward,-1)),100,{70,-20,-160}};
                    const auto upright=tilted?native:outlast_upright_tracking(native,up);
                    close(upright.right,right,"Animated camera tilt entered upright tracking");
                    close(upright.up,up,"Animated camera tilt entered upright tracking");
                    close(upright.backward,scale(forward,-1),"Native heading changed during camera leveling");
                    close(upright.origin,native.origin,"Native stance/camera position changed during leveling");
                    close(upright.pretranslation,native.pretranslation,"Camera leveling changed pretranslation");
                    close(upright.units_per_meter,100,"Camera leveling changed world scale");
                    ++upright_checks;
                }
        {
            const TrackingFrame vertical{{1,2,3},{0,1,0},{-1,0,0},{0,0,-1},100,{0,0,0}};
            const auto upright=outlast_upright_tracking(vertical,{0,0,1});
            close(upright.right,{0,1,0},"Vertical native camera lost its horizontal right axis");
            close(upright.up,{0,0,1},"Vertical native camera tilted gravity");
            close(upright.backward,{-1,0,0},"Vertical native camera lost its recoverable heading");
            refuses([&]{outlast_upright_tracking(vertical,{0,0,0});});
            auto invalid=vertical;invalid.right.y=2;invalid.up.x=-.5f;
            refuses([&]{outlast_upright_tracking(invalid,{0,0,1});});
            invalid=vertical;invalid.backward.z=std::numeric_limits<float>::quiet_NaN();
            refuses([&]{outlast_upright_tracking(invalid,{0,0,1});});
        }
        const float h=std::sqrt(.5f);
        const std::array<EyePose,2> start{{{{2,1.6f,3.032f},{0,h,0,h}},
                                          {{2,1.6f,2.968f},{0,h,0,h}}}};
        OutlastRecenter recenter;
        recenter.recenter(start);
        for (unsigned eye=0;eye<2;++eye) {
            const auto relative=recenter.relative(start[eye]);
            close(relative.position,{eye?.032f:-.032f,0,0},"Recenter lost physical IPD");
            close(relative.orientation.y,0,"Recenter did not cancel initial yaw");
            close(relative.orientation.w,1,"Recenter quaternion is invalid");
        }
        auto moved=start[0];
        moved.position.x-=.2f;
        moved.position.y+=.1f;
        const auto relative=recenter.relative(moved);
        close(relative.position,{-.032f,.1f,-.2f},"Recenter lost physical head translation");
        const Quaternion pitch{std::sin(.2f),0,0,std::cos(.2f)};
        moved.orientation=multiply(start[0].orientation,pitch);
        const auto pitched=recenter.relative(moved).orientation;
        close(pitched.x,pitch.x,"Yaw recenter incorrectly cancelled pitch");
        close(pitched.w,pitch.w,"Recenter lost head orientation");
        auto sign_changed=start;
        sign_changed[1].orientation={0,-h,0,-h};
        recenter.recenter(sign_changed);
        close(recenter.relative(start[0]).position,{-.032f,0,0},"Quaternion double cover changed recenter");
        auto invalid=start;invalid[1].orientation.w=2;
        refuses([&]{recenter.recenter(invalid);});
        close(recenter.relative(start[0]).position,{-.032f,0,0},"Failed recenter changed calibration");
        close(recenter.standing_height(),1.6f,"Floor standing-height reference lost");
        auto squatting=start;
        for (auto& eye:squatting) {eye.position.y-=.5f;eye.position.x+=1;}
        recenter.recenter(squatting,true);
        close(recenter.relative(squatting[0]).position,{-.032f,-.5f,0},
            "System recenter while squatting erased physical crouch height");
        close(recenter.standing_height(),1.6f,"System recenter recalibrated a crouched standing height");
        recenter.change_reference({{0,.2f,0},{0,0,0,1}});
        for (auto& eye:squatting) eye.position.y-=.2f;
        recenter.recenter(squatting,true);
        close(recenter.relative(squatting[0]).position,{-.032f,-.5f,0},
            "Runtime vertical origin change moved the physical crouch baseline");
        close(recenter.standing_height(),1.4f,"Runtime origin height transform lost measured floor position");
        refuses([&]{recenter.change_reference({{0,0,0},{0,0,0,2}});});
        close(recenter.standing_height(),1.4f,"Rejected origin transform modified calibration");
        for (float standing:{1.1f,1.6f,1.9f,2.1f}) {
            close(outlast_floor_eye_height(165,165,standing,100,false),standing*100,
                "Measured floor height was added on top of the authored eye height");
            close(outlast_floor_eye_height(95,165,standing,100,true),standing*100,
                "Physical crouch lowered the measured standing anchor twice");
            close(outlast_floor_eye_height(95,165,standing,100,false),standing*100-70,
                "Floor alignment removed native manual crouching");
            close(outlast_floor_eye_height(95,165,standing,100,false,true),standing*100,
                "Jump animation pulled the measured headset height inside the character");
        }
        refuses([&]{outlast_floor_eye_height(165,165,-1,100,false);});
        refuses([&]{outlast_floor_eye_height(165,165,0,100,false);});
        for (const float height:{-.1f,0.f,1.7f})
            for (const bool position:{false,true})
                for (const bool orientation:{false,true})
                    close(outlast_floor_reference_available(height,position,orientation),
                        height>0 && position && orientation,
                        "Cached/zero STAGE pose was accepted as measured standing height");
        refuses([&]{outlast_floor_reference_available(std::numeric_limits<float>::quiet_NaN(),true,true);});
        std::cout<<"PASS: "<<checks<<" independent native-view and recenter checks; exact uploaded matrix reconstruction, "
            "mirrored/rotated engine bases, pretranslation, finite/infinite depth, invalid views, yaw-only origin and preserved 6DOF; "
            <<"upright_camera_cases="<<upright_checks<<"; native_stance_height_preserved; "
            <<"system_recenter_preserves_physical_height; runtime_origin_transform; measured_floor_height_once; "
            <<"zero_or_untracked_floor_deferred\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
