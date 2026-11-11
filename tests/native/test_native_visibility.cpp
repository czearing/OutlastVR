#include <windows.h>
#include "outlast_visibility.h"
#include "outlast_native_image.h"
#include "outlast_stereo_culling.h"
#include "MinHook.h"
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

static void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using Predicate=int(WINAPI*)(const void*,const float*,const float*);
static Predicate original_sphere=nullptr;
static const olvr::OutlastStereoCulling* owned_culling=nullptr;
static unsigned intercepted_spheres=0,original_spheres=0;
static int WINAPI stereo_sphere(const void* frustum,const float* center,const float* radius) {
    if (owned_culling && owned_culling->owns(frustum)) {
        ++intercepted_spheres;
        return owned_culling->sphere(frustum,center,radius)?1:0;
    }
    ++original_spheres;
    return original_sphere(frustum,center,radius);
}

int wmain(int argc,wchar_t** argv) {
    void* code=nullptr;
    bool hooks_initialized=false;
    try {
        require(argc==2,"Specify the private MCP-recovered native sphere predicate fixture");
        const auto bytes=olvr::outlast_proof_body(std::filesystem::path(argv[1]),0x148780,194,
            "2b2f605c831e9b73bbc6766c6cc24e5dca2fb2a0743797512c9f2cb0b4d2846b");
        require(bytes.size()==194,"Native predicate does not have its recovered exact body extent");
        code=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        require(code!=nullptr,"Cannot allocate isolated native predicate");
        std::memcpy(code,bytes.data(),bytes.size());
        DWORD previous=0;
        require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous)!=FALSE,"Cannot protect native predicate");
        require(FlushInstructionCache(GetCurrentProcess(),code,bytes.size())!=FALSE,"Cannot publish native predicate");
        const auto native=reinterpret_cast<Predicate>(code);
        const std::array<std::array<float,4>,8> planes{{
            {1,0,0,10},{-1,0,0,10},{0,1,0,10},{0,-1,0,10},
            {0,0,1,10},{0,0,-1,10},{1,0,0,10},{-1,0,0,10}}};
        const std::array<float,7> coordinates{-12,-10,-1,0,9,10,12};
        std::size_t checks=0;
        for (const std::int32_t count:{0,4,8}) {
            std::array<unsigned char,0x12c0> view{};
            auto* frustum=view.data()+0x3b0;
            std::array<float,32> permuted{};
            for (std::int32_t group=0;group<count/4;++group)
                for (unsigned axis=0;axis<4;++axis)
                    for (unsigned lane=0;lane<4;++lane)
                        permuted[group*16+axis*4+lane]=planes[group*4+lane][axis];
            if (count<=4)
                std::memcpy(frustum+0x80,permuted.data(),count*sizeof(float)*4);
            else {
                auto* pointer=permuted.data();
                std::memcpy(frustum+0x100,&pointer,sizeof(pointer));
            }
            std::memcpy(frustum+0x110,&count,sizeof(count));
            const auto original=view;
            for (float radius:{-.5f,-std::numeric_limits<float>::denorm_min(),0.f,.5f,2.f})
                for (float x:coordinates) for (float y:coordinates) for (float z:coordinates) {
                    const float center[]{x,y,z,1};
                    bool expected=true;
                    for (std::int32_t p=0;p<count;++p)
                        expected &= planes[p][0]*x+planes[p][1]*y+planes[p][2]*z-planes[p][3]<=radius;
                    require(bool(native(frustum,center,&radius))==expected,
                        "Actual native sphere predicate differs from independent plane-distance calculation");
                    ++checks;
                    {
                        olvr::OutlastVisibilityScope scope(view.data(),view.size(),1);
                        require(native(frustum,center,&radius)==1,"Scoped owned view did not conservatively retain a sphere");
                        ++checks;
                    }
                    require(view==original,"Visibility scope did not byte-exactly restore the native view");
                    ++checks;
                }
            require(view==original,"Actual native predicate mutated its view");
        }
        std::array<unsigned char,0x12c0> invalid{};
        const std::int32_t malformed=3;
        std::memcpy(invalid.data()+0x3b0+0x110,&malformed,sizeof(malformed));
        const auto before=invalid;
        bool rejected=false;
        try { olvr::OutlastVisibilityScope scope(invalid.data(),invalid.size(),1); }
        catch (const std::invalid_argument&) { rejected=true; }
        require(rejected && invalid==before,"Invalid frustum layout changed native memory");
        const olvr::TrackingFrame tracking{{0,0,0},{1,0,0},{0,1,0},{0,0,-1},1,{0,0,0}};
        const olvr::EyeFov fov{-.785398163f,.785398163f,.785398163f,-.785398163f};
        const olvr::StereoViewConstants eyes{
            olvr::eye_constants(tracking,{{-.032f,0,0},{0,0,0,1}},fov,.1f,100),
            olvr::eye_constants(tracking,{{.032f,0,0},{0,0,0,1}},fov,.1f,100)};
        const olvr::StereoViewConstants mono{
            olvr::eye_constants(tracking,{{0,0,0},{0,0,0,1}},fov,.1f,100),
            olvr::eye_constants(tracking,{{0,0,0},{0,0,0,1}},fov,.1f,100)};
        const olvr::StereoFrustum original_frustum(mono);
        std::array<unsigned char,0x12c0*2> owned_views{};
        std::array<unsigned char,0x12c0> capture_view{};
        std::array<float,32> mono_planes{};
        for (unsigned group=0;group<2;++group) for (unsigned axis=0;axis<4;++axis)
            for (unsigned lane=0;lane<4;++lane) {
                const auto plane_index=group*4+lane;
                olvr::StereoPlane plane{{0,0,0},1};
                if (plane_index<6) plane=original_frustum.planes(0)[plane_index];
                const float values[]{plane.normal.x,plane.normal.y,plane.normal.z,plane.distance};
                mono_planes[group*16+axis*4+lane]=values[axis];
            }
        auto set_frustum=[&](unsigned char* view) {
            auto* pointer=mono_planes.data();
            const std::int32_t count=8;
            std::memcpy(view+0x3b0+0x100,&pointer,sizeof(pointer));
            std::memcpy(view+0x3b0+0x110,&count,sizeof(count));
        };
        set_frustum(owned_views.data());set_frustum(owned_views.data()+0x12c0);set_frustum(capture_view.data());
        const auto views_before=owned_views;
        const auto capture_before=capture_view;
        const olvr::OutlastStereoCulling culling(owned_views.data(),owned_views.size(),2,eyes);
        const olvr::OutlastStereoCulling mono_culling(owned_views.data(),owned_views.size(),2,mono);
        const std::uint32_t sentinel_bits=0xffffffffu;
        float sentinel=0;
        std::memcpy(&sentinel,&sentinel_bits,sizeof(sentinel));
        const float sentinel_center[]{sentinel,sentinel,sentinel,1};
        require(native(owned_views.data()+0x3b0,sentinel_center,&sentinel)==1,
            "Actual retail predicate rejected the captured all-FFFFFFFF sphere");
        const float infinity=std::numeric_limits<float>::infinity();
        const std::array<float,8> special_values{-2,0,2,
            std::numeric_limits<float>::quiet_NaN(),sentinel,infinity,-infinity,
            std::numeric_limits<float>::signaling_NaN()};
        std::size_t nonfinite_checks=0;
        for (float radius:special_values)
            for (float x:special_values) for (float y:special_values) for (float z:special_values) {
                if (std::isfinite(radius) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
                    continue;
                const float center[]{x,y,z,1};
                const bool expected=native(owned_views.data()+0x3b0,center,&radius)!=0;
                require(mono_culling.sphere(owned_views.data()+0x3b0,center,&radius)==expected,
                    "Native nonfinite sphere differs from the actual retail predicate");
                ++checks;++nonfinite_checks;
            }
        for (float radius:{-.5f,-std::numeric_limits<float>::denorm_min(),0.f,.5f})
            for (float x:{-2.f,-.4f,0.f,.4f,2.f}) {
                const float center[]{x,0,1,1};
                require(mono_culling.sphere(owned_views.data()+0x3b0,center,&radius)==
                    bool(native(owned_views.data()+0x3b0,center,&radius)),
                    "Native signed sphere margin differs from the actual retail predicate");
                ++checks;
            }
        require(culling.owns(owned_views.data()+0x3b0) &&
            culling.owns(owned_views.data()+0x12c0+0x3b0) &&
            !culling.owns(owned_views.data()+0x3b1) &&
            !culling.owns(owned_views.data()+0x3b0+0x12c0*2) &&
            !culling.owns(capture_view.data()+0x3b0),"Culling ownership included an unrelated or misaligned volume");
        const float eye_only[]{1.01f,0,1,1},outside[]{2,0,1,1},zero=0;
        require(native(owned_views.data()+0x3b0,eye_only,&zero)==0,
            "Actual native mono culler did not reproduce the missing-eye problem");
        require(MH_Initialize()==MH_OK,"Cannot initialize native sphere detours");
        hooks_initialized=true;
        require(MH_CreateHook(code,reinterpret_cast<void*>(&stereo_sphere),
            reinterpret_cast<void**>(&original_sphere))==MH_OK,"Cannot create actual native sphere detour");
        require(MH_EnableHook(code)==MH_OK,"Cannot activate actual native sphere detour");
        owned_culling=&culling;
        require(native(owned_views.data()+0x3b0,eye_only,&zero)==1 &&
            native(owned_views.data()+0x12c0+0x3b0,eye_only,&zero)==1,
            "Actual native sphere hooks did not preserve both owned eye-only objects");
        require(native(owned_views.data()+0x3b0,outside,&zero)==0,
            "Stereo hook disabled frustum rejection instead of using the eye union");
        require(native(capture_view.data()+0x3b0,eye_only,&zero)==0 &&
            intercepted_spheres==3 && original_spheres==1,
            "Stereo sphere hook changed capture/shadow volume behavior");
        require(native(owned_views.data()+0x3b0,sentinel_center,&sentinel)==1 &&
            native(owned_views.data()+0x12c0+0x3b0,sentinel_center,&sentinel)==1 &&
            native(capture_view.data()+0x3b0,sentinel_center,&sentinel)==1 &&
            intercepted_spheres==5 && original_spheres==2,
            "Actual native detour did not preserve the captured NaN sphere for owned/unowned views");
        owned_culling=nullptr;
        require(native(owned_views.data()+0x3b0,eye_only,&zero)==0,
            "Sphere hook retained a released main-view scope");
        require(MH_RemoveHook(code)==MH_OK,"Cannot remove actual native sphere detour");
        require(MH_Uninitialize()==MH_OK,"Cannot release native sphere detours");
        hooks_initialized=false;
        require(std::memcmp(code,bytes.data(),bytes.size())==0 &&
            owned_views==views_before && capture_view==capture_before,
            "Stereo scope mutated native view memory or failed byte-exact detour removal");
        require(VirtualFree(code,0,MEM_RELEASE)!=FALSE,"Cannot release native predicate");
        code=nullptr;
        std::cout<<"PASS: actual MCP-recovered 194-byte Outlast sphere predicate; independent_checks="
            <<checks<<"; inline/external planes, boundary radii, conservative scoped visibility, byte-exact restoration"
            <<"; actual stereo-union detour, two owned views, untouched captures, scope release and outside-volume rejection"
            <<"; native_nonfinite_checks="<<nonfinite_checks
            <<"; all-FFFFFFFF sphere through actual detours; no game launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        owned_culling=nullptr;
        if (hooks_initialized && MH_Uninitialize()!=MH_OK) std::cerr<<"FAIL: native sphere detour cleanup\n";
        if (code && !VirtualFree(code,0,MEM_RELEASE)) std::cerr<<"FAIL: native fixture allocation cleanup\n";
        return 1;
    }
}
