#include "headlocked_ui.h"
#include <iostream>

namespace {
struct Vector {double x,y,z;};
Vector plus(Vector a,Vector b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vector times(Vector a,double b){return {a.x*b,a.y*b,a.z*b};}
double product(Vector a,Vector b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vector rotation(olvr::Quaternion q,Vector v) {
    const double x=q.x,y=q.y,z=q.z,w=q.w;
    return {(1-2*y*y-2*z*z)*v.x+(2*x*y-2*z*w)*v.y+(2*x*z+2*y*w)*v.z,
        (2*x*y+2*z*w)*v.x+(1-2*x*x-2*z*z)*v.y+(2*y*z-2*x*w)*v.z,
        (2*x*z-2*y*w)*v.x+(2*y*z+2*x*w)*v.y+(1-2*x*x-2*y*y)*v.z};
}
olvr::Quaternion compose(olvr::Quaternion a,olvr::Quaternion b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
void require(bool condition,const char* text){if (!condition) throw std::runtime_error(text);}
}

int main(int argc,char** argv) {
    try {
        const std::string mode=argc==2?argv[1]:"";
        const bool hud=mode=="--hud-safe-area" || mode=="--fault-hud-edge";
        require(argc==1 || (argc==2 && (mode=="--fault-identical" || mode=="--fault-aspect" || hud)),
            "Unknown UI projection test option");
        const olvr::Quaternion heads[]{
            {0,0,0,1},{0,std::sin(.4f),0,std::cos(.4f)},
            {std::sin(.75f),0,0,std::cos(.75f)},
            {std::sin(-.75f),0,0,std::cos(-.75f)},
            {0,0,1,0},{.5f,.5f,.5f,.5f}};
        unsigned checks=0,rejected=0;
        double maximum_error=0;
        for (auto head:heads) for (float aspect:{.5f,1.f,16.f/9,21.f/9})
            for (float distance:{1.f,2.f,4.f}) for (bool canted:{false,true}) {
                const Vector origin{2,-1,3};
                const auto right=rotation(head,{1,0,0}),up=rotation(head,{0,1,0}),forward=rotation(head,{0,0,-1});
                const auto depth=hud?2.f:distance;
                const auto center=plus(origin,times(forward,depth));
                std::array<olvr::EyePose,2> eyes{};
                const std::array<olvr::EyeFov,2> fovs{{{-.95f,.55f,.72f,-.69f},{-.53f,.91f,.78f,-.65f}}};
                Vector positions[2],eye_right[2],eye_up[2],eye_forward[2];
                double extent=1e9;
                for (unsigned eye=0;eye<2;++eye) {
                    const float angle=canted?(eye?.06f:-.06f):0;
                    auto q=compose(head,{0,std::sin(angle/2),0,std::cos(angle/2)});
                    if (eye && canted) q={-q.x,-q.y,-q.z,-q.w};
                    positions[eye]=plus(origin,rotation(head,{eye?.032:-.032,eye?.001:-.001,0}));
                    eyes[eye]={{float(positions[eye].x),float(positions[eye].y),float(positions[eye].z)},q};
                    eye_right[eye]=rotation(q,{1,0,0});eye_up[eye]=rotation(q,{0,1,0});
                    eye_forward[eye]=rotation(q,{0,0,-1});
                    const auto& f=fovs[eye];
                    const Vector normals[]{
                        plus(eye_right[eye],times(eye_forward[eye],-std::tan(double(f.left)))),
                        plus(times(eye_right[eye],-1),times(eye_forward[eye],std::tan(double(f.right)))),
                        plus(eye_up[eye],times(eye_forward[eye],-std::tan(double(f.down)))),
                        plus(times(eye_up[eye],-1),times(eye_forward[eye],std::tan(double(f.up))))};
                    for (auto normal:normals) for (double x:{-1.,1.}) for (double y:{-1.,1.}) {
                        const auto coefficient=product(normal,plus(times(right,aspect*x),times(up,y)));
                        if (coefficient<0) {
                            const auto available=product(normal,plus(center,times(positions[eye],-1)));
                            require(available>0,"Independent UI plane is outside the frustum");
                            extent=std::min(extent,-available/coefficient);
                        }
                    }
                }
                if (hud) {
                    constexpr double radians=3.14159265358979323846/180;
                    extent=std::min({extent,depth*std::tan(25*radians),depth*std::tan(35*radians)/aspect});
                    require(std::atan2(extent,depth)<=25*radians+.000001 &&
                        std::atan2(aspect*extent,depth)<=35*radians+.000001,
                        "Passive notification exceeded its readable angular safe area");
                }
                auto matrices=hud && mode!="--fault-hud-edge"?
                    olvr::passive_hud_constants(eyes,fovs,aspect):
                    olvr::headlocked_ui_constants(eyes,fovs,mode=="--fault-aspect"?1:aspect,depth);
                if (mode=="--fault-identical") matrices[1]=matrices[0];
                for (unsigned eye=0;eye<2;++eye) for (double x:{-1.,0.,1.}) for (double y:{-1.,0.,1.}) {
                    const auto point=plus(plus(center,times(right,aspect*extent*x)),times(up,extent*y));
                    const auto relative=plus(point,times(positions[eye],-1));
                    const auto z=product(relative,eye_forward[eye]);
                    const auto& f=fovs[eye];
                    const double left=std::tan(double(f.left)),right_bound=std::tan(double(f.right));
                    const double bottom=std::tan(double(f.down)),top=std::tan(double(f.up));
                    const double expected[]{(2*product(relative,eye_right[eye])/z-right_bound-left)/(right_bound-left),
                        (2*product(relative,eye_up[eye])/z-top-bottom)/(top-bottom)};
                    const auto& m=matrices[eye];
                    const auto w=m[12]*x+m[13]*y+m[15];
                    require(w>0,"UI projection placed a corner behind an eye");
                    for (unsigned axis=0;axis<2;++axis) {
                        const auto actual=(m[axis*4]*x+m[axis*4+1]*y+m[axis*4+3])/w;
                        const auto error=std::abs(actual-expected[axis]);
                        maximum_error=std::max(maximum_error,error);
                        require(error<.00001,"Independent UI angular projection mismatch");
                        require(actual>=-1.00001 && actual<=1.00001,"UI corner escaped its eye frustum");
                        ++checks;
                    }
                }
            }
        const std::array<olvr::EyePose,2> eyes{{{{-.032f,0,0},{0,0,0,1}},{{.032f,0,0},{0,0,0,1}}}};
        const std::array<olvr::EyeFov,2> fovs{{{-.8f,.8f,.8f,-.8f},{-.8f,.8f,.8f,-.8f}}};
        for (float aspect:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN()}) {
            try {olvr::headlocked_ui_constants(eyes,fovs,aspect);}
            catch (const std::invalid_argument&) {++rejected;continue;}
            throw std::runtime_error("Invalid UI aspect was accepted");
        }
        for (float distance:{0.f,-1.f,.01f,std::numeric_limits<float>::quiet_NaN()}) {
            try {olvr::headlocked_ui_constants(eyes,fovs,1,distance);}
            catch (const std::invalid_argument&) {++rejected;continue;}
            throw std::runtime_error("Invalid UI distance was accepted");
        }
        for (float limit:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN()}) {
            bool invalid=false;
            try {olvr::headlocked_ui_constants(eyes,fovs,1,2,limit);}
            catch (const std::invalid_argument&) {invalid=true;}
            require(invalid,"Invalid UI safe-area extent was accepted");
        }
        require(checks==5184 && rejected==7,"UI projection proof was truncated");
        std::cout<<"PASS: "<<(hud?"passive HUD safe-area; ":"")<<"independent UI angular projection; checks="<<checks<<"; invalid_inputs_rejected="<<rejected
            <<"; portrait_square_16x9_21x9; finite_depths=1,2,4; extreme_pitch_roll; canted_antipodal_eyes; max_ndc_error="
            <<maximum_error<<'\n';
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
