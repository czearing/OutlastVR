#pragma once
#include "d3d9_resource.h"
#include "xr_view_constants.h"
#include "outlast_config.h"
#include "outlast_skeleton.h"
#include "outlast_night_vision.h"
#include <cstdint>
#include <xinput.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>

namespace olvr {

using MainViewportEntry=void(WINAPI*)(void*,float*,void*);
using EnqueueEntry=void(WINAPI*)(void*,const void*);
using ConstructorEntry=void*(WINAPI*)(void*,const void*,std::uint64_t,const void*,UINT);
using RendererEntry=void(WINAPI*)(void*);
using ViewEntry=void(WINAPI*)(void*,const void*);
using IndexedEntry=void(WINAPI*)(void*,void*,UINT,INT,UINT,UINT,UINT,UINT);
using SphereEntry=int(WINAPI*)(const void*,const float*,const float*);
using SphereInsideEntry=int(WINAPI*)(const void*,const float*,const float*,int*);
using CrosshairUpdateEntry=void(WINAPI*)(void*,float);
using BoxEntry=int(WINAPI*)(const void*,const float*,const float*);
using BoxInsideEntry=int(WINAPI*)(const void*,const float*,const float*,int*);
using TextureEntry=void*(WINAPI*)(void*,UINT,UINT,BYTE,UINT,UINT,std::uintptr_t);
using SurfaceEntry=void*(WINAPI*)(void*,UINT,UINT,BYTE,std::uintptr_t,UINT,std::uintptr_t);
using BatchedEntry=std::uint64_t(WINAPI*)(void*,const void*,UINT,std::uintptr_t,UINT,std::uint32_t);
using UICommandEntry=std::uint64_t(WINAPI*)(void*);
using CameraBaseEntry=void(WINAPI*)(void*,float*);
using CameraUpdateEntry=void(WINAPI*)(void*,float);
using HeroCastEntry=void*(WINAPI*)(void*);
using InputTickEntry=void(WINAPI*)(void*,float);
using GamepadStateEntry=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
using GamepadVibrationEntry=DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*);
using ViewportFocusEntry=INT(WINAPI*)(void*);
using TranslucencyBoundsEntry=INT(WINAPI*)(void*,UINT,RECT*);

struct RuntimeBindings {
    MainViewportEntry main_viewport=nullptr;
    EnqueueEntry enqueue=nullptr;
    ConstructorEntry constructor=nullptr;
    RendererEntry dispatch=nullptr,normal=nullptr,alternate=nullptr;
    ViewEntry view=nullptr;
    IndexedEntry indexed=nullptr;
    SphereEntry sphere=nullptr;
    BoxEntry box=nullptr;
    BoxInsideEntry box_inside=nullptr;
    TextureEntry texture=nullptr;
    SurfaceEntry surface=nullptr;
    BatchedEntry batched=nullptr;
    UICommandEntry ui=nullptr;
    SphereInsideEntry sphere_inside=nullptr;
    CrosshairUpdateEntry crosshair_update=nullptr;
    CameraBaseEntry camera_base=nullptr;
    CameraUpdateEntry camera_update=nullptr,lean_tick=nullptr;
    HeroCastEntry hero_cast=nullptr;
    InputTickEntry input_tick=nullptr;
    GamepadStateEntry gamepad_state=nullptr;
    void** gamepads=nullptr;
    INT* gamepad_count=nullptr;
    GamepadVibrationEntry gamepad_vibration=nullptr;
    ViewportFocusEntry viewport_focus=nullptr;
    TranslucencyBoundsEntry translucency_bounds=nullptr;
    SkeletonEvaluateEntry skeleton_evaluate=nullptr;
    void** night_vision_engine=nullptr;
    std::uint64_t(WINAPI* menu_state)()=nullptr;
    OutlastNightEffect night_effect=nullptr;
    OutlastCameraEffect normal_effect=nullptr,camera_effect=nullptr;
    OutlastNightLight night_light=nullptr;
};

struct RuntimeTestConfiguration {
    UINT size=sizeof(RuntimeTestConfiguration);
    UINT version=12;
    RuntimeBindings bindings{};
    PFN_xrGetInstanceProcAddr xr_resolve=nullptr;
    Vec3 world_up{0,1,0};
    OutlastComfortSettings comfort;
};

struct RuntimeStatistics {
    UINT size=sizeof(RuntimeStatistics);
    UINT initialized=0;
    UINT devices=0;
    UINT frames=0;
    UINT submitted=0;
    UINT geometry_draws=0;
    UINT consumed_replay_calls=0;
    UINT tagged_allocations=0;
    UINT tracked_views=0;
    UINT owned_culls=0;
    UINT non_scene_draws=0;
    UINT reset_count=0;
    UINT failures=0;
    UINT thread_handoffs=0;
    UINT ui_draws=0;
    UINT ui_plane_draws=0;
    UINT world_panel_draws=0;
    UINT source_width=0,source_height=0,shader_variants=0;
    UINT gpu_timing_status=0,gpu_timing_frame=0;
    UINT64 xr_wait_us=0,cpu_render_us=0,gpu_work_us=0;
    UINT crosshair_updates=0;
    UINT sphere_inside_culls=0;
    UINT camera_pose_matches=0,camera_stabilized_views=0;
    UINT direct_xr_submissions=0;
    UINT64 state_block_allocations=0,state_block_captures=0;
    UINT floor_reference_acquisitions=0;
    float standing_reference_m=0;
    BOOL floor_reference_ready=FALSE;
};

}
