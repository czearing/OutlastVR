#include <windows.h>
#include "outlast_native_image.h"
#include "outlast_stereo_culling.h"
#include "MinHook.h"
#include <iostream>

namespace {
using Predicate=int(WINAPI*)(const void*,const float*,const float*,int*);
Predicate original=nullptr;
const olvr::OutlastStereoCulling* owned=nullptr;
unsigned intercepted=0,forwarded=0;
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
int WINAPI replacement(const void* frustum,const float* center,const float* radius,int* inside) {
    if (owned && owned->owns(frustum)) {
        bool full=false;
        const auto visible=owned->sphere(frustum,center,radius,&full);
        *inside=full?1:0;
        ++intercepted;
        return visible?1:0;
    }
    ++forwarded;
    return original(frustum,center,radius,inside);
}
}

int wmain(int argc,wchar_t** argv) {
    void* code=nullptr;
    bool initialized=false;
    try {
        require(argc==2 || (argc==3 && std::wstring(argv[2])==L"--fault-body-camera"),
            "Specify the exact retail image and optional body-camera control");
        const bool fault=argc==3;
        const auto bytes=olvr::outlast_proof_body(std::filesystem::path(argv[1]),0x148850,249,
            "e45e61a0c050b38fd1ad6e11635334a75907a4dd54b7120a35e69e2fd1e42f97");
        code=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        require(code!=nullptr,"Cannot allocate native sphere-containment fixture");
        std::memcpy(code,bytes.data(),bytes.size());
        DWORD protection=0;
        require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&protection)!=FALSE &&
            FlushInstructionCache(GetCurrentProcess(),code,bytes.size())!=FALSE,
            "Cannot publish native sphere-containment fixture");
        const auto native=reinterpret_cast<Predicate>(code);
        const olvr::TrackingFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,-1},1,{0,0,0}};
        const olvr::EyeFov fov{-.785398163f,.785398163f,.785398163f,-.785398163f};
        const auto mono_eye=olvr::eye_constants(frame,{{0,0,0},{0,0,0,1}},fov,.1f,100);
        const olvr::StereoViewConstants mono{mono_eye,mono_eye};
        const olvr::StereoFrustum mono_frustum(mono);
        std::array<float,32> planes{};
        for (unsigned p=0;p<8;++p) {
            const auto plane=mono_frustum.planes(0)[p%6];
            const float values[]{plane.normal.x,plane.normal.y,plane.normal.z,plane.distance};
            for (unsigned axis=0;axis<4;++axis) planes[(p/4)*16+axis*4+p%4]=values[axis];
        }
        std::array<unsigned char,0x12c0*2> views{};
        std::array<unsigned char,0x12c0> capture{};
        auto initialize=[&](unsigned char* view) {
            auto* pointer=planes.data();
            const int count=8;
            std::memcpy(view+0x3b0+0x100,&pointer,sizeof(pointer));
            std::memcpy(view+0x3b0+0x110,&count,sizeof(count));
        };
        initialize(views.data());initialize(views.data()+0x12c0);initialize(capture.data());
        const auto before=views;
        const auto capture_before=capture;
        const olvr::OutlastStereoCulling body(views.data(),views.size(),2,mono);
        unsigned checks=0,ieee_checks=0;
        const float infinity=std::numeric_limits<float>::infinity();
        std::uint32_t sentinel_bits=0xffffffffu;
        float sentinel=0;
        std::memcpy(&sentinel,&sentinel_bits,sizeof(sentinel));
        const float values[]{-20,-2,0,2,20,sentinel,infinity,-infinity};
        for (const float radius:values) for (const float x:values)
            for (const float y:values) for (const float z:values) {
                const float center[]{x,y,z};
                int native_full=17;
                const bool expected=native(views.data()+0x3b0,center,&radius,&native_full)!=0;
                bool full=false;
                const bool actual=body.sphere(views.data()+0x3b0,center,&radius,&full);
                if (actual!=expected)
                    throw std::runtime_error("Sphere containment visibility mismatch: center="+
                        std::to_string(x)+","+std::to_string(y)+","+std::to_string(z)+
                        "; radius="+std::to_string(radius)+"; native="+std::to_string(expected));
                require(!full || native_full,"Sphere containment falsely claimed the complete sphere was inside");
                if (!std::isfinite(radius) || !olvr::finite({x,y,z})) {
                    require(full==bool(native_full),"Sphere containment changed native IEEE full-inside behavior");
                    ++ieee_checks;
                }
                ++checks;
            }
        require(MH_Initialize()==MH_OK,"Cannot initialize sphere-containment detour");
        initialized=true;
        require(MH_CreateHook(code,reinterpret_cast<void*>(&replacement),
            reinterpret_cast<void**>(&original))==MH_OK && MH_EnableHook(code)==MH_OK,
            "Cannot activate actual sphere-containment detour");
        for (const float yaw:{-1.57079632679f,1.57079632679f,3.14159265359f}) {
            olvr::StereoViewConstants eyes{};
            const olvr::Quaternion head{0,std::sin(yaw/2),0,std::cos(yaw/2)};
            for (unsigned eye=0;eye<2;++eye) {
                const float ipd=eye?.032f:-.032f;
                eyes[eye]=olvr::eye_constants(frame,
                    {{ipd*std::cos(yaw),0,-ipd*std::sin(yaw)},head},fov,.1f,100);
            }
            const olvr::OutlastStereoCulling tracked(views.data(),views.size(),2,eyes);
            const float ahead[]{-10*std::sin(yaw),0,10*std::cos(yaw)},radius=.1f;
            const float behind[]{-ahead[0],0,-ahead[2]};
            int full=17;
            owned=nullptr;
            require(native(views.data()+0x3b0,ahead,&radius,&full)==0 && full==0,
                "Retail body-facing containment did not reproduce head-turn rejection");
            owned=fault?&body:&tracked;
            for (unsigned index=0;index<2;++index) {
                auto* frustum=views.data()+index*0x12c0+0x3b0;
                require(native(frustum,ahead,&radius,&full)==1 && full==1,
                    "Native containment culled head-facing geometry before the hooked box test");
                require(native(frustum,behind,&radius,&full)==0 && full==0,
                    "Native containment disabled rejection behind the tracked eyes");
                ++checks;
            }
            require(native(capture.data()+0x3b0,ahead,&radius,&full)==0 && full==0,
                "Head-turn containment changed an unowned capture/shadow frustum");
            const float crossing[]{-std::sin(yaw),1,std::cos(yaw)},crossing_radius=.1f;
            require(native(views.data()+0x3b0,crossing,&crossing_radius,&full)==1 && full==0,
                "Partially intersecting sphere incorrectly skipped downstream box refinement");
            ++checks;
            owned=nullptr;
            require(native(views.data()+0x3b0,ahead,&radius,&full)==0 && full==0,
                "Sphere-containment ownership leaked after renderer scope release");
        }
        require(intercepted==15 && forwarded==9,"Native containment detour coverage was truncated");
        require(MH_RemoveHook(code)==MH_OK && MH_Uninitialize()==MH_OK,
            "Cannot remove native sphere-containment detour");
        initialized=false;
        require(std::memcmp(code,bytes.data(),bytes.size())==0 && views==before && capture==capture_before,
            "Sphere-containment detour did not preserve native code/view bytes");
        require(VirtualFree(code,0,MEM_RELEASE)!=FALSE,"Cannot release native containment fixture");
        code=nullptr;
        std::cout<<"PASS: actual MCP-recovered 249-byte sphere-containment predicate; independent_checks="
            <<checks<<"; native_nonfinite_checks="<<ieee_checks
            <<"; unchanged body with -90/+90/180-degree head turns; actual detour; two owned views;"
            <<" unowned captures unchanged; partial/full/outside classifications; byte-exact restoration\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        owned=nullptr;
        if (initialized && MH_Uninitialize()!=MH_OK) std::cerr<<"FAIL: native containment detour cleanup\n";
        if (code && !VirtualFree(code,0,MEM_RELEASE)) std::cerr<<"FAIL: native containment allocation cleanup\n";
        return 1;
    }
}
