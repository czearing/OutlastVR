#include "outlast_runtime_api.h"
#include "native_hook_set.h"
#include "outlast_native_image.h"
#include "outlast_renderer_ownership.h"
#include "outlast_native_view.h"
#include "outlast_locomotion_camera.h"
#include "outlast_motion_controls.h"
#include "outlast_comfort.h"
#include "outlast_native_use.h"
#include "outlast_camcorder.h"
#include "outlast_native_camcorder.h"
#include "outlast_body_ik.h"
#include "outlast_hand_alignment.h"
#include "outlast_hand_collision.h"
#include "outlast_desktop_cursor.h"
#include "outlast_focus_pause.h"
#include "outlast_menu_state.h"
#include "outlast_render_size.h"
#include "outlast_stereo_culling.h"
#include "outlast_replay_compactor.h"
#include "outlast_target_roles.h"
#include "stereo_reconstruction.h"
#include "headlocked_ui.h"
#include "world_panel.h"
#include "native_camcorder_probe.h"
#include "outlast_config.h"
#include "outlast_night_vision.h"
#include "d3d9_night_vision.h"
#include "native_mono_resolve_probe.h"
#include "d3d9_frame_profile.h"
#include "native_draw_probe.h"
#include "native_glass_reference_probe.h"
#include "native_probe_input.h"
#include "d3d9_shader_variants.h"
#include "openxr_loader.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>

using namespace olvr;
namespace {
HMODULE own_module=nullptr;
bool headless_probe=false;
thread_local unsigned internal_depth=0;
struct Internal {
    Internal(){++internal_depth;}
    ~Internal(){--internal_depth;}
};
std::mutex initialization_mutex;
std::string last_error;
std::ofstream logfile;
std::mutex logfile_mutex;
void log(const std::string& text) {
    OutputDebugStringA((text+"\n").c_str());
    std::lock_guard<std::mutex> lock(logfile_mutex);
    if (logfile) {logfile<<text<<'\n';logfile.flush();}
}
[[noreturn]] void fatal(const std::exception& error) {
    last_error=error.what();log("FATAL: "+last_error);
    std::array<wchar_t,32768> executable{};
    GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
    if (headless_probe || std::filesystem::path(executable.data()).filename()==L"test_integrated_mod.exe")
        std::cerr<<"FATAL: "<<last_error<<'\n';
    else
        MessageBoxA(nullptr,last_error.c_str(),"Outlast single-pass VR stopped",MB_OK|MB_ICONERROR);
    RaiseFailFastException(nullptr,nullptr,0);std::terminate();
}
template<class F> auto guarded(F&& fn)->decltype(fn()) {
    try {return fn();} catch (const std::exception& error) {fatal(error);}
}
template<class T> T field(const void* object,std::size_t offset) {
    if (!object) throw std::invalid_argument("Null native object");
    T value{};std::memcpy(&value,static_cast<const unsigned char*>(object)+offset,sizeof(value));return value;
}
template<class F> F entry(void* object,unsigned index) {
    void** table=nullptr;std::memcpy(&table,object,sizeof(table));return reinterpret_cast<F>(table[index]);
}
std::filesystem::path module_path(HMODULE module) {
    std::array<wchar_t,32768> path{};
    const auto count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if (!count || count>=path.size()) throw std::runtime_error("Cannot resolve loaded module path");
    return std::wstring(path.data(),count);
}

class Runtime {
    using AdapterCount=UINT(WINAPI*)(IDirect3D9*);
    using AdapterIdentifier=HRESULT(WINAPI*)(IDirect3D9*,UINT,DWORD,D3DADAPTER_IDENTIFIER9*);
    using ModeCount=UINT(WINAPI*)(IDirect3D9*,UINT,D3DFORMAT);
    using EnumMode=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DFORMAT,UINT,D3DDISPLAYMODE*);
    using DisplayMode=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDISPLAYMODE*);
    using CheckType=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,D3DFORMAT,BOOL);
    using CheckFormat=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,DWORD,D3DRESOURCETYPE,D3DFORMAT);
    using CheckSamples=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,BOOL,D3DMULTISAMPLE_TYPE,DWORD*);
    using CheckDepth=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,D3DFORMAT,D3DFORMAT);
    using CheckConversion=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,D3DFORMAT);
    using AdapterCaps=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DCAPS9*);
    using AdapterMonitor=HMONITOR(WINAPI*)(IDirect3D9*,UINT);
    using CreateDevice=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
    using GetBackBuffer=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,UINT,D3DBACKBUFFER_TYPE,IDirect3DSurface9**);
    using SetTarget=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,IDirect3DSurface9*);
    using SetViewport=HRESULT(WINAPI*)(IDirect3DDevice9*,const D3DVIEWPORT9*);
    using GetViewport=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DVIEWPORT9*);
    using CreateTexture=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*);
    using CreateTarget=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,UINT,D3DFORMAT,D3DMULTISAMPLE_TYPE,DWORD,BOOL,IDirect3DSurface9**,HANDLE*);
    using TextureDesc=HRESULT(WINAPI*)(IDirect3DTexture9*,UINT,D3DSURFACE_DESC*);
    using SurfaceDesc=HRESULT(WINAPI*)(IDirect3DSurface9*,D3DSURFACE_DESC*);
    using TextureSurface=HRESULT(WINAPI*)(IDirect3DTexture9*,UINT,IDirect3DSurface9**);
    using Clear=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
    using Stretch=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const RECT*,D3DTEXTUREFILTERTYPE);
    using UpdateSurface=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const POINT*);
    using Fill=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,D3DCOLOR);
    using DrawIndexed=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
    using DrawPrimitive=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
    using DrawUP=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
    using DrawIndexedUP=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
    using Present=HRESULT(WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
    using Reset=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
    using FactoryEntry=IDirect3D9*(WINAPI*)(UINT);
    using PrecomputedVisibility=const BYTE*(WINAPI*)(void*,const void*,const void*);
    PrecomputedVisibility original_probe_visibility_=nullptr;
    std::ofstream probe_visibility_;
    struct Allocation {OutlastTargetRole role;UINT width,height;};
    struct ViewState {
        const void* view;
        OutlastViewCamera camera;
        StereoViewConstants eyes;
        std::unique_ptr<OutlastStereoCulling> culling;
    };
    struct RenderState {
        void* renderer;
        bool main;
        bool panel;
        std::vector<std::unique_ptr<ViewState>> views;
        ViewState* current=nullptr;
    };
    static inline Runtime* instance_=nullptr;
    static inline thread_local RenderState* render_=nullptr;
    static inline thread_local Allocation* allocation_=nullptr;
    static inline thread_local OutlastReplayCompactor* replay_=nullptr;
    static inline thread_local bool canvas_=false;
    static inline thread_local bool scaleform_=false;
    using MovieDisplay=void(WINAPI*)(void*,void*);
    using DisplayRoot=void*(WINAPI*)(void*);
    UICommandEntry original_offscreen_movie_=nullptr;
    MovieDisplay original_movie_display_=nullptr;
    DisplayRoot movie_display_root_=nullptr;
    static inline thread_local void* owned_hud_display_root_=nullptr;
    static inline thread_local bool rendering_native_hud_=false;
    std::unique_ptr<LoadedOpenXRStereoSession> loaded_xr_;
    std::unique_ptr<OpenXRStereoSession> test_xr_;
    OutlastRendererOwnership ownership_;
    RuntimeBindings original_{};
    std::uintptr_t base_=0;
    std::size_t module_extent_=0;
    void** factory_iat_=nullptr;
    void* saved_factory_=nullptr;
    std::array<UINT,3> saved_policy_{};
    bool policies_=false,test_=false,frame_open_=false,frame_attempted_=false;
    bool motion_enabled_=true;
    bool recommended_render_size_=false;
    using ResizeFrameEntry=void(WINAPI*)(void*,UINT,UINT,INT,INT,INT);
    using ResizeRHIEntry=void(WINAPI*)(void*,UINT,UINT,INT,INT);
    ResizeFrameEntry original_resize_frame_=nullptr;
    ResizeRHIEntry original_resize_rhi_=nullptr;
    using ParticleParameters=void(WINAPI*)(void*,void*,void*,void*);
    ParticleParameters original_particle_parameters_=nullptr;
    inline static thread_local const void* particle_parameter_view_=nullptr;
    inline static thread_local std::uint64_t particle_parameter_frame_=0;
    inline static thread_local bool particle_camera_axes_view_dependent_=false;
    std::recursive_mutex graphics_mutex_;
    DWORD graphics_thread_=0,last_graphics_thread_=0,creation_thread_=0;
    class GraphicsScope {
        Runtime& runtime_;
        std::unique_lock<std::recursive_mutex> lock_;
        DWORD previous_;
    public:
        explicit GraphicsScope(Runtime& runtime):runtime_(runtime),lock_(runtime.graphics_mutex_),
            previous_(runtime.graphics_thread_) {
            const auto thread=GetCurrentThreadId();
            if (!previous_ && runtime.last_graphics_thread_ && runtime.last_graphics_thread_!=thread)
                ++runtime.stats_.thread_handoffs;
            runtime.graphics_thread_=runtime.last_graphics_thread_=thread;
        }
        ~GraphicsScope(){runtime_.graphics_thread_=previous_;}
    };
    Com<IDirect3DDevice9> device_;
    Com<IDirect3DSurface9> desktop_,final_surface_,submission_surface_;
    Com<IDirect3DTexture9> final_,submission_;
    std::unique_ptr<D3D9ShaderVariants> variants_;
    OutlastRecenter recenter_;
    bool floor_reference_ready_=false,floor_wait_logged_=false;
    std::array<EyePose,2> tracked_{};
    std::array<EyeFov,2> fovs_{};
    Vec3 world_up_{0,0,1};
    StereoViewConstants ui_eyes_{};
    bool ui_draw_=false;
    using MenuStateEntry=std::uint64_t(WINAPI*)();
    MenuStateEntry menu_state_=nullptr;
    using MenuMovieEntry=const void*(WINAPI*)(const void*,UINT);
    MenuMovieEntry menu_movie_=nullptr;
    bool menu_seen_=false;
    bool present_panel_=false;
    bool last_present_panel_=false;
    WorldPanel panel_anchor_;
    std::atomic<bool> native_interactive_menu_{false};
    bool menu_overlay_=false;
    std::unique_ptr<D3D9WorldPanel> panel_renderer_;
    std::unordered_map<std::uintptr_t,Allocation> native_textures_;
    UINT width_=0,height_=0;
    std::ofstream probe_cameras_;
    std::unique_ptr<NativeCamcorderProbe> camcorder_probe_;
    std::unique_ptr<NativeCamcorderProbe> camcorder_capture_;
    std::unique_ptr<NativeCamcorderMesh> camcorder_mesh_;
    using MeshVisibility=void(WINAPI*)(void*,INT);
    MeshVisibility original_mesh_visibility_=nullptr;
    std::ofstream camcorder_mesh_events_;
    OutlastCamcorder camcorder_controls_;
    OutlastConfig config_;
    OutlastComfort comfort_;
    float vignette_strength_=0;
    OutlastNightVision night_vision_;
    bool night_vision_enabled_=false;
    std::unique_ptr<D3D9NightVision> night_vision_filter_;
    using CamcorderRaise=UINT64(WINAPI*)(void*);
    using CamcorderLower=void(WINAPI*)(void*);
    using CamcorderNV=void(WINAPI*)(void*,INT);
    using CollectGarbage=void(WINAPI*)(UINT64,INT);
    CamcorderRaise raise_camcorder_=nullptr;
    CamcorderLower lower_camcorder_=nullptr;
    CamcorderNV original_nv_effect_=nullptr;
    CamcorderLower original_camera_effect_=nullptr,normal_camera_effect_=nullptr;
    CamcorderLower original_camera_hud_=nullptr;
    using FindScriptFunction=void*(WINAPI*)(void*,UINT64,INT);
    FindScriptFunction find_script_function_=nullptr;
    std::atomic<UINT64> isolated_camera_hud_{0};
    CamcorderNV request_camcorder_gc_=nullptr;
    bool camcorder_gc_requested_=false;
    std::ofstream camcorder_lifecycle_;
    std::ofstream camcorder_composition_;
    std::unique_ptr<NativeMonoResolveProbe> camcorder_resolve_probe_;
    using CamcorderEnable=void(WINAPI*)(void*,UINT);
    CamcorderEnable enable_camera_component_=nullptr;
    using PostprocessDispatch=UINT(WINAPI*)(void*,UINT,UINT);
    PostprocessDispatch original_postprocess_dispatch_=nullptr;
    using UberRender=UINT64(WINAPI*)(void*,void*,UINT,void*,void*,void*);
    UberRender original_uber_render_=nullptr;
    using BoundShaders=void(WINAPI*)(void*,void*,void*,void*,UINT,void*,UINT);
    BoundShaders original_bound_shaders_=nullptr;
    void* traced_uber_proxy_=nullptr;
    using FinishView=void(WINAPI*)(void*,void*,INT);
    FinishView original_finish_view_=nullptr;
    using ShaderTexture=void(WINAPI*)(void*,void*,void*,void*,INT);
    ShaderTexture original_shader_texture_=nullptr;
    void* capture_finish_view_=nullptr;
    bool capture_finish_ldr_source_=false;
    bool stock_camera_fx_control_=false;
    Com<IDirect3DPixelShader9> capture_gamma_shader_;
    std::ofstream native_postprocess_dispatch_;
    using UberProxy=void*(WINAPI*)(void*,void*);
    UberProxy original_uber_proxy_=nullptr;
    using CaptureProxies=void(WINAPI*)(void*);
    CaptureProxies original_capture_proxies_=nullptr;
    static inline thread_local bool building_capture_proxies_=false;
    UINT64 native_nv_lut_name_=0,native_nv_lut_package_=0,native_texture_class_name_=0;
    std::ofstream native_postprocess_factory_;
    std::ofstream native_effect_visibility_;
    using MaterialProxy=void*(WINAPI*)(void*,void*,void*,void*);
    MaterialProxy original_material_proxy_=nullptr;
    std::mutex native_postprocess_factory_mutex_;
    bool capture_postprocess_scope_=false;
    CollectGarbage original_collect_garbage_=nullptr;
    void** camcorder_engine_=nullptr;
    struct PhysicalCamera {
        CamcorderFrame frame{};
        Vec3 native_lens{};
        std::array<INT,3> rotation{};
        bool night_vision=false;
        bool native_mesh_ready=false;
        std::uintptr_t native_mesh_component=0,native_mesh_asset=0;
        float battery=0;
        BYTE native_state=0,native_mode=0;
        UINT64 clock=0,generation=0;
    };
    std::mutex camcorder_mutex_;
    PhysicalCamera physical_camera_{};
    UINT64 camcorder_generation_=0;
    std::atomic<UINT64> isolated_camera_effects_{0};
    Com<IDirect3DSurface9> camcorder_world_depth_;
    std::unique_ptr<D3D9WorldPanel> camcorder_renderer_;
    float camcorder_near_=.01f,camcorder_far_=1000,camcorder_depth_scale_=0;
    std::ofstream camcorder_events_;
    std::ofstream camcorder_gpu_events_;
    static inline thread_local Com<IDirect3DTexture9>* probe_created_texture_=nullptr;
    std::optional<OutlastDesktopCursor> desktop_cursor_;
    std::ofstream probe_menu_pointer_;
    bool probe_menu_fixture_=false;
    std::atomic<UINT64> native_menu_cursor_queries_{0};
    OutlastDesktopCursor::Clip original_clip_cursor_=nullptr;
    OutlastDesktopCursor::Position original_set_cursor_pos_=nullptr;
    OutlastDesktopCursor::GetPosition original_get_cursor_pos_=nullptr;
    HWND menu_window_=nullptr;
    std::optional<POINT> menu_native_position_;
    struct MenuEdge {POINT position;bool down;};
    std::mutex menu_edge_mutex_;
    std::vector<MenuEdge> menu_edges_;
    static inline thread_local std::optional<POINT> menu_dispatch_position_;
    bool menu_pointer_down_=false;
    bool menu_pointer_tracking_=false;
    ControllerMenuPress menu_press_;
    unsigned menu_pointer_hand_=1;
    std::atomic<UINT64> suppressed_cursor_clips_{0},suppressed_cursor_warps_{0};
    std::ofstream probe_desktop_cursor_;
    using DirectInputFactory=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
    using DirectInputDevice=HRESULT(WINAPI*)(void*,REFGUID,void**,IUnknown*);
    using MouseCooperation=HRESULT(WINAPI*)(void*,HWND,DWORD);
    DirectInputFactory original_direct_input_=nullptr;
    std::atomic<void*> desktop_mouse_{nullptr};
    Com<IUnknown> desktop_mouse_reference_;
    std::atomic<UINT64> nonexclusive_mouse_requests_{0};
    struct InputImport {void** slot;void* previous;};
    std::vector<InputImport> desktop_input_imports_;
    using CameraBaseEntry=void(WINAPI*)(void*,float*);
    using CameraUpdateEntry=void(WINAPI*)(void*,float);
    CameraBaseEntry original_camera_base_=nullptr;
    CameraUpdateEntry original_camera_update_=nullptr;
    CameraUpdateEntry original_lean_tick_=nullptr;
    using HeroCastEntry=void*(WINAPI*)(void*);
    HeroCastEntry hero_cast_=nullptr;
    std::ofstream probe_camera_poses_;
    std::ofstream probe_camera_updates_;
    std::ofstream probe_lean_weights_;
    std::unordered_map<std::uintptr_t,float> lean_weights_;
    std::mutex camera_pose_mutex_;
    UINT64 camera_pose_serial_=0;
    OutlastLocomotionCamera locomotion_camera_;
    struct CameraCapture {
        void* camera;
        std::optional<OutlastLocomotionSample> sample;
    };
    static inline thread_local CameraCapture* camera_capture_=nullptr;
    using InputTickEntry=void(WINAPI*)(void*,float);
    using FocusPauseEntry=void(WINAPI*)(void*,int);
    using MouseDataEntry=HRESULT(WINAPI*)(void*,DWORD,void*,DWORD*,DWORD);
    using InputEnqueueEntry=void(WINAPI*)(void*,void*,UINT,WPARAM,LPARAM);
    using InputConsumeEntry=void(WINAPI*)(void*,const void*);
    InputConsumeEntry original_menu_consume_=nullptr;
    using MenuKeyEntry=INT(WINAPI*)(void*,void*,INT,UINT64,INT,float,INT);
    using MenuAxisEntry=INT(WINAPI*)(void*,void*,INT,UINT64,float,float,INT);
    MenuKeyEntry original_menu_key_=nullptr;
    using KeyStateEntry=SHORT(WINAPI*)(int);
    InputTickEntry original_input_tick_=nullptr;
    GamepadStateEntry original_gamepad_state_=nullptr;
    using ProbeStartupPad=BOOL(WINAPI*)(DWORD*,BOOL*);
    ProbeStartupPad probe_startup_pad_=nullptr;
    std::atomic<UINT> probe_startup_state_{UINT_MAX};
    GamepadVibrationEntry original_gamepad_vibration_=nullptr;
    ViewportFocusEntry original_viewport_focus_=nullptr;
    static inline thread_local bool native_input_poll_=false;
    void** gamepads_=nullptr;
    INT* gamepad_count_=nullptr;
    OutlastMotionControls motion_controls_;
    SkeletonEvaluateEntry original_skeleton_evaluate_=nullptr;
    BoneNameEntry bone_name_=nullptr;
    BoneIndexEntry bone_index_=nullptr;
    BoneMatrixEntry bone_matrix_=nullptr;
    std::ofstream probe_skeleton_;
    std::ofstream probe_hand_contacts_;
    std::ofstream probe_hand_recovery_;
    std::mutex contact_log_mutex_;
    bool probe_skeleton_all_=false;
    void* probe_reference_asset_=nullptr;
    std::mutex skeleton_mutex_;
    UINT64 skeleton_observations_=0;
    OutlastBodyIK body_ik_;
    float hand_wristward_m_=outlast_default_hand_wristward_m;
    std::array<bool,2> hand_reach_warned_{};
    struct BodyFrame {
        BodyIKTargets targets;
        OutlastLocomotionSample native;
        UINT64 clock;
        MotionFrame motion{};
    };
    std::optional<BodyFrame> body_frame_;
    OutlastNativeUseContext native_use_context_{};
    using NativeUseEntry=UINT64(WINAPI*)(void*,INT);
    NativeUseEntry original_pickup_=nullptr,original_use_volume_=nullptr;
    std::ofstream probe_native_use_;
    std::atomic<bool> native_use_thread_warned_{false};
    NativeWorldTrace world_trace_=nullptr;
    void** native_world_=nullptr;
    OutlastHandCollision hand_collision_;
    std::atomic<DWORD> input_thread_{0};
    bool collision_thread_warned_=false;
    struct CollisionFrame {
        std::array<HandContact,2> hands{};
        std::array<bool,2> tracked{};
        Vec3 root{};
        std::uintptr_t hero=0;
        void* world=nullptr;
        UINT64 generation=0,clock=0;
    };
    std::optional<CollisionFrame> collision_frame_;
    UINT64 collision_generation_=0,collision_updated_=0;
    std::uintptr_t collision_hero_=0;
    Vec3 collision_root_{};
    UINT64 body_generation_=1,calibrated_generation_=0;
    std::uintptr_t standing_camera_hero_=0;
    float standing_camera_height_=0;
    FocusPauseEntry original_focus_pause_=nullptr;
    OutlastFocusPause focus_pause_;
    std::recursive_mutex focus_pause_mutex_;
    enum class ProbeFocusPolicy { Legacy,Tracked,Control };
    ProbeFocusPolicy probe_focus_policy_=ProbeFocusPolicy::Legacy;
    MouseDataEntry original_mouse_data_=nullptr;
    InputEnqueueEntry original_input_enqueue_=nullptr;
    KeyStateEntry original_key_state_=nullptr;
    NativeProbeKeys probe_keys_;
    std::atomic<void*> probe_mouse_{nullptr};
    std::atomic<UINT64> suppressed_mouse_records_{0};
    bool probe_desktop_unfocused_=false;
    std::ofstream probe_input_events_;
    std::mutex probe_input_mutex_;
    std::unique_ptr<NativeDrawProbe> draw_probe_;
    std::unique_ptr<NativeDrawProbe> contact_draw_probe_;
    std::unique_ptr<NativeDrawProbe> recovery_draw_probe_;
    std::unique_ptr<NativeGlassReferenceProbe> glass_reference_probe_;
    std::atomic<UINT> probe_recovery_hand_mask_{0};
    std::atomic<UINT64> probe_recovery_ready_since_us_{0};
    bool probe_draw_contact_=false;
    bool probe_draw_jump_=false;
    bool probe_draw_camera_=false;
    UINT64 probe_camera_ready_since_us_=0;
    std::atomic<UINT64> probe_contact_ready_since_us_{0};
    std::atomic<UINT64> probe_body_ready_since_us_{0};
    RuntimeStatistics stats_{};
    std::unique_ptr<D3D9FrameProfile> gpu_profile_;
    LARGE_INTEGER clock_frequency_{},work_start_{};
    struct PerformanceBatch {
        UINT count=0,gpu_count=0,gpu_frame=std::numeric_limits<UINT>::max(),period_count=0;
        UINT64 cpu=0,wait=0,submit=0,gpu=0,cpu_max=0,gpu_max=0,last=0,period=0;
        bool panel=false;
        std::array<UINT64,300> periods{};
    } performance_;
    void record_performance(UINT64 submit_us) {
        if (headless_probe) return;
        auto& p=performance_;
        if (p.panel!=present_panel_) {p=PerformanceBatch{};p.panel=present_panel_;}
        const auto now=input_clock();
        if (p.last && now>=p.last) {
            p.periods[p.period_count++]=now-p.last;p.period+=now-p.last;
        }
        p.last=now;
        p.cpu+=stats_.cpu_render_us;p.wait+=stats_.xr_wait_us;p.submit+=submit_us;
        p.cpu_max=std::max(p.cpu_max,stats_.cpu_render_us);
        if (stats_.gpu_timing_status==2 && p.gpu_frame!=stats_.gpu_timing_frame) {
            p.gpu_frame=stats_.gpu_timing_frame;++p.gpu_count;
            p.gpu+=stats_.gpu_work_us;p.gpu_max=std::max(p.gpu_max,stats_.gpu_work_us);
        }
        if (++p.count<300) return;
        std::sort(p.periods.begin(),p.periods.begin()+p.period_count);
        log("FRAME_PERFORMANCE phase="+std::string(p.panel?"panel":"gameplay")+
            "; submitted_samples="+std::to_string(p.count)+
            "; eye_extent="+std::to_string(xr().width())+"x"+std::to_string(xr().height())+
            "; cpu_render_avg_us="+std::to_string(p.cpu/p.count)+
            "; cpu_render_max_us="+std::to_string(p.cpu_max)+
            "; xr_wait_avg_us="+std::to_string(p.wait/p.count)+
            "; xr_submit_cpu_avg_us="+std::to_string(p.submit/p.count)+
            "; submitted_period_avg_us="+std::to_string(p.period_count?p.period/p.period_count:0)+
            "; submitted_period_p95_us="+std::to_string(p.period_count?
                p.periods[(p.period_count-1)*95/100]:0)+
            "; gpu_render_samples="+std::to_string(p.gpu_count)+
            "; gpu_render_avg_us="+std::to_string(p.gpu_count?p.gpu/p.gpu_count:0)+
            "; gpu_render_max_us="+std::to_string(p.gpu_max)+
            "; gpu_timer_status="+std::to_string(stats_.gpu_timing_status));
        const auto last=p.last;const auto gpu_frame=p.gpu_frame;const auto panel=p.panel;
        p=PerformanceBatch{};p.last=last;p.gpu_frame=gpu_frame;p.panel=panel;
    }
    std::unique_ptr<NativeHookSet> hooks_;
    HANDLE watchdog_=nullptr;
    std::atomic<UINT64> render_heartbeat_{0},input_heartbeat_{0};
    std::atomic<UINT64> camera_heartbeat_{0},skeleton_heartbeat_{0};
    std::atomic<float> camera_elapsed_{0};
    std::atomic<UINT64> render_phase_{0},camera_phase_{0},skeleton_phase_{0};
    std::atomic<UINT64> gameplay_state_{0};
    std::atomic<WORD> action_buttons_{0};
    class DiagnosticScope {
        std::atomic<UINT64>& phase_;
        UINT64 previous_;
    public:
        DiagnosticScope(std::atomic<UINT64>& phase,unsigned operation):phase_(phase),
            previous_(phase.exchange((GetTickCount64()<<8)|operation,std::memory_order_acq_rel)) {}
        ~DiagnosticScope() {phase_.store(previous_,std::memory_order_release);}
    };
    static VOID CALLBACK watch_progress(PVOID context,BOOLEAN) {
        auto& r=*static_cast<Runtime*>(context);
        const auto now=GetTickCount64();
        auto age=[&](UINT64 tick) {return tick && now>=tick?now-tick:0;};
        const auto render_age=age(r.render_heartbeat_.load(std::memory_order_acquire));
        const auto input_age=age(r.input_heartbeat_.load(std::memory_order_acquire));
        const auto state=r.gameplay_state_.load(std::memory_order_acquire);
        if (render_age<5000 && input_age<5000 && (!(state&0xffff) || age(state>>16)<5000)) return;
        log("GAMEPLAY_PROGRESS tick_ms="+std::to_string(now)+
            "; render_idle_ms="+std::to_string(render_age)+"; input_idle_ms="+std::to_string(input_age)+
            "; render_phase="+std::to_string(r.render_phase_.load(std::memory_order_acquire))+
            "; camera_phase="+std::to_string(r.camera_phase_.load(std::memory_order_acquire))+
            "; skeleton_phase="+std::to_string(r.skeleton_phase_.load(std::memory_order_acquire))+
            "; camera_idle_ms="+std::to_string(age(r.camera_heartbeat_.load(std::memory_order_acquire)))+
            "; skeleton_idle_ms="+std::to_string(age(r.skeleton_heartbeat_.load(std::memory_order_acquire)))+
            "; camera_elapsed="+std::to_string(r.camera_elapsed_.load(std::memory_order_acquire))+
            "; native_state="+std::to_string(state&0xffff)+"; state_age_ms="+std::to_string(age(state>>16))+
            "; native_buttons="+std::to_string(r.action_buttons_.load(std::memory_order_acquire)));
    }

    OpenXRStereoSession& xr(){return loaded_xr_?loaded_xr_->session():*test_xr_;}
    UINT64 input_clock() const {
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        return UINT64(double(now.QuadPart)*1000000/clock_frequency_.QuadPart);
    }
    static Runtime& self() {
        if (!instance_) throw std::logic_error("Native runtime callback has no owner");
        return *instance_;
    }
    template<class F> F original(void* object,unsigned index) {
        std::lock_guard<std::recursive_mutex> lock(graphics_mutex_);
        return hooks_->original_for<F>(object,index);
    }
    template<class F> void com_hook(void* object,unsigned index,F replacement) {
        F trampoline=nullptr;hooks_->add(entry<F>(object,index),replacement,trampoline);
    }
    void on_render_thread() {
        if (graphics_thread_!=GetCurrentThreadId())
            throw std::logic_error("VR operation requires serialized graphics ownership");
    }
    bool game_caller(void* address) const {
        const auto value=reinterpret_cast<std::uintptr_t>(address);
        return value>=base_ && value-base_<module_extent_;
    }
    bool target_tag(OutlastAtlasTag& tag) {
        Internal internal;Com<IDirect3DSurface9> target;
        const auto cooperative=device_->TestCooperativeLevel();
        if (cooperative==D3DERR_DEVICELOST || cooperative==D3DERR_DEVICENOTRESET) return false;
        gpu_check(cooperative,"Read native device recovery state");
        gpu_check(device_->GetRenderTarget(0,target.put()),"Read native draw target");
        return target.get() && atlas_tag(*target.get(),tag);
    }
    bool owned_camera_view() const {
        return render_ && !render_->main && camcorder_capture_ &&
            field<std::uintptr_t>(render_->renderer,0x18)==camcorder_capture_->render_target_address();
    }
    void hook_surface(IDirect3DSurface9& surface) {
        com_hook(&surface,12,&surface_desc);hooks_->enable_pending();
    }
    void hook_texture(IDirect3DTexture9& texture) {
        com_hook(&texture,17,&texture_desc);com_hook(&texture,18,&texture_surface);
        hooks_->enable_pending();
    }
    static void tag_surface(IDirect3DSurface9& surface,const Allocation& allocation) {
        const OutlastAtlasTag tag{1,allocation.role,allocation.width,allocation.height};
        gpu_check(surface.SetPrivateData(outlast_atlas_guid,&tag,sizeof(tag),0),"Tag actual native atlas surface");
    }
    void create_final() {
        Internal internal;
        gpu_check(device_->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,desktop_.put()),"Get unmodified desktop backbuffer");
        D3DSURFACE_DESC description{};gpu_check(desktop_->GetDesc(&description),"Read desktop extent");
        width_=description.Width;height_=description.Height;
        stats_.source_width=width_;stats_.source_height=height_;
        if (!width_ || !height_ || width_>8192 || height_>8192 ||
            description.MultiSampleType!=D3DMULTISAMPLE_NONE)
            throw std::runtime_error("Unsupported desktop extent or multisampled VR output");
        gpu_check(device_->CreateTexture(width_*2,height_,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,final_.put(),nullptr),"Create final native stereo atlas");
        tag_atlas_texture(*final_.get(),OutlastTargetRole::post,width_,height_);
        gpu_check(final_->GetSurfaceLevel(0,final_surface_.put()),"Get native final atlas surface");
        hook_texture(*final_.get());hook_surface(*final_surface_.get());
        gpu_check(device_->SetRenderTarget(0,final_surface_.get()),"Bind initial native stereo backbuffer");
        Com<IDirect3DSurface9> depth;
        const auto result=device_->GetDepthStencilSurface(depth.put());
        if (result!=D3DERR_NOTFOUND) {
            gpu_check(result,"Inspect initial native depth");
            if (depth.get()) {
                D3DSURFACE_DESC d{};gpu_check(depth->GetDesc(&d),"Read initial depth format");
                Com<IDirect3DSurface9> replacement;
                gpu_check(device_->CreateDepthStencilSurface(width_*2,height_,d.Format,
                    D3DMULTISAMPLE_NONE,0,TRUE,replacement.put(),nullptr),"Create matching native VR auto-depth");
                tag_surface(*replacement.get(),{OutlastTargetRole::scene_depth,width_,height_});
                hook_surface(*replacement.get());
                gpu_check(device_->SetDepthStencilSurface(replacement.get()),"Bind matching native VR auto-depth");
            }
        }
        D3DVIEWPORT9 viewport{0,0,width_*2,height_,0,1};
        gpu_check(device_->SetViewport(&viewport),"Set initial native atlas viewport");
        ++stats_.tagged_allocations;
    }
    void attach_device(IDirect3DDevice9& device) {
        if (device_.get()) throw std::runtime_error("Multiple native D3D9 devices require an explicit engine policy");
        Internal internal;
        verify_on12_device(device,*xr().graphics().device());
        *device_.put()=&device;device.AddRef();
        creation_thread_=GetCurrentThreadId();
        D3DDEVICE_CREATION_PARAMETERS creation{};
        gpu_check(device.GetCreationParameters(&creation),"Locate owned menu input window");
        menu_window_=creation.hFocusWindow;
        variants_=std::make_unique<D3D9ShaderVariants>(device);
        gpu_profile_=std::make_unique<D3D9FrameProfile>(device);
        com_hook(&device,18,&backbuffer);com_hook(&device,37,&set_target);
        com_hook(&device,47,&set_viewport);com_hook(&device,48,&get_viewport);
        com_hook(&device,23,&create_texture);com_hook(&device,28,&create_target);
        com_hook(&device,29,&create_depth);com_hook(&device,43,&clear);
        com_hook(&device,30,&update_surface);com_hook(&device,34,&stretch);com_hook(&device,35,&fill);
        com_hook(&device,81,&draw_primitive);com_hook(&device,82,&draw_indexed);
        com_hook(&device,83,&draw_up);com_hook(&device,84,&draw_indexed_up);
        com_hook(&device,17,&present);com_hook(&device,16,&reset);
        hooks_->enable_pending();create_final();
        ++stats_.devices;log("Native D3D9On12 device attached; single-pass atlas and real GPU XR transport enabled");
    }
    EyePose relative_pose(EyePose pose) const {return comfort_.pose(recenter_.relative(pose));}
    bool begin_frame() {
        DiagnosticScope diagnostic(render_phase_,2);
        on_render_thread();
        if (frame_open_) return true;
        if (frame_attempted_) return false;
        frame_attempted_=true;
        Internal internal;
        LARGE_INTEGER wait_start{};
        QueryPerformanceCounter(&wait_start);
        if (!xr().begin_frame()) {
            motion_controls_.publish({},input_clock());
            night_vision_.update({},false,input_clock());
            controller_menu_pointer(false,nullptr);
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            body_frame_.reset();
            return false;
        }
        motion_controls_.publish(xr().motion_frame(),input_clock());
        if (motion_enabled_) {
            const auto feedback=motion_controls_.rumble(input_clock());
            for (unsigned hand=0;hand<2;++hand) xr().vibrate(hand,feedback[hand],100000000);
        }
        QueryPerformanceCounter(&work_start_);
        stats_.xr_wait_us=UINT64(double(work_start_.QuadPart-wait_start.QuadPart)*1000000/clock_frequency_.QuadPart);
        if (gpu_profile_) gpu_profile_->begin(stats_.frames);
        frame_open_=true;
        const auto& views=xr().views();
        std::array<EyePose,2> absolute{};
        for (unsigned eye=0;eye<2;++eye) {
            const auto& p=views[eye].pose;
            Quaternion q{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w};
            const float norm=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
            q={q.x/norm,q.y/norm,q.z/norm,q.w/norm};
            absolute[eye]={{p.position.x,p.position.y,p.position.z},q};
            const auto& f=views[eye].fov;
            fovs_[eye]={f.angleLeft,f.angleRight,f.angleUp,f.angleDown};
        }
        const auto changes=xr().take_reference_changes();
        for (const auto& change:changes) {
            if (recenter_.initialized() && change.referenceSpaceType==xr().reference_space_type()) {
                if (change.poseValid) {
                    const auto& pose=change.poseInPreviousSpace;
                    recenter_.change_reference({{pose.position.x,pose.position.y,pose.position.z},
                        {pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w}});
                } else {
                    log("OpenXR origin transform unavailable; "+std::string(xr().floor_known()?
                        "retaining measured floor-height reference":"relative height continuity cannot be established"));
                }
            }
        }
        if (floor_reference_ready_ && recenter_.standing_height()<=0) {
            floor_reference_ready_=false;
            floor_wait_logged_=false;
            log("OpenXR origin change invalidated the standing floor reference; awaiting tracked positive height");
        }
        const auto flags=xr().view_state_flags();
        const auto tracked_flags=XR_VIEW_STATE_POSITION_TRACKED_BIT|XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
        const bool head_tracked=(flags&tracked_flags)==tracked_flags;
        const float measured_height=(absolute[0].position.y+absolute[1].position.y)*.5f;
        const bool acquire_floor=xr().floor_known() && !floor_reference_ready_ &&
            outlast_floor_reference_available(measured_height,
                (flags&XR_VIEW_STATE_POSITION_TRACKED_BIT)!=0,(flags&XR_VIEW_STATE_ORIENTATION_TRACKED_BIT)!=0);
        const bool keyboard_recenter=!headless_probe && (GetAsyncKeyState(VK_F8)&1);
        const bool controller_recenter=motion_controls_.take_recenter(input_clock());
        const bool manual_recenter=(keyboard_recenter || controller_recenter) && head_tracked;
        if ((keyboard_recenter || controller_recenter) && !head_tracked)
            log("Recenter ignored while head tracking is unavailable; release and press again after tracking returns");
        if (!recenter_.initialized() || acquire_floor || manual_recenter || !changes.empty()) {
            const bool initial=!recenter_.initialized();
            recenter_.recenter(absolute,!initial && !acquire_floor);
            comfort_.reset();
            if (acquire_floor) {
                floor_reference_ready_=true;
                ++stats_.floor_reference_acquisitions;
            }
            log(std::string(initial?"Tracking origin initialized: ":"Tracking origin recentered: ")+
                (xr().floor_known()?(floor_reference_ready_?"STAGE floor measured":"STAGE floor pending"):
                    "LOCAL only; absolute floor height unavailable")+
                "; standing_reference_m="+std::to_string(recenter_.standing_height())+
                "; located_height_m="+std::to_string(measured_height)+"; view_state_flags="+std::to_string(flags));
            panel_anchor_.reset();
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            ++body_generation_;
            body_frame_.reset();
        }
        if (xr().floor_known() && !floor_reference_ready_ && !floor_wait_logged_) {
            log("Awaiting tracked positive STAGE height; keeping native camera height; located_height_m="+
                std::to_string(measured_height)+"; view_state_flags="+std::to_string(flags));
            floor_wait_logged_=true;
        }
        const auto motion=motion_controls_.sample(input_clock());
        const auto qa=absolute[0].orientation,qb=absolute[1].orientation;
        const float qsign=qa.x*qb.x+qa.y*qb.y+qa.z*qb.z+qa.w*qb.w<0?-1.f:1.f;
        const auto head=recenter_.relative({scale(add(absolute[0].position,absolute[1].position),.5f),
            bone_normalize({qa.x+qsign*qb.x,qa.y+qsign*qb.y,qa.z+qsign*qb.z,qa.w+qsign*qb.w})});
        const bool gameplay=!present_panel_ && !native_interactive_menu_.load(std::memory_order_acquire);
        comfort_.update(config_.comfort,motion,head,gameplay && head_tracked,input_clock());
        motion_controls_.movement_yaw(!head_tracked &&
            config_.comfort.movement_reference==MovementReference::head?std::optional<float>{}:
            comfort_.movement_yaw(config_.comfort,motion,recenter_,head),gameplay);
        vignette_strength_=OutlastComfort::vignette(config_.comfort,motion,gameplay);
        for (unsigned eye=0;eye<2;++eye) tracked_[eye]=relative_pose(absolute[eye]);
        if (xr().floor_known() && !floor_reference_ready_) {
            const float center_y=(tracked_[0].position.y+tracked_[1].position.y)*.5f;
            for (auto& eye:tracked_) eye.position.y-=center_y;
        }
        stats_.floor_reference_ready=floor_reference_ready_?TRUE:FALSE;
        stats_.standing_reference_m=recenter_.standing_height();
        const bool overlay=!present_panel_ && native_interactive_menu_.load(std::memory_order_acquire);
        if (overlay!=menu_overlay_) {
            panel_anchor_.reset();
            menu_overlay_=overlay;
            log(overlay?"Native interactive overlay: upright controller menu plane":
                "Native interactive overlay closed: passive HUD restored");
        }
        ui_eyes_=menu_overlay_?panel_anchor_.locate_ui(tracked_,fovs_,float(width_)/height_):
            headlocked_ui_constants(tracked_,fovs_,float(width_)/height_);
        if (!test_ && !config_.physical_camcorder)
            night_vision_enabled_=night_vision_.update(motion,
                !present_panel_ && !menu_overlay_,input_clock(),config_.comfort.primary_hand());
        return true;
    }
    void publish_body(TrackingFrame& tracking,const OutlastLocomotionView& view) {
        if (!bone_name_) return;
        std::lock_guard<std::mutex> lock(skeleton_mutex_);
        const auto& native=view.native;
        native_use_context_={input_clock(),native.basic,native.special};
        if (xr().floor_known() && !floor_reference_ready_ && !config_.comfort.seated) {
            motion_controls_.physical_stance(false,0,native.pawn_crouched,native.special!=0,input_clock());
            body_frame_.reset();
            return;
        }
        BodyIKTargets targets{};
        targets.grip_anchored=true;
        targets.controller_fingers=true;
        targets.hand_wristward_m=hand_wristward_m_;
        targets.tracking=tracking;
        targets.head.position=scale(add(tracked_[0].position,tracked_[1].position),.5f);
        auto a=tracked_[0].orientation,b=tracked_[1].orientation;
        const float sign=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w<0?-1.f:1.f;
        targets.head.orientation=bone_normalize({a.x+sign*b.x,a.y+sign*b.y,a.z+sign*b.z,a.w+sign*b.w});
        const auto frame=motion_controls_.sample(input_clock());
        const bool airborne=outlast_airborne_locomotion(native.basic,native.special);
        const bool mode_supported=native.mode==0 ||
            (native.special>=1 && native.special<=3 && native.mode==std::numeric_limits<unsigned>::max());
        const bool normal=outlast_tracked_locomotion(native.basic,native.special) && mode_supported && !(native.flags&4) &&
            native.lean_intent==0 && native.lean_observed &&
            std::abs(native.lean_weight)<=8*std::numeric_limits<float>::epsilon();
        const bool eligible=normal && frame.focused;
        const bool stance_gameplay=!present_panel_ && !native_interactive_menu_.load(std::memory_order_acquire);
        const bool physical=motion_controls_.physical_stance(eligible && stance_gameplay && !airborne &&
            !config_.comfort.seated,targets.head.position.y,
            native.pawn_crouched,native.special!=0,input_clock());
        if (motion_controls_.take_stance_warning())
            log("Native physical crouch request was not accepted after two bounded attempts; retries suspended until height changes");
        if (!eligible) {body_frame_.reset();return;}
        if (native.basic==0 && !native.pawn_crouched && !native.special && standing_camera_hero_!=native.hero) {
            standing_camera_hero_=native.hero;
            standing_camera_height_=tracking.origin.z-native.root.z;
        }
        if (!config_.comfort.seated && xr().floor_known() && floor_reference_ready_ && standing_camera_hero_==native.hero) {
            targets.standing_height_delta=recenter_.standing_height()-
                standing_camera_height_/tracking.units_per_meter;
            tracking.origin.z=native.root.z+outlast_floor_eye_height(tracking.origin.z-native.root.z,
                standing_camera_height_,recenter_.standing_height(),tracking.units_per_meter,physical,airborne);
            targets.tracking=tracking;
        } else if (physical && standing_camera_hero_==native.hero &&
                   (native.pawn_crouched || native.special)) {
            tracking.origin.z=native.root.z+standing_camera_height_;
            targets.tracking=tracking;
        }
        targets.compensate_native_crouch=physical;
        for (unsigned hand=0;hand<2;++hand) {
            targets.finger_trigger[hand]=frame.hands[hand].trigger;
            targets.finger_squeeze[hand]=frame.hands[hand].squeeze;
            targets.tracked[hand]=frame.hands[hand].grip_tracked;
            if (!targets.tracked[hand]) continue;
            const auto& pose=frame.hands[hand].grip;
            targets.controllers[hand]=relative_pose({{pose.position.x,pose.position.y,pose.position.z},
                {pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w}});
            targets.hands[hand]=outlast_aligned_hand(targets.controllers[hand],hand_wristward_m_);
        }
        auto relative_motion=frame;
        for (unsigned hand=0;hand<2;++hand) {
            const auto& p=targets.hands[hand];
            relative_motion.hands[hand].grip={{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},
                {p.position.x,p.position.y,p.position.z}};
            if (frame.hands[hand].aim_tracked) {
                const auto& raw=frame.hands[hand].aim;
                const auto aim=relative_pose({{raw.position.x,raw.position.y,raw.position.z},
                    {raw.orientation.x,raw.orientation.y,raw.orientation.z,raw.orientation.w}});
                relative_motion.hands[hand].aim={{aim.orientation.x,aim.orientation.y,aim.orientation.z,aim.orientation.w},
                    {aim.position.x,aim.position.y,aim.position.z}};
            }
        }
        body_frame_=BodyFrame{targets,native,input_clock(),relative_motion};
    }
    TrackingFrame native_body_tracking(void* hero,const BodyFrame& request) const {
        auto tracking=request.targets.tracking;
        const auto root=field<Vec3>(hero,0x80);
        const auto yaw=field<std::array<INT,3>>(hero,0x8c)[1];
        const double angle=(double(yaw)-request.native.yaw)*(6.2831853071795864769/65536);
        auto turn=[&](Vec3 v) {
            return Vec3{float(v.x*std::cos(angle)-v.y*std::sin(angle)),
                float(v.x*std::sin(angle)+v.y*std::cos(angle)),v.z};
        };
        tracking.origin=add(root,turn(add(tracking.origin,scale(request.native.root,-1))));
        tracking.right=turn(tracking.right);tracking.up=turn(tracking.up);
        tracking.backward=turn(tracking.backward);
        return tracking;
    }
    std::optional<OutlastUseRay> controller_use_ray(void* hero,UINT64 now) {
        if (GetCurrentThreadId()!=input_thread_.load(std::memory_order_acquire)) {
            if (!native_use_thread_warned_.exchange(true,std::memory_order_acq_rel))
                log("Controller-directed use deferred outside the observed native gameplay thread");
            return {};
        }
        std::optional<BodyFrame> request;
        {
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            request=body_frame_;
        }
        const auto current=motion_controls_.sample(now);
        if (!request || request->native.hero!=reinterpret_cast<std::uintptr_t>(hero) ||
            !current.focused || !current.hands[config_.comfort.primary_hand()].aim_tracked ||
            native_interactive_menu_.load(std::memory_order_acquire) ||
            menu_state_() || field<BYTE>(hero,0x7bc)!=0 || field<BYTE>(hero,0x7bd)!=0)
            return {};
        return outlast_controller_use_ray(native_body_tracking(hero,*request),request->motion,
            request->clock,now,true,config_.comfort.primary_hand());
    }
    UINT64 evaluate_native_use(NativeUseEntry original,const char* kind,void* hero,INT action) {
        const auto now=input_clock();
        const auto ray=controller_use_ray(hero,now);
        const auto before_origin=field<std::array<unsigned char,sizeof(Vec3)>>(hero,0x30e4);
        const auto before_direction=field<std::array<unsigned char,sizeof(Vec3)>>(hero,0x2bd8);
        std::optional<OutlastNativeUsePose> scope;
        if (ray) scope.emplace(hero,*ray);
        const auto observed_origin=field<Vec3>(hero,0x30e4);
        const auto observed_direction=field<Vec3>(hero,0x2bd8);
        const auto result=original(hero,action);
        scope.reset();
        const bool restored=field<std::array<unsigned char,sizeof(Vec3)>>(hero,0x30e4)==before_origin &&
            field<std::array<unsigned char,sizeof(Vec3)>>(hero,0x2bd8)==before_direction;
        if (ray && !restored)
            throw std::runtime_error("Native use query did not restore the caller's view pose");
        if (probe_native_use_.is_open()) {
            auto& out=probe_native_use_;
            auto vector=[&](Vec3 v) {out<<'['<<v.x<<','<<v.y<<','<<v.z<<']';};
            out<<std::setprecision(9)<<"{\"kind\":\""<<kind<<"\",\"clock_us\":"<<now
                <<",\"thread\":"<<GetCurrentThreadId()<<",\"input_thread\":"
                <<input_thread_.load(std::memory_order_acquire)
                <<",\"hero\":"<<reinterpret_cast<std::uintptr_t>(hero)<<",\"action\":"<<action
                <<",\"controller_aim\":"<<(ray?"true":"false")<<",\"result\":"<<result
                <<",\"origin\":";
            vector(observed_origin);out<<",\"direction\":";vector(observed_direction);
            out<<",\"native_pickup_count\":"<<field<INT>(hero,0x2c2c)
                <<",\"selected_pickup\":"<<reinterpret_cast<std::uintptr_t>(field<void*>(hero,0x3648))
                <<",\"special\":"<<unsigned(field<BYTE>(hero,0x7bd))
                <<",\"view_restored\":"<<(restored?"true":"false")<<",\"body_origin\":";
            vector(field<Vec3>(hero,0x80));
            if (original==original_pickup_) {
                const auto count=field<INT>(hero,0x2c2c);
                auto* const* items=field<void* const*>(hero,0x2c24);
                if (count<0 || count>4096 || (count && !items))
                    throw std::runtime_error("Invalid native pickup candidate inventory");
                out<<",\"native_horizontal_range\":["<<field<float>(hero,0x4390)<<','
                    <<field<float>(hero,0x4394)<<"],\"pickup_candidates\":[";
                for (INT index=0;index<count;++index) {
                    auto* item=items[index];
                    out<<(index?",":"");
                    if (!item) {out<<"null";continue;}
                    out<<"{\"actor\":"<<reinterpret_cast<std::uintptr_t>(item)
                        <<",\"class\":"<<reinterpret_cast<std::uintptr_t>(field<void*>(item,0x50))
                        <<",\"vtable\":"<<reinterpret_cast<std::uintptr_t>(field<void*>(item,0))
                        <<",\"flags\":"<<field<UINT>(item,0x250)<<",\"origin\":";
                    vector(field<Vec3>(item,0x80));out<<'}';
                }
                out<<']';
            }
            out<<"}\n";out.flush();
            if (!out) throw std::runtime_error("Cannot retain native controller-use evidence");
        }
        return result;
    }
    static UINT64 WINAPI native_pickup(void* hero,INT action) {
        return guarded([&] {auto& r=self();return r.evaluate_native_use(r.original_pickup_,"pickup",hero,action);});
    }
    static UINT64 WINAPI native_use_volume(void* hero,INT action) {
        return guarded([&] {auto& r=self();return r.evaluate_native_use(r.original_use_volume_,"use-volume",hero,action);});
    }
    void sample_native_camcorder(PhysicalCamera& camera,const TrackingFrame& tracking) {
        const auto pose=camcorder_mesh_->pose(tracking);
        camera.native_mesh_ready=pose.has_value();
        camera.native_mesh_component=reinterpret_cast<std::uintptr_t>(camcorder_mesh_->component());
        camera.native_mesh_asset=reinterpret_cast<std::uintptr_t>(camcorder_mesh_->asset());
        if (!pose) return;
        camera.frame.pose=pose->model;camera.frame.lens=pose->lens;camera.frame.screen=pose->screen;
        camera.frame.screen_half_extents=pose->screen_half_extents;
        camera.native_lens=pose->native_lens;
        camera.rotation=camcorder_native_rotation(tracking,pose->lens);
    }
    static void WINAPI mesh_visibility(void* hero,INT mode) {
        guarded([&] {
            auto& r=self();r.original_mesh_visibility_(hero,mode);
            if (r.camcorder_mesh_) r.camcorder_mesh_->observe_stock_visibility(hero);
        });
    }
    void update_camcorder(void* camera) {
        if (!camcorder_capture_) return;
        if (!input_thread_.load(std::memory_order_acquire)) return;
        if (input_thread_.load(std::memory_order_acquire)!=GetCurrentThreadId())
            throw std::runtime_error("Physical camcorder requires the native gameplay thread");
        auto* hero=hero_cast_(field<void*>(camera,0x60));
        const auto now=input_clock();
        std::optional<BodyFrame> request;
        {
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            request=body_frame_;
        }
        const bool current=hero && request && request->native.hero==reinterpret_cast<std::uintptr_t>(hero) &&
            now>=request->clock && now-request->clock<=250000;
        PhysicalCamera result{};result.clock=now;result.generation=camcorder_generation_;
        if (current) {
            const auto tracking=native_body_tracking(hero,*request);
            auto* controller=field<void*>(hero,0x2bbc);
            const BYTE state=field<BYTE>(hero,0x303e),mode=field<BYTE>(hero,0x303d);
            if (state>3 || mode>2) throw std::runtime_error("Unknown retail camcorder state/mode");
            const bool available=controller && (field<UINT>(controller,0xa84)&2);
            const bool allowed=outlast_tracked_locomotion(field<BYTE>(hero,0x7bc),field<BYTE>(hero,0x7bd)) &&
                !menu_state_() &&
                !(field<UINT>(hero,0x2f88)&0x80000);
            result.frame=camcorder_controls_.update(request->motion,request->targets.head,{0,0,0,1},
                {available,allowed,state==1,state==2 || state==3},now,camcorder_mesh_->holster(hero,tracking));
            if (result.frame.held) camcorder_capture_->create(hero);
            if (result.frame.request_toggle) {
                if (result.frame.held) raise_camcorder_(hero);
                else lower_camcorder_(hero);
            }
            if (result.frame.held && state==1 && available && allowed) {
                auto* hud=field<void*>(controller,0xa54);
                if (!hud) throw std::runtime_error("Native raised camera lost its controller-owned HUD");
                if (camcorder_capture_->hud_movie()!=field<void*>(hud,0x7ac)) camera_hud_show(hud);
            }
            if (result.frame.request_rejected)
                log("Native physical camcorder request was not acknowledged; retry blocked until a fresh grip edge");
            result.native_state=state;result.native_mode=mode;
            result.battery=field<float>(hero,0x31a8);
            if (!std::isfinite(result.battery)) throw std::runtime_error("Nonfinite native camcorder battery");
            result.night_vision=mode==1 && result.battery>0;
            camcorder_mesh_->control(hero,tracking,result.frame);
            sample_native_camcorder(result,tracking);
            if (available && allowed && !stock_camera_fx_control_) {
                for (const auto offset:{0x2d40u,0x2d48u,0x2d50u}) {
                    auto* component=field<void*>(hero,offset);
                    if (!component) throw std::runtime_error("Native camcorder has no recovered illumination component");
                    enable_camera_component_(component,offset==0x2d50?1:0);
                }
            }
            if (result.frame.held && result.frame.visible && result.native_mesh_ready) {
                if (request_camcorder_gc_ && !camcorder_gc_requested_ && camcorder_capture_->frame()>=450 &&
                    camcorder_capture_->texture_handle()) {
                    camcorder_gc_requested_=true;
                    request_camcorder_gc_(nullptr,0);
                    log("Physical camera fixture requested engine-deferred garbage collection while holding a live lens");
                }
            } else camcorder_capture_->hide();
        } else {
            camcorder_controls_.update({},{{0,0,0},{0,0,0,1}},{0,0,0,1},{},now);
            camcorder_mesh_->release_control();
            camcorder_capture_->hide();
        }
        {
            std::lock_guard<std::mutex> lock(camcorder_mutex_);
            physical_camera_=result;
        }
        if (camcorder_events_.is_open()) {
            const auto& p=result.frame.pose;
            camcorder_events_<<std::setprecision(9)<<"{\"clock\":"<<now
                <<",\"frame\":"<<camcorder_capture_->frame()
                <<",\"body_request_schema_version\":1,\"body_request_clock_us\":";
            if (request) camcorder_events_<<request->clock;
            else camcorder_events_<<"null";
            camcorder_events_<<",\"body_request_age_us\":";
            if (request && now>=request->clock) camcorder_events_<<now-request->clock;
            else camcorder_events_<<"null";
            camcorder_events_<<",\"body_request_matches_hero\":"
                <<(hero && request && request->native.hero==reinterpret_cast<std::uintptr_t>(hero)?"true":"false")
                <<",\"body_request_current\":"<<(current?"true":"false")
                <<",\"generation\":"<<result.generation<<",\"visible\":"<<(result.frame.visible?"true":"false")
                <<",\"held\":"<<(result.frame.held?"true":"false")<<",\"hand\":"<<result.frame.hand
                <<",\"native_mesh_ready\":"<<(result.native_mesh_ready?"true":"false")
                <<",\"native_mesh_component\":"<<result.native_mesh_component
                <<",\"native_mesh_asset\":"<<result.native_mesh_asset
                <<",\"toggle\":"<<(result.frame.request_toggle?"true":"false")
                <<",\"native_state\":"<<unsigned(result.native_state)<<",\"native_mode\":"<<unsigned(result.native_mode)
                <<",\"battery\":"<<result.battery<<",\"night_vision\":"<<(result.night_vision?"true":"false")
                <<",\"body_position\":["<<p.position.x<<','<<p.position.y<<','<<p.position.z
                <<"],\"lens_position\":["<<result.native_lens.x<<','<<result.native_lens.y<<','<<result.native_lens.z
                <<"],\"rotation\":["<<result.rotation[0]<<','<<result.rotation[1]<<','<<result.rotation[2]
                <<"],\"rhi_texture\":"<<camcorder_capture_->texture_handle()
                <<",\"isolated_effects\":"<<isolated_camera_effects_.load(std::memory_order_relaxed)
                <<",\"isolated_hud\":"<<isolated_camera_hud_.load(std::memory_order_relaxed)<<"}\n";
            camcorder_events_.flush();
            if (!camcorder_events_) throw std::runtime_error("Cannot retain physical camcorder evidence");
        }
    }
    static void WINAPI collect_garbage(UINT64 flags,INT full) {
        guarded([&] {
            auto& r=self();
            const auto texture=r.camcorder_capture_?r.camcorder_capture_->texture_handle():0;
            bool held=false;
            {
                std::lock_guard<std::mutex> lock(r.camcorder_mutex_);
                held=r.physical_camera_.frame.held;
            }
            if (r.camcorder_lifecycle_.is_open()) {
                r.camcorder_lifecycle_<<"{\"phase\":\"before_gc\",\"generation\":"<<r.camcorder_generation_
                    <<",\"flags\":"<<flags<<",\"full\":"<<full<<",\"held\":"<<(held?"true":"false")
                    <<",\"rhi_texture\":"<<texture<<",\"requested\":"
                    <<(r.camcorder_gc_requested_?"true":"false")<<"}\n";
                r.camcorder_lifecycle_.flush();
                if (!r.camcorder_lifecycle_) throw std::runtime_error("Cannot retain camera pre-GC evidence");
            }
            if (r.camcorder_probe_) r.camcorder_probe_->release();
            if (r.camcorder_capture_) r.camcorder_capture_->release();
            if (r.camcorder_mesh_) {
                r.camcorder_mesh_->release_control();
                r.camcorder_mesh_->reset();
            }
            // Pointer-free grip latches must survive GC without requiring another squeeze edge.
            {
                std::lock_guard<std::mutex> lock(r.camcorder_mutex_);
                r.physical_camera_={};
                ++r.camcorder_generation_;
            }
            r.original_collect_garbage_(flags,full);
            if (r.camcorder_lifecycle_.is_open()) {
                r.camcorder_lifecycle_<<"{\"phase\":\"after_gc\",\"generation\":"<<r.camcorder_generation_
                    <<",\"rhi_texture\":"<<(r.camcorder_capture_?r.camcorder_capture_->texture_handle():0)<<"}\n";
                r.camcorder_lifecycle_.flush();
                if (!r.camcorder_lifecycle_) throw std::runtime_error("Cannot retain camera post-GC evidence");
            }
        });
    }
    bool isolate_camera_effect(void* effects) {
        if (stock_camera_fx_control_) return false;
        if (!camcorder_capture_ || !camcorder_engine_ || !*camcorder_engine_ ||
            field<void*>(*camcorder_engine_,0xa74)!=effects) return false;
        if (field<BYTE>(effects,0x3b8)!=0) normal_camera_effect_(effects);
        ++isolated_camera_effects_;
        return true;
    }
    static void WINAPI camera_night_effect(void* effects,INT powered) {
        guarded([&] {auto& r=self();
            if (!r.isolate_camera_effect(effects)) r.original_nv_effect_(effects,powered);
        });
    }
    static void WINAPI camera_color_effect(void* effects) {
        guarded([&] {auto& r=self();
            if (!r.isolate_camera_effect(effects)) r.original_camera_effect_(effects);
        });
    }
    static void close_camera_hud_movie(void* movie) {
        auto& r=self();
        if (!movie || !r.bone_name_ || field<std::uintptr_t>(movie,0)!=r.base_+0x1ad8290)
            throw std::runtime_error("Cannot close an unadmitted native camera HUD movie");
        UINT64 name=0;r.bone_name_(&name,L"Close",1);
        auto* function=r.find_script_function_(movie,name,0);
        if (!function) throw std::runtime_error("Native camera HUD close event is unavailable");
        using Event=void(WINAPI*)(void*,void*,void*,void*);
        INT unload=0;
        entry<Event>(movie,0x218/sizeof(void*))(movie,function,&unload,nullptr);
    }
    static void WINAPI camera_hud_show(void* hud) {
        guarded([&] {
            auto& r=self();
            if (!r.camcorder_capture_) {r.original_camera_hud_(hud);return;}
            auto* movie=field<void*>(hud,0x7ac);
            if (!movie || field<std::uintptr_t>(movie,0)!=r.base_+0x1ad8290)
                throw std::runtime_error("Physical camera HUD does not match the admitted retail movie");
            r.camcorder_capture_->bind_hud(movie);
            r.original_camera_hud_(hud);
            r.camcorder_capture_->refresh_hud();
            ++r.isolated_camera_hud_;
        });
    }
    bool trace_native_postprocess() const {
        return native_postprocess_dispatch_.is_open() &&
            (stats_.frames==400 || stats_.frames==711 || stats_.frames==1080 ||
             (draw_probe_ && stats_.frames==draw_probe_->selected_frame()));
    }
    static void WINAPI finish_view(void* renderer,void* view,INT resolve) {
        guarded([&] {auto& r=self();
            auto* previous=r.capture_finish_view_;
            const bool previous_source=r.capture_finish_ldr_source_;
            const bool capture=r.camcorder_capture_ && render_ && !render_->main &&
                field<std::uintptr_t>(renderer,0x18)==r.camcorder_capture_->render_target_address();
            r.capture_finish_view_=capture?view:nullptr;
            r.capture_finish_ldr_source_=false;
            if (r.trace_native_postprocess() && render_ && !render_->main) {
                r.native_postprocess_dispatch_<<"{\"kind\":\"finish_view\",\"frame\":"<<r.stats_.frames
                    <<",\"view\":"<<reinterpret_cast<std::uintptr_t>(view)
                    <<",\"owned_capture\":"<<(capture?"true":"false")
                    <<",\"target\":"<<field<std::uintptr_t>(renderer,0x18)
                    <<",\"expected_target\":"<<(r.camcorder_capture_?r.camcorder_capture_->render_target_address():0)
                    <<",\"ldr\":"<<field<UINT>(view,0x52c)<<"}\n";
            }
            r.original_finish_view_(renderer,view,resolve);
            r.capture_finish_view_=previous;
            r.capture_finish_ldr_source_=previous_source;
        });
    }
    static void WINAPI shader_texture(void* shader,void* parameter,void* sampler,void* texture,INT offset) {
        guarded([&] {auto& r=self();
            auto* selected=texture;
            if (r.capture_finish_view_ && field<UINT>(r.capture_finish_view_,0x52c) &&
                field<USHORT>(parameter,2) && !field<USHORT>(parameter,0) && !offset &&
                texture==*reinterpret_cast<void**>(r.base_+0x20119d8)) {
                selected=*reinterpret_cast<void**>(r.base_+0x2011c08);
                if (!selected || selected==texture)
                    throw std::runtime_error("Completed native camera LDR output is unavailable");
                r.capture_finish_ldr_source_=true;
            }
            if (r.capture_finish_view_ && r.trace_native_postprocess()) {
                r.native_postprocess_dispatch_<<"{\"kind\":\"final_copy_source\",\"frame\":"<<r.stats_.frames
                    <<",\"view\":"<<reinterpret_cast<std::uintptr_t>(r.capture_finish_view_)
                    <<",\"texture\":"<<reinterpret_cast<std::uintptr_t>(texture)
                    <<",\"selected_texture\":"<<reinterpret_cast<std::uintptr_t>(selected)
                    <<",\"native_hdr\":"<<*reinterpret_cast<std::uintptr_t*>(r.base_+0x20119d8)
                    <<",\"native_ldr\":"<<*reinterpret_cast<std::uintptr_t*>(r.base_+0x2011c08)
                    <<",\"register\":"<<field<USHORT>(parameter,0)<<"}\n";
                r.native_postprocess_dispatch_.flush();
                if (!r.native_postprocess_dispatch_) throw std::runtime_error("Cannot retain native final-copy source");
            }
            r.original_shader_texture_(shader,parameter,sampler,selected,offset);
            if (r.camcorder_resolve_probe_ && r.camcorder_capture_->diagnostic_ready() &&
                r.camcorder_resolve_probe_->needs_raw_source(UINT(r.stats_.frames)) &&
                r.capture_postprocess_scope_ && r.owned_camera_view() && render_->current &&
                field<USHORT>(parameter,2) && field<USHORT>(parameter,0)<16 && !offset &&
                texture==*reinterpret_cast<void**>(r.base_+0x20119d8)) {
                Internal internal;
                Com<IDirect3DPixelShader9> pixel;
                gpu_check(r.device_->GetPixelShader(pixel.put()),"Identify native raw-optics source shader");
                if (!pixel.get()) throw std::runtime_error("Native raw-optics source has no pixel shader");
                UINT bytes=0;
                gpu_check(pixel->GetFunction(nullptr,&bytes),"Read native raw-optics shader extent");
                if (bytes<8 || bytes%4 || bytes>1048576)
                    throw std::runtime_error("Invalid native raw-optics shader extent");
                std::vector<DWORD> code(bytes/4);
                const auto expected=bytes;
                gpu_check(pixel->GetFunction(code.data(),&bytes),"Read native raw-optics source shader");
                if (bytes!=expected) throw std::runtime_error("Native raw-optics source shader extent changed");
                {
                    Com<IDirect3DBaseTexture9> source;
                    const auto slot=field<USHORT>(parameter,0);
                    gpu_check(r.device_->GetTexture(slot,source.put()),"Read actual first HDR postprocess sampler");
                    if (!source.get() || source->GetType()!=D3DRTYPE_TEXTURE)
                        throw std::runtime_error("Actual native Uber scene sampler is not a 2D texture");
                    const auto* view=render_->current->view;
                    r.camcorder_resolve_probe_->raw_source(*r.device_.get(),
                        *static_cast<IDirect3DTexture9*>(source.get()),UINT(r.stats_.frames),
                        RECT{field<INT>(view,0x6c),field<INT>(view,0x70),
                            field<INT>(view,0x6c)+field<INT>(view,0x74),
                            field<INT>(view,0x70)+field<INT>(view,0x78)},
                        sha256_bytes(code.data(),bytes),code,slot,reinterpret_cast<std::uintptr_t>(texture),
                        reinterpret_cast<std::uintptr_t>(view));
                }
            }
        });
    }
    static UINT WINAPI postprocess_dispatch(void* renderer,UINT dpg,UINT lighting) {
        return guarded([&] {auto& r=self();
            if (r.trace_native_postprocess()) {
                const auto count=field<INT>(renderer,0x70);
                auto* views=field<BYTE*>(renderer,0x68);
                if (count<0 || count>16 || (count && !views))
                    throw std::runtime_error("Invalid native postprocess dispatch view array");
                r.native_postprocess_dispatch_<<"{\"kind\":\"dispatch\",\"frame\":"<<r.stats_.frames
                    <<",\"renderer\":"<<reinterpret_cast<std::uintptr_t>(renderer)
                    <<",\"main\":"<<(render_ && render_->renderer==renderer && render_->main?"true":"false")
                    <<",\"dpg\":"<<dpg<<",\"lighting_only\":"<<lighting
                    <<",\"show_flags\":["<<field<UINT64>(renderer,0x28)<<','
                    <<field<UINT64>(renderer,0x30)<<"],\"views\":[";
                for (INT i=0;i<count;++i) {
                    auto* view=views+std::size_t(i)*0x12c0;
                    const auto proxies_count=field<INT>(view,0x44);
                    auto* proxies=field<void**>(view,0x3c);
                    if (proxies_count<0 || proxies_count>64 || (proxies_count && !proxies))
                        throw std::runtime_error("Invalid actual native postprocess dispatch proxy array");
                    r.native_postprocess_dispatch_<<(i?",":"")<<"{\"view\":"
                        <<reinterpret_cast<std::uintptr_t>(view)<<",\"proxies\":[";
                    for (INT j=0;j<proxies_count;++j) {
                        auto* proxy=proxies[j];
                        r.native_postprocess_dispatch_<<(j?",":"")<<"{\"proxy\":"
                            <<reinterpret_cast<std::uintptr_t>(proxy)<<",\"flags\":"
                            <<(proxy?field<UINT>(proxy,8):0)<<",\"vtable\":"
                            <<(proxy?field<std::uintptr_t>(proxy,0):0)<<"}";
                    }
                    r.native_postprocess_dispatch_<<"]}";
                }
                r.native_postprocess_dispatch_<<"]}\n";
                r.native_postprocess_dispatch_.flush();
                if (!r.native_postprocess_dispatch_) throw std::runtime_error("Cannot retain actual postprocess dispatch");
            }
            const auto previous=r.capture_postprocess_scope_;
            r.capture_postprocess_scope_=dpg==4 && !lighting && r.owned_camera_view();
            UINT result=0;
            try {
                result=r.original_postprocess_dispatch_(renderer,dpg,lighting);
            } catch (...) {
                r.capture_postprocess_scope_=previous;
                throw;
            }
            r.capture_postprocess_scope_=previous;
            if (r.trace_native_postprocess() && dpg==4 && !lighting) {
                Internal internal;
                Com<IDirect3DSurface9> target;
                gpu_check(r.device_->GetRenderTarget(0,target.put()),"Read completed native postprocess output");
                D3DSURFACE_DESC extent{};
                gpu_check(target->GetDesc(&extent),"Read completed native postprocess output extent");
                OutlastAtlasTag tag{};
                r.native_postprocess_dispatch_<<"{\"kind\":\"dispatch_output\",\"frame\":"<<r.stats_.frames
                    <<",\"renderer\":"<<reinterpret_cast<std::uintptr_t>(renderer)
                    <<",\"surface\":"<<reinterpret_cast<std::uintptr_t>(target.get())
                    <<",\"width\":"<<extent.Width<<",\"height\":"<<extent.Height
                    <<",\"format\":"<<UINT(extent.Format)<<",\"atlas\":"
                    <<(atlas_tag(*target.get(),tag)?"true":"false")<<"}\n";
                r.native_postprocess_dispatch_.flush();
                if (!r.native_postprocess_dispatch_)
                    throw std::runtime_error("Cannot retain completed native postprocess output");
            }
            return result;
        });
    }
    static void WINAPI capture_proxies(void* component) {
        guarded([&] {auto& r=self();
            const auto previous=building_capture_proxies_;
            building_capture_proxies_=r.camcorder_capture_ &&
                r.input_thread_.load(std::memory_order_acquire)==GetCurrentThreadId() &&
                r.camcorder_capture_->owns_component(component);
            try {
                r.original_capture_proxies_(component);
            } catch (...) {
                building_capture_proxies_=previous;
                throw;
            }
            building_capture_proxies_=previous;
        });
    }
    static void* WINAPI material_proxy(void* effect,void* settings,void* third,void* fourth) {
        return guarded([&] {
            auto& r=self();
            const UINT flags=field<UINT>(effect,0x60);
            const bool omitted=building_capture_proxies_ &&
                !NativeCamcorderProbe::gameplay_effect_visible(flags);
            auto* proxy=omitted?nullptr:r.original_material_proxy_(effect,settings,third,fourth);
            if (r.native_effect_visibility_.is_open()) {
                std::lock_guard<std::mutex> lock(r.native_postprocess_factory_mutex_);
                r.native_effect_visibility_<<"{\"kind\":\"material_proxy\",\"private_capture\":"
                    <<(building_capture_proxies_?"true":"false")
                    <<",\"effect\":"<<reinterpret_cast<std::uintptr_t>(effect)
                    <<",\"material\":"<<field<std::uintptr_t>(effect,0x88)
                    <<",\"source_flags\":"<<flags
                    <<",\"native_gameplay_visible\":"
                    <<(NativeCamcorderProbe::gameplay_effect_visible(flags)?"true":"false")
                    <<",\"omitted_disabled_effect\":"<<(omitted?"true":"false")
                    <<",\"proxy\":"<<reinterpret_cast<std::uintptr_t>(proxy)<<"}\n";
                r.native_effect_visibility_.flush();
                if (!r.native_effect_visibility_) throw std::runtime_error("Cannot retain native effect visibility evidence");
            }
            return proxy;
        });
    }
    void* native_camera_night_settings() {
        auto* engine=camcorder_engine_?*camcorder_engine_:nullptr;
        auto* effects=engine?field<void*>(engine,0xa74):nullptr;
        if (!effects) throw std::runtime_error("Native camera night-vision settings owner is unavailable");
        auto* settings=static_cast<BYTE*>(effects)+0x60;
        const auto lut=field<void*>(settings,0xc4);
        if (!(field<UINT>(settings,4)&0x20) || !lut)
            throw std::runtime_error("Actual stock camera night-vision settings have no authored LUT override");
        if (!native_nv_lut_name_) {
            bone_name_(&native_nv_lut_name_,L"lut_night_vision_desatured",1);
            bone_name_(&native_nv_lut_package_,L"Asylum_post_process",1);
            bone_name_(&native_texture_class_name_,L"Texture2D",1);
        }
        auto* type=field<void*>(lut,0x50);
        auto* package=field<void*>(lut,0x40);
        if (field<UINT64>(lut,0x48)!=native_nv_lut_name_ || !type ||
            field<UINT64>(type,0x48)!=native_texture_class_name_ || !package ||
            field<UINT64>(package,0x48)!=native_nv_lut_package_ || field<void*>(package,0x40))
            throw std::runtime_error("Actual camera night-vision settings reference an unadmitted retail LUT");
        return settings;
    }
    static void* WINAPI uber_proxy(void* effect,void* settings) {
        return guarded([&] {auto& r=self();
            const auto mode=UINT(field<BYTE>(effect,0x1d0));
            auto* selected=building_capture_proxies_ && mode==2?r.native_camera_night_settings():settings;
            auto* proxy=r.original_uber_proxy_(effect,selected);
            if (selected!=settings && (!proxy || field<void*>(proxy,0x60)!=field<void*>(selected,0xc4)))
                throw std::runtime_error("Owned camera proxy did not retain the actual stock night-vision LUT");
            if (!r.native_postprocess_factory_.is_open()) return proxy;
            const UINT flags0=settings?field<UINT>(settings,0):0;
            const UINT flags1=settings?field<UINT>(settings,4):0;
            const auto lut=settings && (flags1&0x20)?field<std::uintptr_t>(settings,0xc4):0;
            std::lock_guard<std::mutex> lock(r.native_postprocess_factory_mutex_);
            r.native_postprocess_factory_<<"{\"kind\":\"native_uber_factory\",\"thread\":"<<GetCurrentThreadId()
                <<",\"private_capture\":"<<(building_capture_proxies_?"true":"false")
                <<",\"effect\":"<<reinterpret_cast<std::uintptr_t>(effect)
                <<",\"source_mode\":"<<mode
                <<",\"settings\":"<<reinterpret_cast<std::uintptr_t>(settings)
                <<",\"settings_flags\":["<<flags0<<','<<flags1<<"],\"settings_lut\":"<<lut
                <<",\"selected_settings\":"<<reinterpret_cast<std::uintptr_t>(selected)
                <<",\"selected_flags\":["<<(selected?field<UINT>(selected,0):0)<<','
                <<(selected?field<UINT>(selected,4):0)<<"],\"selected_lut\":"
                <<(selected && (field<UINT>(selected,4)&0x20)?field<std::uintptr_t>(selected,0xc4):0)
                <<",\"grading_texture_count\":"<<(settings?field<INT>(settings,0xd4):0)
                <<",\"grading_weight_count\":"<<(settings?field<INT>(settings,0xe4):0)
                <<",\"proxy\":"<<reinterpret_cast<std::uintptr_t>(proxy)
                <<",\"proxy_lut\":"<<(proxy?field<std::uintptr_t>(proxy,0x60):0)
                <<",\"native_stack_rvas\":[";
            void* stack[20]{};
            const auto depth=CaptureStackBackTrace(0,UINT(std::size(stack)),stack,nullptr);
            bool comma=false;
            for (USHORT j=0;j<depth;++j) {
                const auto address=reinterpret_cast<std::uintptr_t>(stack[j]);
                if (address<r.base_ || address-r.base_>=r.module_extent_) continue;
                r.native_postprocess_factory_<<(comma?",":"")<<(address-r.base_);comma=true;
            }
            r.native_postprocess_factory_<<"]}\n";
            r.native_postprocess_factory_.flush();
            if (!r.native_postprocess_factory_) throw std::runtime_error("Cannot retain native color-grading factory inputs");
            return proxy;
        });
    }
    static void WINAPI bound_shaders(void* state,void* declaration,void* vertex,void* pixel,
                                     UINT stride,void* geometry,UINT topology) {
        guarded([&] {auto& r=self();
            r.original_bound_shaders_(state,declaration,vertex,pixel,stride,geometry,topology);
            if (r.traced_uber_proxy_ && r.trace_native_postprocess()) {
                Internal internal;
                Com<IDirect3DPixelShader9> actual;
                gpu_check(r.device_->GetPixelShader(actual.put()),"Read actual native bound-shader-state pixel shader");
                if (!actual.get()) throw std::runtime_error("Native uber shader binding has no actual GPU pixel shader");
                UINT bytes=0;
                gpu_check(actual->GetFunction(nullptr,&bytes),"Read actual native bound shader extent");
                if (bytes<8 || bytes%4 || bytes>1048576)
                    throw std::runtime_error("Invalid actual native bound shader extent");
                std::vector<DWORD> code(bytes/4);
                const auto expected=bytes;
                gpu_check(actual->GetFunction(code.data(),&bytes),"Read actual native bound shader program");
                if (bytes!=expected) throw std::runtime_error("Actual native bound shader extent changed");
                if (r.draw_probe_) r.draw_probe_->retain_bound_shaders(*r.device_.get(),r.stats_.frames);
                auto* rhi=*reinterpret_cast<void**>(r.base_+0x1ffb0b0);
                auto* methods=field<BYTE*>(rhi,0);
                const auto requested=pixel?field<std::uintptr_t>(pixel,0x4c):0;
                auto* expected_pixel=requested?reinterpret_cast<IDirect3DPixelShader9*>(requested-8):nullptr;
                std::string expected_hash;
                if (expected_pixel) {
                    UINT source_bytes=0;
                    gpu_check(expected_pixel->GetFunction(nullptr,&source_bytes),"Read requested native RHI shader extent");
                    if (source_bytes<8 || source_bytes%4 || source_bytes>1048576)
                        throw std::runtime_error("Invalid requested native RHI shader extent");
                    std::vector<DWORD> source(source_bytes/4);
                    const auto expected_bytes=source_bytes;
                    gpu_check(expected_pixel->GetFunction(source.data(),&source_bytes),"Read requested native RHI shader");
                    if (source_bytes!=expected_bytes) throw std::runtime_error("Requested native shader extent changed");
                    expected_hash=sha256_bytes(source.data(),source_bytes);
                }
                r.native_postprocess_dispatch_<<"{\"kind\":\"bound_shader\",\"frame\":"<<r.stats_.frames
                    <<",\"main\":"<<(render_ && render_->main?"true":"false")
                    <<",\"mode\":"<<field<INT>(r.traced_uber_proxy_,0x240)
                    <<",\"pixel_object\":"<<reinterpret_cast<std::uintptr_t>(pixel)
                    <<",\"pixel_rhi\":"<<requested
                    <<",\"requested_pixel\":"<<std::quoted(expected_hash)
                    <<",\"actual_pixel_object\":"<<reinterpret_cast<std::uintptr_t>(actual.get())
                    <<",\"bind_rva\":"<<field<std::uintptr_t>(methods,0x2d8)-r.base_
                    <<",\"texture_rva\":"<<field<std::uintptr_t>(methods,0x318)-r.base_
                    <<",\"pixel_constant_rva\":"<<field<std::uintptr_t>(methods,0x3f0)-r.base_
                    <<",\"actual_pixel\":"<<std::quoted(sha256_bytes(code.data(),bytes))<<"}\n";
                r.native_postprocess_dispatch_.flush();
                if (!r.native_postprocess_dispatch_) throw std::runtime_error("Cannot retain native bound shader evidence");
            }
        });
    }
    static UINT64 WINAPI uber_render(void* proxy,void* scene,UINT dpg,void* view,void* settings,void* flags) {
        return guarded([&] {auto& r=self();
            if (r.trace_native_postprocess()) {
                r.native_postprocess_dispatch_<<"{\"kind\":\"uber_render\",\"frame\":"<<r.stats_.frames
                    <<",\"proxy\":"<<reinterpret_cast<std::uintptr_t>(proxy)
                    <<",\"view\":"<<reinterpret_cast<std::uintptr_t>(view)
                    <<",\"dpg\":"<<dpg<<",\"mode\":"<<field<INT>(proxy,0x240)
                    <<",\"secondary_mode\":"<<field<INT>(proxy,0x244)
                    <<",\"proxy_flags\":"<<field<UINT>(proxy,8)
                    <<",\"resolve_scene\":"<<field<UINT>(field<void*>(view,0),0x4c)
                    <<",\"family_output_policy\":"<<field<UINT>(field<void*>(view,0),0x54)
                    <<",\"ldr_before\":"<<field<UINT>(view,0x52c)<<"}\n";
                r.native_postprocess_dispatch_.flush();
                if (!r.native_postprocess_dispatch_) throw std::runtime_error("Cannot retain actual native uber render");
            }
            auto* previous=r.traced_uber_proxy_;
            r.traced_uber_proxy_=proxy;
            const auto status=r.original_uber_render_(proxy,scene,dpg,view,settings,flags);
            r.traced_uber_proxy_=previous;
            if (r.trace_native_postprocess()) {
                Internal internal;
                if (r.draw_probe_) r.draw_probe_->retain_bound_shaders(*r.device_.get(),r.stats_.frames);
                Com<IDirect3DSurface9> target;
                gpu_check(r.device_->GetRenderTarget(0,target.put()),"Read actual native postprocess output target");
                D3DSURFACE_DESC extent{};
                gpu_check(target->GetDesc(&extent),"Read actual native postprocess output extent");
                if (r.camcorder_resolve_probe_ && render_ && !render_->main)
                    r.camcorder_resolve_probe_->postprocess_output(*r.device_.get(),*target.get(),
                        UINT(r.stats_.frames),field<INT>(proxy,0x240),
                        RECT{field<INT>(view,0x6c),field<INT>(view,0x70),
                            field<INT>(view,0x6c)+field<INT>(view,0x74),
                            field<INT>(view,0x70)+field<INT>(view,0x78)});
                D3DVIEWPORT9 viewport{};
                gpu_check(r.device_->GetViewport(&viewport),"Read actual native postprocess output viewport");
                OutlastAtlasTag tag{};
                std::array<float,224*4> constants{};
                gpu_check(r.device_->GetPixelShaderConstantF(0,constants.data(),224),
                    "Read actual native postprocess pixel parameters");
                r.native_postprocess_dispatch_<<"{\"kind\":\"uber_output\",\"frame\":"<<r.stats_.frames
                    <<",\"proxy\":"<<reinterpret_cast<std::uintptr_t>(proxy)
                    <<",\"result\":"<<status<<",\"ldr_after\":"<<field<UINT>(view,0x52c)
                    <<",\"surface\":"<<reinterpret_cast<std::uintptr_t>(target.get())
                    <<",\"width\":"<<extent.Width<<",\"height\":"<<extent.Height
                    <<",\"viewport\":["<<viewport.X<<','<<viewport.Y<<','<<viewport.Width<<','<<viewport.Height<<']'
                    <<",\"format\":"<<UINT(extent.Format)<<",\"atlas\":"
                    <<(atlas_tag(*target.get(),tag)?"true":"false")<<",\"pixel_constant_bits\":[";
                for (std::size_t i=0;i<constants.size();++i) {
                    DWORD bits=0;std::memcpy(&bits,&constants[i],sizeof(bits));
                    r.native_postprocess_dispatch_<<(i?",":"")<<bits;
                }
                r.native_postprocess_dispatch_<<"]}\n";
                r.native_postprocess_dispatch_.flush();
                if (!r.native_postprocess_dispatch_) throw std::runtime_error("Cannot retain native postprocess output state");
            }
            return status;
        });
    }
    void trace_camcorder_gpu(const char* operation,const D3DVIEWPORT9* requested=nullptr) {
            if (!camcorder_gpu_events_.is_open() || stats_.frames!=400) return;
            Internal internal;
            Com<IDirect3DSurface9> target;
            gpu_check(device_->GetRenderTarget(0,target.put()),"Inspect diagnostic lens target");
            D3DSURFACE_DESC description{};
            gpu_check(target->GetDesc(&description),"Inspect diagnostic lens extent");
            D3DVIEWPORT9 viewport{};
            gpu_check(device_->GetViewport(&viewport),"Inspect diagnostic lens viewport");
            OutlastAtlasTag tag{};const bool atlas=atlas_tag(*target.get(),tag);
            auto& out=camcorder_gpu_events_;
            out<<"{\"op\":"<<std::quoted(operation)<<",\"main\":"<<(render_ && render_->main?"true":"false")
                <<",\"render\":"<<(render_?"true":"false")<<",\"target\":"
                <<reinterpret_cast<std::uintptr_t>(target.get())<<",\"extent\":["<<description.Width<<','<<description.Height
                <<"],\"atlas\":"<<(atlas?"true":"false")<<",\"role\":"<<(atlas?UINT(tag.role):0)
                <<",\"viewport\":["<<viewport.X<<','<<viewport.Y<<','<<viewport.Width<<','<<viewport.Height<<']';
            if (requested) out<<",\"requested\":["<<requested->X<<','<<requested->Y<<','<<requested->Width<<','<<requested->Height<<']';
            if (render_ && render_->current) {
                const auto origin=render_->current->camera.tracking.origin;
                out<<",\"origin\":["<<origin.x<<','<<origin.y<<','<<origin.z<<']';
            }
            out<<"}\n";out.flush();
            if (!out) throw std::runtime_error("Cannot retain native lens viewport evidence");
    }
    void compose_camcorder() {
        if (!camcorder_capture_ || present_panel_) return;
        PhysicalCamera camera;
        {
            std::lock_guard<std::mutex> lock(camcorder_mutex_);
            camera=physical_camera_;
        }
        const auto now=input_clock();
        if (!camera.frame.visible || !camera.native_mesh_ready || now<camera.clock || now-camera.clock>250000) return;
        if (!camcorder_world_depth_.get())
            throw std::runtime_error("Physical camcorder has no matched native world-depth surface");
        if (!camcorder_renderer_) camcorder_renderer_=std::make_unique<D3D9WorldPanel>(*device_.get());
        auto* image=camcorder_capture_->texture();
        if (camera.frame.held && image) {
            auto* hud=camcorder_capture_->hud_texture();
            if (!hud) {
                if (camcorder_capture_->hud_timed_out())
                    throw std::runtime_error("Actual stock camera HUD has no completed native alpha image");
                return;
            }
            if (camcorder_resolve_probe_) camcorder_resolve_probe_->screen_before(*device_.get(),
                *final_surface_.get(),UINT(stats_.frames),width_,height_,xr().width(),xr().height());
            const auto screens=camcorder_object_constants(camera.frame.screen,tracked_,fovs_,
                camcorder_near_,camcorder_far_,camcorder_depth_scale_,camera.frame.screen_half_extents);
            camcorder_renderer_->draw_screen(*image,*final_surface_.get(),*camcorder_world_depth_.get(),
                width_,height_,screens,camera.night_vision && !camcorder_capture_->native_postprocess(),camera.battery,hud);
            ++stats_.geometry_draws;
            if (camcorder_composition_.is_open()) {
                auto& out=camcorder_composition_;
                out<<std::setprecision(9)<<"{\"frame\":"<<stats_.frames<<",\"clock\":"<<camera.clock
                    <<",\"hand\":"<<camera.frame.hand<<",\"night_vision\":"<<(camera.night_vision?"true":"false")
                    <<",\"native_mesh_component\":"<<camera.native_mesh_component
                    <<",\"native_mesh_asset\":"<<camera.native_mesh_asset
                    <<",\"battery\":"<<camera.battery<<",\"rhi_texture\":"<<camcorder_capture_->texture_handle()
                    <<",\"native_hud_rhi_texture\":"<<camcorder_capture_->hud_texture_handle()
                    <<",\"native_capture_component\":"<<camcorder_capture_->component_address()
                    <<",\"native_postprocess_chain\":"<<camcorder_capture_->postprocess_chain()
                    <<",\"synthetic_night_vision\":"<<
                        (camera.night_vision && !camcorder_capture_->native_postprocess()?"true":"false")
                    <<",\"native_world_camera_effect\":"<<unsigned(field<BYTE>(
                        field<void*>(*camcorder_engine_,0xa74),0x3b8))
                    <<",\"depth_matched\":true,\"screen_matrices\":[";
                for (unsigned eye=0;eye<2;++eye) {
                    out<<(eye?",[":"[");
                    for (unsigned i=0;i<16;++i) out<<(i?",":"")<<screens[eye][i];
                    out<<']';
                }
                out<<"]}\n";out.flush();
                if (!out) throw std::runtime_error("Cannot retain physical screen composition evidence");
            }
        }
    }
    void update_hand_collisions(void* camera) {
        if (!world_trace_) return;
        if (input_thread_.load(std::memory_order_acquire)!=GetCurrentThreadId()) {
            if (!collision_thread_warned_) {
                log("Hand world queries deferred: camera callback is not on the observed native input thread");
                collision_thread_warned_=true;
            }
            return;
        }
        const auto now=input_clock();
        std::optional<BodyFrame> request;
        UINT64 generation=0;
        {
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            request=body_frame_;generation=body_generation_;
        }
        auto* hero=field<void*>(camera,0x60);
        if (!request || request->native.hero!=reinterpret_cast<std::uintptr_t>(hero) ||
            now<request->clock || now-request->clock>250000 || !motion_controls_.sample(now).focused) {
            hand_collision_.reset();
            probe_recovery_hand_mask_.store(0,std::memory_order_release);
            return;
        }
        const auto root=field<Vec3>(hero,0x80);
        const auto moved=add(root,scale(collision_root_,-1));
        if (generation!=collision_generation_ || request->native.hero!=collision_hero_ ||
            now<collision_updated_ || now-collision_updated_>250000 || dot(moved,moved)>40000) {
            hand_collision_.reset();
            probe_recovery_hand_mask_.store(0,std::memory_order_release);
        }
        collision_generation_=generation;collision_hero_=request->native.hero;
        collision_updated_=now;collision_root_=root;
        auto tracking=request->targets.tracking;
        const auto yaw=field<std::array<INT,3>>(hero,0x8c)[1];
        const double angle=(double(yaw)-request->native.yaw)*(6.2831853071795864769/65536);
        auto turn=[&](Vec3 v) {
            return Vec3{float(v.x*std::cos(angle)-v.y*std::sin(angle)),
                float(v.x*std::sin(angle)+v.y*std::cos(angle)),v.z};
        };
        tracking.origin=add(root,turn(add(tracking.origin,scale(request->native.root,-1))));
        tracking.right=turn(tracking.right);tracking.up=turn(tracking.up);tracking.backward=turn(tracking.backward);
        auto* world=*native_world_;
        if (!world) throw std::runtime_error("Eligible native hand collision has no current world");
        const Vec3 anchor{root.x,root.y,root.z+field<float>(hero,0x360)*.75f};
        CollisionFrame result{};
        result.tracked=request->targets.tracked;result.root=root;result.hero=request->native.hero;
        result.world=world;result.generation=generation;result.clock=now;
        for (unsigned hand=0;hand<2;++hand) {
            if (!result.tracked[hand]) {hand_collision_.reset(hand);continue;}
            const auto desired=add(tracking.origin,scale(map_tracking(tracking,
                request->targets.hands[hand].position),tracking.units_per_meter));
            HandContactDiagnostic diagnostic{};
            result.hands[hand]=hand_collision_.update(hand,anchor,desired,tracking.units_per_meter,
                [&](NativeHandHit& hit,Vec3 end,Vec3 start,Vec3 extent) {
                    const bool clear=world_trace_(world,&hit,hero,&end,&start,0x20bf,&extent,nullptr);
                    std::lock_guard<std::mutex> contact_log(contact_log_mutex_);
                    if (probe_hand_contacts_.is_open()) {
                        auto& out=probe_hand_contacts_;
                        auto vector=[&](Vec3 v) {out<<'['<<v.x<<','<<v.y<<','<<v.z<<']';};
                        out<<std::setprecision(9)<<"{\"clock_us\":"<<now<<",\"hand\":"<<hand
                            <<",\"thread\":"<<GetCurrentThreadId()<<",\"input_thread\":"
                            <<input_thread_.load(std::memory_order_acquire)
                            <<",\"clear\":"<<(clear?"true":"false")<<",\"actor\":"<<hit.actor
                            <<",\"time\":"<<hit.time<<",\"start\":";
                        vector(start);out<<",\"end\":";vector(end);out<<",\"extent\":";vector(extent);
                        out<<",\"normal\":";vector(hit.normal);out<<"}\n";out.flush();
                        if (!out) throw std::runtime_error("Cannot retain native hand collision query");
                    }
                    return clear;
                },headless_probe?&diagnostic:nullptr);
            if (headless_probe) {
                if (recovery_draw_probe_ && diagnostic.reset==HandContactReset::Occluded)
                    probe_recovery_hand_mask_.fetch_or(1u<<hand,std::memory_order_release);
                std::lock_guard<std::mutex> contact_log(contact_log_mutex_);
                if (probe_hand_recovery_.is_open()) {
                    auto& out=probe_hand_recovery_;
                    auto vector=[&](Vec3 v) {out<<'['<<v.x<<','<<v.y<<','<<v.z<<']';};
                    out<<std::setprecision(9)<<"{\"clock_us\":"<<now<<",\"hand\":"<<hand
                        <<",\"thread\":"<<GetCurrentThreadId()<<",\"hero\":"<<result.hero
                        <<",\"generation\":"<<generation<<",\"root\":";vector(root);
                    out<<",\"anchor\":";vector(anchor);out<<",\"desired\":";vector(desired);
                    out<<",\"previous_position\":";vector(diagnostic.previous_position);
                    out<<",\"previous_anchor\":";vector(diagnostic.previous_anchor);
                    out<<",\"previous_valid\":"<<(diagnostic.valid?"true":"false")
                        <<",\"previous_blocked\":"<<(diagnostic.blocked?"true":"false")
                        <<",\"revalidated\":"<<(diagnostic.revalidated?"true":"false")
                        <<",\"reset_reason\":"<<unsigned(diagnostic.reset)
                        <<",\"blocked\":"<<(result.hands[hand].blocked?"true":"false")
                        <<",\"position\":";vector(result.hands[hand].position);
                    out<<"}\n";out.flush();
                    if (!out) throw std::runtime_error("Cannot retain native hand recovery evidence");
                }
            }
        }
        std::lock_guard<std::mutex> lock(skeleton_mutex_);
        if (generation==body_generation_) collision_frame_=result;
    }
    ViewState* decode_view(RenderState& state,const void* view) {
        const auto m=field<std::array<float,16>>(view,0x200);
        if (outlast_orthographic_view(outlast_view_matrix(view,0x12c0)))
            return nullptr;
        auto decoded=std::make_unique<ViewState>();
        decoded->view=view;
        try {decoded->camera=outlast_view_camera(view,0x12c0,100);}
        catch (const std::exception& error) {
            std::string details=std::string(error.what())+"; native_main="+(state.main?"1":"0")+"; matrix=";
            for (const auto value:m) details+=std::to_string(value)+",";
            const auto origin=field<Vec3>(view,0x3a0),translation=field<Vec3>(view,0x280);
            details+="; origin="+std::to_string(origin.x)+","+std::to_string(origin.y)+","+std::to_string(origin.z);
            details+="; pretranslation="+std::to_string(translation.x)+","+std::to_string(translation.y)+","+std::to_string(translation.z);
            throw std::runtime_error(details);
        }
        if (!state.main && (camcorder_probe_ || camcorder_capture_))
            (camcorder_probe_?camcorder_probe_:camcorder_capture_)->observe_view(
                field<Vec3>(view,0x3a0),m,UINT(stats_.frames),decoded->camera.tracking,field<INT>(view,0x44));
        const bool located=state.main && begin_frame();
        const bool tracking=located && !state.panel;
        auto tracking_frame=tracking?
            outlast_upright_tracking(decoded->camera.tracking,world_up_):decoded->camera.tracking;
        if (tracking) tracking_frame.units_per_meter/=config_.comfort.world_scale;
        std::optional<OutlastLocomotionView> locomotion;
        if (tracking) {
            std::lock_guard<std::mutex> lock(camera_pose_mutex_);
            locomotion=locomotion_camera_.locate(decoded->camera.tracking.origin);
            if (!locomotion) {
                const auto origin=decoded->camera.tracking.origin;
                throw std::runtime_error("Owned native gameplay view has no matching completed camera pose; origin="+
                    std::to_string(origin.x)+","+std::to_string(origin.y)+","+std::to_string(origin.z)+
                    "; observed_base_serial="+std::to_string(camera_pose_serial_));
            }
            tracking_frame.origin=locomotion->origin;
            ++stats_.camera_pose_matches;
            if (locomotion->stabilized) ++stats_.camera_stabilized_views;
        }
        if (tracking && locomotion) publish_body(tracking_frame,*locomotion);
        else if (located && bone_name_) {
            motion_controls_.physical_stance(false,0,false,false,input_clock());
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            body_frame_.reset();
        }
        for (unsigned eye=0;eye<2;++eye)
            decoded->eyes[eye]=eye_constants(tracking_frame,
                tracking?tracked_[eye]:EyePose{{0,0,0},{0,0,0,1}},
                tracking?fovs_[eye]:decoded->camera.original_fov,
                decoded->camera.near_plane,decoded->camera.far_plane,decoded->camera.depth_scale);
        if (located) {
            if (headless_probe && probe_cameras_.is_open()) {
                probe_cameras_.precision(9);
                probe_cameras_<<"{\"native_frame\":"<<stats_.frames
                    <<",\"world_panel\":"<<(state.panel?"true":"false")<<",\"matrix\":[";
                for (unsigned index=0;index<16;++index) probe_cameras_<<(index?",":"")<<m[index];
                const auto origin=field<Vec3>(view,0x3a0),translation=field<Vec3>(view,0x280);
                UINT64 camera_serial=0;
                {
                    std::lock_guard<std::mutex> lock(camera_pose_mutex_);
                    camera_serial=camera_pose_serial_;
                }
                probe_cameras_<<"],\"origin\":["<<origin.x<<','<<origin.y<<','<<origin.z
                    <<"],\"pretranslation\":["<<translation.x<<','<<translation.y<<','<<translation.z
                    <<"],\"camera_pose_serial\":"<<camera_serial
                    <<",\"camera_source_serial\":"<<(locomotion?locomotion->source_serial:0)
                    <<",\"camera_completed_serial\":"<<(locomotion?locomotion->completed_serial:0)
                    <<",\"camera_stabilized\":"<<(locomotion && locomotion->stabilized?"true":"false")
                    <<",\"tracking_origin\":["<<tracking_frame.origin.x<<','<<tracking_frame.origin.y
                    <<','<<tracking_frame.origin.z<<"]"
                    <<",\"tracking_up\":["<<tracking_frame.up.x<<','<<tracking_frame.up.y<<','<<tracking_frame.up.z
                    <<"],\"tracking_backward\":["<<tracking_frame.backward.x<<','<<tracking_frame.backward.y
                    <<','<<tracking_frame.backward.z<<"],\"rendered_eyes\":[";
                for (unsigned eye=0;eye<2;++eye) {
                    probe_cameras_<<(eye?",[":"[");
                    for (unsigned i=0;i<decoded->eyes[eye].size();++i)
                        probe_cameras_<<(i?",":"")<<decoded->eyes[eye][i];
                    probe_cameras_<<']';
                }
                probe_cameras_<<"]}\n";
                probe_cameras_.flush();
                if (!probe_cameras_) throw std::runtime_error("Native camera evidence write failed");
            }
            decoded->culling=std::make_unique<OutlastStereoCulling>(view,0x12c0,1,decoded->eyes);
            ++stats_.tracked_views;
        }
        auto* result=decoded.get();state.views.push_back(std::move(decoded));return result;
    }
    static void WINAPI camera_base(void* camera,float* pose) {
        guarded([&] {
            auto& r=self();
            r.original_camera_base_(camera,pose);
            const auto* hero=field<const void*>(camera,0x60);
            if (!hero || !pose) throw std::runtime_error("Native camera observer lacks its hero or pose");
            const auto root=field<Vec3>(hero,0x80);
            const auto rotation=field<std::array<INT,3>>(hero,0x8c);
            const auto basic=field<BYTE>(hero,0x7bc),special=field<BYTE>(hero,0x7bd);
            const int mode=!special && basic<16?field<BYTE>(hero,0x3221+std::size_t(basic)*0x40):-1;
            const auto height=field<float>(hero,0x360);
            if (!std::isfinite(height) || !std::isfinite(root.x) ||
                !std::isfinite(root.y) || !std::isfinite(root.z))
                throw std::runtime_error("Native camera observer received a nonfinite root or eye height");
            LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);
            std::lock_guard<std::mutex> lock(r.camera_pose_mutex_);
            const auto serial=++r.camera_pose_serial_;
            const auto lean=r.lean_weights_.find(reinterpret_cast<std::uintptr_t>(hero));
            OutlastLocomotionSample sample{};
            sample.serial=serial;sample.camera=reinterpret_cast<std::uintptr_t>(camera);
            sample.hero=reinterpret_cast<std::uintptr_t>(hero);
            sample.root=root;sample.bone={pose[0],pose[1],pose[2]};sample.yaw=rotation[1];
            sample.basic=basic;sample.special=special;sample.mode=unsigned(mode);
            sample.flags=field<UINT>(camera,0x118);
            sample.crouched=(field<UINT>(hero,0x2f88)&1)!=0;
            sample.pawn_crouched=(field<UINT>(hero,0x288)&0x10)!=0;
            sample.lean_observed=lean!=r.lean_weights_.end();
            sample.lean_weight=sample.lean_observed?lean->second:0.f;
            sample.lean_intent=field<float>(hero,0x30b0);
            const UINT64 state=(UINT64(basic)<<8)|special;
            const auto previous=r.gameplay_state_.load(std::memory_order_acquire);
            if (!previous || (previous&0xffff)!=state) {
                r.gameplay_state_.store((GetTickCount64()<<16)|state,std::memory_order_release);
                log("NATIVE_GAMEPLAY_STATE basic="+std::to_string(basic)+"; special="+std::to_string(special)+
                    "; pawn_crouched="+std::to_string(sample.pawn_crouched)+
                    "; camera_flags="+std::to_string(sample.flags));
            }
            if (camera_capture_ && camera_capture_->camera==camera) camera_capture_->sample=sample;
            if (!headless_probe || !r.probe_camera_poses_.is_open()) return;
            const auto previous_final=field<std::array<float,6>>(camera,0x68);
            auto& output=r.probe_camera_poses_;
            output.precision(9);
            output<<"{\"serial\":"<<serial
                <<",\"clock\":"<<clock.QuadPart<<",\"camera\":"<<reinterpret_cast<std::uintptr_t>(camera)
                <<",\"hero\":"<<reinterpret_cast<std::uintptr_t>(hero)
                <<",\"basic_state\":"<<unsigned(basic)<<",\"special_state\":"<<unsigned(special)
                <<",\"pawn_crouched\":"<<(sample.pawn_crouched?"true":"false")
                <<",\"camera_mode\":"<<mode<<",\"camera_flags\":"<<field<UINT>(camera,0x118)
                <<",\"lean_intent\":"<<sample.lean_intent
                <<",\"lean_weight_observed\":"<<(sample.lean_observed?"true":"false")
                <<",\"lean_weight\":"<<sample.lean_weight
                <<",\"eye_height\":"<<height<<",\"root\":["<<root.x<<','<<root.y<<','<<root.z
                <<"],\"root_rotator\":["<<rotation[0]<<','<<rotation[1]<<','<<rotation[2]
                <<"],\"base_pose\":[";
            for (unsigned index=0;index<6;++index) output<<(index?",":"")<<pose[index];
            output<<"],\"previous_final_pose\":[";
            for (unsigned index=0;index<6;++index) output<<(index?",":"")<<previous_final[index];
            output<<"],\"camera_offsets_0x104\":[";
            for (unsigned index=0;index<5;++index)
                output<<(index?",":"")<<field<float>(camera,0x104+index*4);
            output<<"],\"pawn_eye_region_0x330\":[";
            for (unsigned index=0;index<20;++index)
                output<<(index?",":"")<<field<UINT>(hero,0x330+index*4);
            output<<"],\"stance_region_0x2f80\":[";
            for (unsigned index=0;index<20;++index)
                output<<(index?",":"")<<field<UINT>(hero,0x2f80+index*4);
            output<<"]}\n";output.flush();
            if (!output) throw std::runtime_error("Native camera-pose evidence write failed");
        });
    }
    static void WINAPI camera_update(void* camera,float elapsed) {
        guarded([&] {
            auto& r=self();
            r.camera_heartbeat_.store(GetTickCount64(),std::memory_order_release);
            r.camera_elapsed_.store(elapsed,std::memory_order_release);
            DiagnosticScope diagnostic(r.camera_phase_,1);
            CameraCapture capture{camera,std::nullopt};
            struct RestoreCapture {
                CameraCapture*& current;
                CameraCapture* previous;
                ~RestoreCapture() {current=previous;}
            } restore{camera_capture_,camera_capture_};
            camera_capture_=&capture;
            const bool timed=headless_probe && r.probe_camera_updates_.is_open();
            const auto started=timed?r.input_clock():0;
            r.original_camera_update_(camera,elapsed);
            const auto native_done=timed?r.input_clock():0;
            r.update_hand_collisions(camera);
            const auto collisions_done=timed?r.input_clock():0;
            r.update_camcorder(camera);
            const auto camcorder_done=timed?r.input_clock():0;
            std::lock_guard<std::mutex> lock(r.camera_pose_mutex_);
            const auto final=field<std::array<float,6>>(camera,0x68);
            if (r.camcorder_probe_) {
                auto* hero=r.hero_cast_(field<void*>(camera,0x60));
                if (hero && field<BYTE>(hero,0x7bc)==0 && r.camcorder_probe_->frame()>=250)
                    r.camcorder_probe_->update(hero,final,r.camcorder_probe_->frame());
            }
            if (capture.sample) r.locomotion_camera_.complete(*capture.sample,{final[0],final[1],final[2]});
            else if (elapsed>0)
                throw std::runtime_error("Native camera update did not produce its observed base pose");
            if (!headless_probe || !r.probe_camera_updates_.is_open()) return;
            auto& output=r.probe_camera_updates_;
            output.precision(9);
            output<<"{\"camera_pose_serial\":"<<(capture.sample?capture.sample->serial:0)
                <<",\"camera\":"<<reinterpret_cast<std::uintptr_t>(camera)
                <<",\"clock_us\":"<<r.input_clock()<<",\"elapsed\":"<<elapsed
                <<",\"update_started_us\":"<<started<<",\"native_update_us\":"<<native_done-started
                <<",\"hand_collisions_us\":"<<collisions_done-native_done
                <<",\"physical_camcorder_us\":"<<camcorder_done-collisions_done<<",\"final_pose\":[";
            for (unsigned index=0;index<6;++index) output<<(index?",":"")<<final[index];
            output<<"]}\n";output.flush();
            if (!output) throw std::runtime_error("Native final-camera evidence write failed");
        });
    }
    static void WINAPI lean_tick(void* node,float elapsed) {
        guarded([&] {
            auto& r=self();
            r.original_lean_tick_(node,elapsed);
            const auto mesh=field<void*>(node,0x84);
            if (!mesh) return;
            const auto hero=r.hero_cast_(field<void*>(mesh,0x78));
            if (!hero || mesh!=field<void*>(hero,0x444)) return;
            const auto weight=field<float>(node,0x140);
            if (!std::isfinite(weight)) throw std::runtime_error("Native lean observer received a nonfinite blend");
            LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);
            std::lock_guard<std::mutex> lock(r.camera_pose_mutex_);
            r.lean_weights_[reinterpret_cast<std::uintptr_t>(hero)]=weight;
            if (!headless_probe || !r.probe_lean_weights_.is_open()) return;
            auto& output=r.probe_lean_weights_;
            output.precision(9);
            output<<"{\"clock\":"<<clock.QuadPart<<",\"hero\":"<<reinterpret_cast<std::uintptr_t>(hero)
                <<",\"node\":"<<reinterpret_cast<std::uintptr_t>(node)
                <<",\"weight\":"<<weight<<",\"desired\":"<<field<float>(hero,0x30b0)<<"}\n";
            output.flush();
            if (!output) throw std::runtime_error("Native lean-blend evidence write failed");
        });
    }
    static BOOL WINAPI desktop_clip_cursor(const RECT* rectangle) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        return guarded([&] {
            auto& r=self();
            if (rectangle && r.desktop_cursor_->owns(caller))
                ++r.suppressed_cursor_clips_;
            return r.desktop_cursor_->clip(r.original_clip_cursor_,caller,rectangle);
        });
    }
    static HRESULT WINAPI desktop_mouse_cooperation(void* mouse,HWND window,DWORD flags) {
        return guarded([&] {
            auto& r=self();GraphicsScope graphics(r);
            if (mouse==r.desktop_mouse_.load(std::memory_order_acquire)) {
                flags=outlast_mouse_cooperation(flags);
                ++r.nonexclusive_mouse_requests_;
            }
            return r.original<MouseCooperation>(mouse,13)(mouse,window,flags);
        });
    }
    static HRESULT WINAPI desktop_input_device(void* input,REFGUID guid,void** device,IUnknown* outer) {
        return guarded([&] {
            auto& r=self();GraphicsScope graphics(r);
            const auto result=r.original<DirectInputDevice>(input,3)(input,guid,device,outer);
            if (SUCCEEDED(result) && device && *device && outlast_desktop_mouse(guid)) {
                auto* retained=static_cast<IUnknown*>(*device);
                retained->AddRef();
                r.desktop_mouse_reference_.reset();
                *r.desktop_mouse_reference_.put()=retained;
                r.desktop_mouse_.store(*device,std::memory_order_release);
                r.com_hook(*device,13,&desktop_mouse_cooperation);
                r.hooks_->enable_pending();
            }
            return result;
        });
    }
    static HRESULT WINAPI desktop_direct_input(HINSTANCE module,DWORD version,REFIID iid,void** input,IUnknown* outer) {
        return guarded([&] {
            auto& r=self();GraphicsScope graphics(r);
            const auto result=r.original_direct_input_(module,version,iid,input,outer);
            if (SUCCEEDED(result) && input && *input) {
                r.com_hook(*input,3,&desktop_input_device);
                r.hooks_->enable_pending();
            }
            return result;
        });
    }
    static BOOL WINAPI desktop_set_cursor_pos(int x,int y) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        return guarded([&] {
            auto& r=self();
            if (r.desktop_cursor_->owns(caller)) ++r.suppressed_cursor_warps_;
            return r.desktop_cursor_->position(r.original_set_cursor_pos_,caller,x,y);
        });
    }
    static BOOL WINAPI desktop_get_cursor_pos(POINT* point) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        return guarded([&] {
            auto& r=self();
            if (headless_probe && r.desktop_cursor_->owns(caller))
                r.native_menu_cursor_queries_.fetch_add(1,std::memory_order_relaxed);
            const auto result=r.desktop_cursor_->get_position(r.original_get_cursor_pos_,caller,point);
            if (result && point && r.desktop_cursor_->owns(caller) && menu_dispatch_position_)
                *point=*menu_dispatch_position_;
            if (r.probe_menu_fixture_ && r.desktop_cursor_->owns(caller) && result && point &&
                r.probe_input_events_.is_open()) {
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                r.probe_input_events_<<"{\"kind\":\"menu_cursor_query\",\"clock_us\":"<<r.input_clock()
                    <<",\"caller_rva\":"<<caller-r.base_<<",\"screen\":["<<point->x<<','<<point->y<<"]}\n";
                r.probe_input_events_.flush();
                if (!r.probe_input_events_) throw std::runtime_error("Cannot retain native menu cursor queries");
            }
            return result;
        });
    }
    void controller_menu_pointer(bool panel,IDirect3DSurface9* output) {
        if (!desktop_cursor_) return;
        const auto motion=motion_controls_.sample(input_clock());
        std::optional<WorldPanelHit> hit;
        std::optional<EyePose> aim;
        const bool enabled=panel && motion.focused && panel_anchor_.placed();
        menu_pointer_hand_=menu_press_.select_hand(enabled,menu_pointer_hand_,
            {motion.hands[0].aim_tracked,motion.hands[1].aim_tracked},
            {motion.hands[0].trigger,motion.hands[1].trigger});
        if (enabled) {
            const auto& value=motion.hands[menu_pointer_hand_];
            if (value.aim_tracked) {
                const auto& p=value.aim;
                aim=relative_pose({{p.position.x,p.position.y,p.position.z},
                    {p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}});
                hit=panel_anchor_.intersect(*aim);
            }
        }
        const bool down=menu_press_.update(aim.has_value(),hit.has_value(),menu_pointer_hand_,
            motion.hands[menu_pointer_hand_].trigger);
        POINT point{-1,-1};
        RECT client{};
        if (panel || menu_pointer_down_) {
            if (!menu_window_ || !GetClientRect(menu_window_,&client) ||
                client.right<=0 || client.bottom<=0)
                throw std::runtime_error("Tracked menu pointer lost its owned client rectangle");
            if (hit)
                point=outlast_menu_source_point(hit->u,hit->v,width_,height_);
            if (!ClientToScreen(menu_window_,&point))
                throw std::runtime_error("Cannot map tracked menu pointer to native screen coordinates");
            desktop_cursor_->menu_pointer(panel && (aim.has_value() || menu_pointer_tracking_ || menu_pointer_down_),point);
        } else desktop_cursor_->menu_pointer(false);
        if (down!=menu_pointer_down_) {
            std::lock_guard<std::mutex> lock(menu_edge_mutex_);
            if (menu_edges_.size()>=256)
                throw std::runtime_error("Native input thread did not consume tracked menu edges");
            menu_edges_.push_back({point,down});
            menu_pointer_down_=down;
        }
        menu_pointer_tracking_=panel && aim.has_value();
        if (probe_menu_pointer_.is_open()) {
            POINT desktop{};
            if (!original_get_cursor_pos_(&desktop))
                throw std::runtime_error("Cannot observe actual desktop cursor during native menu diagnostics");
            probe_menu_pointer_<<std::setprecision(9)<<"{\"native_frame\":"<<stats_.frames
                <<",\"input_bridge_version\":2"
                <<",\"panel\":"<<(panel?"true":"false")<<",\"hand\":"<<menu_pointer_hand_
                <<",\"interactive_overlay\":"<<(menu_overlay_?"true":"false")
                <<",\"aim_tracked\":"<<(aim?"true":"false")<<",\"hit\":"<<(hit?"true":"false")
                <<",\"uv\":["<<(hit?hit->u:-1)<<','<<(hit?hit->v:-1)<<']'
                <<",\"trigger\":"<<motion.hands[menu_pointer_hand_].trigger
                <<",\"down\":"<<(down?"true":"false")
                <<",\"screen\":["<<point.x<<','<<point.y<<"],\"desktop\":["<<desktop.x<<','<<desktop.y<<']'
                <<",\"client_size\":["<<client.right<<','<<client.bottom<<']'
                <<",\"native_size\":["<<width_<<','<<height_<<']'
                <<",\"native_cursor_queries\":"<<native_menu_cursor_queries_.load(std::memory_order_relaxed)<<"}\n";
            probe_menu_pointer_.flush();
            if (!probe_menu_pointer_) throw std::runtime_error("Cannot retain native menu pointer diagnostics");
            if (probe_menu_fixture_ && panel && (stats_.frames==120 || stats_.frames==180)) {
                D3DSURFACE_DESC extent{};
                gpu_check(final_surface_->GetDesc(&extent),"Inspect native menu source");
                if ((extent.Format!=D3DFMT_A8R8G8B8 && extent.Format!=D3DFMT_X8R8G8B8) ||
                    extent.Width<width_ || extent.Height<height_)
                    throw std::runtime_error("Native menu source has an unsupported extent or format");
                Com<IDirect3DSurface9> readback;
                gpu_check(device_->CreateOffscreenPlainSurface(extent.Width,extent.Height,extent.Format,
                    D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create native menu source readback");
                gpu_check(device_->GetRenderTargetData(final_surface_.get(),readback.get()),"Read actual native menu source");
                D3DLOCKED_RECT locked{};
                gpu_check(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Lock native menu source");
                std::vector<char> pixels(960*540*3);
                for (unsigned y=0;y<540;++y) for (unsigned x=0;x<960;++x) {
                    const auto* value=static_cast<const unsigned char*>(locked.pBits)+
                        std::size_t(y)*height_/540*locked.Pitch+std::size_t(x)*width_/960*4;
                    for (unsigned channel=0;channel<3;++channel)
                        pixels[(y*960+x)*3+channel]=char(value[2-channel]);
                }
                gpu_check(readback->UnlockRect(),"Unlock native menu source");
                wchar_t folder[32768]{};
                const auto length=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder,UINT(std::size(folder)));
                if (!length || length>=std::size(folder))
                    throw std::runtime_error("Native menu source lacks its diagnostic evidence directory");
                std::ofstream file(std::filesystem::path(std::wstring(folder,length))/
                    (L"native-menu-source-"+std::to_wstring(stats_.frames)+L".ppm"),std::ios::binary);
                file<<"P6\n960 540\n255\n";file.write(pixels.data(),pixels.size());file.close();
                if (!file) throw std::runtime_error("Cannot retain actual native menu source");
            }
        }
        if (aim && output) {
            const TrackingFrame identity{{0,0,0},{1,0,0},{0,1,0},{0,0,1},1,{0,0,0}};
            StereoViewConstants views{};
            for (unsigned eye=0;eye<2;++eye)
                views[eye]=eye_constants(identity,tracked_[eye],fovs_[eye],.01f,1000);
            const auto end=hit?hit->point:add(aim->position,scale(rotate(aim->orientation,{0,0,-1}),2.5f));
            panel_renderer_->draw_pointer(*output,xr().width(),xr().height(),views,aim->position,end,hit.has_value());
            ++stats_.geometry_draws;
        }
    }
    static HRESULT WINAPI probe_mouse_data(void* device,DWORD stride,void* data,DWORD* count,DWORD flags) {
        return guarded([&] {
            auto& r=self();
            const auto owned=r.probe_mouse_.load(std::memory_order_acquire);
            const auto result=r.original_mouse_data_(device,stride,data,count,flags);
            if (device==owned && SUCCEEDED(result) && count) {
                r.suppressed_mouse_records_.fetch_add(*count,std::memory_order_relaxed);
                *count=0;
            }
            return result;
        });
    }
    static DWORD WINAPI gamepad_state(DWORD index,XINPUT_STATE* output) {
        return guarded([&] {
            auto& r=self();
            if (r.probe_startup_pad_) {
                if (!output) return DWORD(ERROR_BAD_ARGUMENTS);
                *output={};
                DWORD packet=0;BOOL pressed=FALSE;
                if (index) return DWORD(ERROR_DEVICE_NOT_CONNECTED);
                const bool active=r.probe_startup_pad_(&packet,&pressed)!=FALSE;
                const UINT state=(active?2u:0u)|(pressed?1u:0u);
                if (r.probe_startup_state_.exchange(state,std::memory_order_acq_rel)!=state) {
                    std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                    r.probe_input_events_<<"{\"kind\":\"startup_pad\",\"packet\":"<<packet
                        <<",\"active\":"<<(active?"true":"false")
                        <<",\"pressed\":"<<(pressed?"true":"false")<<"}\n";
                    r.probe_input_events_.flush();
                    if (!r.probe_input_events_) throw std::runtime_error("Cannot retain startup-only pad transition");
                }
                if (!active) return DWORD(ERROR_DEVICE_NOT_CONNECTED);
                output->dwPacketNumber=packet;
                output->Gamepad.wButtons=pressed?XINPUT_GAMEPAD_A:0;
                return DWORD(ERROR_SUCCESS);
            }
            XINPUT_STATE physical{};
            const auto result=r.original_gamepad_state_(index,output?&physical:nullptr);
            const auto clock=r.input_clock();
            const bool menu_pointer=index==0 && (r.native_interactive_menu_.load(std::memory_order_acquire) ||
                (r.desktop_cursor_ && r.desktop_cursor_->menu_position().has_value()));
            bool use_ready=false;
            if (index==0) {
                std::lock_guard<std::mutex> lock(r.skeleton_mutex_);
                const auto& body=r.body_frame_;
                // Native use animations still own held/released actions. Only the
                // temporary targeting override requires idle locomotion.
                const bool body_ready=body && clock>=body->clock && clock-body->clock<=250000 &&
                    body->motion.hands[r.config_.comfort.primary_hand()].aim_tracked;
                use_ready=outlast_native_use_action_ready(body_ready,r.native_use_context_,clock);
            }
            const auto status=r.motion_controls_.state(index,output,result,physical,clock,menu_pointer,use_ready,
                r.test_ || r.config_.physical_camcorder);
            if (index==0 && output && status==ERROR_SUCCESS) {
                const WORD actions=output->Gamepad.wButtons&
                    (XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_START);
                if (r.action_buttons_.exchange(actions,std::memory_order_acq_rel)!=actions)
                    log("NATIVE_GAMEPLAY_INPUT clock_us="+std::to_string(clock)+
                        "; actions="+std::to_string(actions));
            }
            if (headless_probe && index==0 && output && status==ERROR_SUCCESS) {
                const auto frame=r.motion_controls_.sample(clock);
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                if (!r.probe_input_events_.is_open()) return status;
                r.probe_input_events_<<"{\"kind\":\"motion_pad\",\"clock_us\":"<<clock
                    <<",\"xr_time\":"<<frame.time<<",\"focused\":"<<(frame.focused?"true":"false")
                    <<",\"left_grip_tracked\":"<<(frame.hands[0].grip_tracked?"true":"false")
                    <<",\"right_grip_tracked\":"<<(frame.hands[1].grip_tracked?"true":"false")
                    <<",\"right_aim_tracked\":"<<(frame.hands[1].aim_tracked?"true":"false")
                    <<",\"use_ready\":"<<(use_ready?"true":"false")
                    <<",\"physical_result\":"<<result<<",\"virtual_result\":"<<status
                    <<",\"buttons\":"<<output->Gamepad.wButtons<<",\"left_x\":"<<output->Gamepad.sThumbLX
                    <<",\"left_y\":"<<output->Gamepad.sThumbLY<<",\"right_x\":"<<output->Gamepad.sThumbRX<<"}\n";
                if (!r.probe_input_events_) throw std::runtime_error("Cannot retain native motion input evidence");
            }
            return status;
        });
    }
    static INT WINAPI viewport_focus(void* viewport) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        return guarded([&] {
            auto& r=self();
            const INT actual=r.original_viewport_focus_(viewport);
            const bool joystick=native_input_poll_ && caller==r.base_+0xc47aed;
            const auto clock=r.input_clock();
            const auto frame=joystick?r.motion_controls_.sample(clock):MotionFrame{};
            INT result=actual;
            if (!actual && viewport && joystick && frame.focused) {
                const auto window=field<HWND>(viewport,0xb4);
                DWORD process=0;
                if (GetWindowThreadProcessId(window,&process) && process==GetCurrentProcessId())
                    result=1;
            }
            if (headless_probe && joystick) {
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                if (r.probe_input_events_.is_open()) {
                    r.probe_input_events_<<"{\"kind\":\"joystick_focus\",\"clock_us\":"<<clock
                        <<",\"xr_time\":"<<frame.time<<",\"focused\":"<<(frame.focused?"true":"false")
                        <<",\"actual_focus\":"<<actual<<",\"result\":"<<result<<"}\n";
                    if (!r.probe_input_events_)
                        throw std::runtime_error("Cannot retain native joystick focus evidence");
                }
            }
            return result;
        });
    }
    static DWORD WINAPI gamepad_vibration(DWORD index,XINPUT_VIBRATION* value) {
        return guarded([&] {
            auto& r=self();
            const auto physical=r.original_gamepad_vibration_(index,value);
            const auto clock=r.input_clock();
            const auto result=r.motion_controls_.vibration(index,value,physical,clock);
            if (headless_probe && index==0 && value && result==ERROR_SUCCESS) {
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                if (r.probe_input_events_.is_open()) {
                    r.probe_input_events_<<"{\"kind\":\"native_rumble\",\"clock_us\":"<<clock
                        <<",\"low_frequency\":"<<value->wLeftMotorSpeed
                        <<",\"high_frequency\":"<<value->wRightMotorSpeed
                        <<",\"physical_result\":"<<physical<<",\"virtual_result\":"<<result<<"}\n";
                    if (!r.probe_input_events_) throw std::runtime_error("Cannot retain native rumble evidence");
                }
            }
            return result;
        });
    }
    void sample_native_camcorder_skeleton(void* hero,void* component,bool post_ik=false,
                                         const BodyIKTargets* applied=nullptr) {
        if (GetCurrentThreadId()!=input_thread_.load(std::memory_order_acquire))
            throw std::runtime_error("Native camera skeletal publication is not on the observed gameplay thread");
        std::optional<BodyFrame> request;
        {
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            request=body_frame_;
        }
        const auto now=input_clock();
        if (!request || request->native.hero!=reinterpret_cast<std::uintptr_t>(hero) ||
            now<request->clock || now-request->clock>250000) return;
        PhysicalCamera camera;
        {
            std::lock_guard<std::mutex> lock(camcorder_mutex_);
            camera=physical_camera_;
        }
        if (!camera.frame.visible || camera.generation!=camcorder_generation_) return;
        const auto tracking=native_body_tracking(hero,*request);
        sample_native_camcorder(camera,tracking);
        camera.clock=now;
        if (post_ik) {
            if (camera.frame.held && camera.native_mesh_ready)
                camcorder_capture_->update_pose(hero,camera.native_lens,camera.rotation,camera.night_vision,
                    camera.native_state==1);
            std::lock_guard<std::mutex> lock(camcorder_mutex_);
            physical_camera_=camera;
        }
        if (!camcorder_mesh_events_.is_open() || !camera.native_mesh_ready) return;
        const auto skeleton=outlast_skeleton(component);
        const auto parent=outlast_skeleton(camcorder_mesh_->parent());
        const auto bone=bone_index_(camcorder_mesh_->parent(),camcorder_mesh_->attachment_bone());
        if (bone<0) throw std::runtime_error("Published native camera attachment bone was lost");
        const auto attachment=skeleton_bone_matrix(parent,bone);
        const auto expected=camcorder_mesh_->world(parent);
        auto& out=camcorder_mesh_events_;
        auto vector=[&](Vec3 p) {out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';};
        out<<std::setprecision(9)<<"{\"frame\":"<<camcorder_capture_->frame()<<",\"clock\":"<<now
            <<",\"source\":\""<<(post_ik?"body_post_ik":"camera_evaluation")<<"\""
            <<",\"thread\":"<<GetCurrentThreadId()<<",\"hero\":"<<reinterpret_cast<std::uintptr_t>(hero)
            <<",\"component\":"<<camera.native_mesh_component<<",\"asset\":"<<camera.native_mesh_asset
            <<",\"asset_name\":\"handycam\",\"hand\":"<<camcorder_mesh_->hand()
            <<",\"native_body_serial\":"<<(post_ik?skeleton_observations_+1:0)
            <<",\"controller_tracked\":";
        const auto& targets=applied?*applied:request->targets;
        const auto hand=camcorder_mesh_->hand();
        const bool controller_tracked=hand>=0 && targets.tracked[hand];
        out<<(controller_tracked?"true":"false")
            <<",\"controller_orientation\":";
        if (controller_tracked) {
            const auto q=targets.controllers[hand].orientation;
            out<<'['<<q.x<<','<<q.y<<','<<q.z<<','<<q.w<<']';
        } else out<<"null";
        out
            <<",\"flags\":"<<field<UINT>(component,0x178)<<",\"parent\":"
            <<reinterpret_cast<std::uintptr_t>(camcorder_mesh_->parent())
            <<",\"bone\":"<<bone<<",\"attachment_world\":[";
        for (unsigned i=0;i<16;++i) out<<(i?",":"")<<attachment[i];
        out<<"],\"world\":[";
        for (unsigned i=0;i<16;++i) out<<(i?",":"")<<skeleton.world[i];
        out<<"],\"expected_world\":[";
        for (unsigned i=0;i<16;++i) out<<(i?",":"")<<expected[i];
        out<<"],\"stock_mount_version\":5,\"optical_tracking\":\"immutable_anatomical_grip_minus_Z_plus_Y\""
            <<",\"aim_orientation\":";
        if (hand>=0) {
            const auto q=camcorder_mesh_->held_pose().orientation;
            out<<'['<<q.x<<','<<q.y<<','<<q.z<<','<<q.w<<']';
        } else out<<"null";
        out<<",\"stock_original_local\":[";
        const auto original_local=camcorder_mesh_->stock_local();
        for (unsigned i=0;i<16;++i) out<<(i?",":"")<<original_local[i];
        for (const auto& item:std::initializer_list<std::pair<const char*,INT>>{
            {"right_wrist_world",36},{"right_socket_world",52},{"left_wrist_world",12}}) {
            const auto native=skeleton_bone_matrix(parent,item.second);
            out<<"],\""<<item.first<<"\":[";
            for (unsigned i=0;i<16;++i) out<<(i?",":"")<<native[i];
        }
        out<<"],\"model_grip_point\":";vector(camcorder_mesh_->mesh_grip_point());
        out<<",\"model_grip_position\":";
        vector(add(skeleton_vector(skeleton.world,camcorder_mesh_->mesh_grip_point()),
            {skeleton.world[12],skeleton.world[13],skeleton.world[14]}));
        out<<",\"palm_position\":";
        if (camcorder_mesh_->hand()>=0) {
            UINT64 name=0;
            bone_name_(&name,camcorder_mesh_->hand()?L"Hero-R-Hand":L"Hero-L-Hand",1);
            const auto wrist=bone_index_(parent.component,name);
            if (wrist<0) throw std::runtime_error("Camera diagnostics lost the native holding wrist");
            const auto anchor=outlast_grip_anchor(parent,wrist);
            const auto palm=skeleton_bone_matrix(parent,wrist);
            vector(add(skeleton_vector(palm,anchor.center),{palm[12],palm[13],palm[14]}));
        } else out<<"null";
        out<<",\"atoms\":[";
        for (unsigned i=0;i<2;++i) {
            const auto& atom=skeleton.atoms[i];
            out<<(i?",[":"[")<<atom.rotation.x<<','<<atom.rotation.y<<','<<atom.rotation.z<<','
                <<atom.rotation.w<<','<<atom.position.x<<','<<atom.position.y<<','<<atom.position.z<<','<<atom.scale<<']';
        }
        out<<"],\"lens\":";vector(camera.native_lens);
        out<<",\"screen_half_extents\":";vector(camera.frame.screen_half_extents);
        out<<",\"screen_position\":";vector(camera.frame.screen.position);
        const auto q=camera.frame.screen.orientation;
        out<<",\"screen_orientation\":["<<q.x<<','<<q.y<<','<<q.z<<','<<q.w
            <<"],\"tracking_origin\":";vector(tracking.origin);
        out<<",\"tracking_right\":";vector(tracking.right);
        out<<",\"tracking_up\":";vector(tracking.up);
        out<<",\"tracking_backward\":";vector(tracking.backward);
        out<<",\"units_per_meter\":"<<tracking.units_per_meter<<"}\n";
        out.flush();
        if (!out) throw std::runtime_error("Cannot retain actual retail camera skeletal placement");
    }
    static void WINAPI skeleton_evaluate(void* component,UINT flags,UINT secondary) {
        const auto callsite=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        guarded([&] {
            auto& r=self();
            DiagnosticScope diagnostic(r.skeleton_phase_,1);
            r.skeleton_heartbeat_.store(GetTickCount64(),std::memory_order_release);
            const auto owner=r.hero_cast_(field<void*>(component,0x78));
            const bool owned=owner && component==field<void*>(owner,0x444) &&
                field<void*>(component,0x238) && !field<void*>(component,0x33c);
            bool restored=false;
            if (owned) {
                std::lock_guard<std::mutex> lock(r.skeleton_mutex_);
                restored=r.body_ik_.restore(component);
            }
            r.original_skeleton_evaluate_(component,flags,secondary);
            if (r.camcorder_mesh_ && r.camcorder_mesh_->owns(owner,component)) {
                r.camcorder_mesh_->update_transform(outlast_skeleton(r.camcorder_mesh_->parent()));
                r.sample_native_camcorder_skeleton(owner,component);
                return;
            }
            if (!owned || !field<void*>(component,0x238)) return;
            std::unique_lock<std::mutex> lock(r.skeleton_mutex_);
            // Recenter can publish a new generation during native evaluation.
            if (r.body_generation_!=r.calibrated_generation_) {
                r.body_ik_.reset();r.calibrated_generation_=r.body_generation_;
            }
            const auto now=r.input_clock();
            const bool retain=r.probe_skeleton_.is_open() &&
                (r.probe_skeleton_all_ || r.skeleton_observations_<60 || (r.skeleton_observations_+1)%30==0);
            std::vector<BoneAtom> before;
            std::optional<BodyIKTargets> applied_targets;
            std::optional<BodyIKResult> solved;
            INT camcorder_hand=-1;
            const auto body_request_clock=r.body_frame_?std::optional<UINT64>(r.body_frame_->clock):std::nullopt;
            const bool body_current=r.body_frame_ &&
                r.body_frame_->native.hero==reinterpret_cast<std::uintptr_t>(owner) &&
                now>=r.body_frame_->clock && now-r.body_frame_->clock<=250000;
            const auto motion=r.motion_controls_.sample(now);
            const bool motion_focused=body_current && motion.focused;
            if (motion_focused) {
                const auto basic=field<BYTE>(owner,0x7bc),special=field<BYTE>(owner,0x7bd);
                auto targets=r.body_frame_->targets;
                for (unsigned hand=0;hand<2;++hand)
                    targets.tracked[hand]=targets.tracked[hand] && motion.hands[hand].grip_tracked;
                if (outlast_tracked_locomotion(basic,special) &&
                    (special==0 || (special<=2 && targets.compensate_native_crouch) ||
                     outlast_airborne_locomotion(basic,special))) {
                    targets.native_crouched=(field<UINT>(owner,0x288)&0x10)!=0;
                    targets.stance_transition=special!=0;
                    auto skeleton=outlast_skeleton(component);
                    const auto root=field<Vec3>(owner,0x80);
                    const auto yaw=field<std::array<INT,3>>(owner,0x8c)[1];
                    const double angle=(double(yaw)-r.body_frame_->native.yaw)*(6.2831853071795864769/65536);
                    auto turn=[&](Vec3 value) {
                        return Vec3{float(value.x*std::cos(angle)-value.y*std::sin(angle)),
                            float(value.x*std::sin(angle)+value.y*std::cos(angle)),value.z};
                    };
                    auto& frame=targets.tracking;
                    frame.origin=add(root,turn(add(frame.origin,scale(r.body_frame_->native.root,-1))));
                    frame.right=turn(frame.right);frame.up=turn(frame.up);frame.backward=turn(frame.backward);
                    for (unsigned hand=0;hand<2;++hand)
                        targets.raw_hand_positions[hand]=targets.hands[hand].position;
                    if (r.collision_frame_ && r.collision_frame_->generation==r.body_generation_ &&
                        r.collision_frame_->hero==reinterpret_cast<std::uintptr_t>(owner) &&
                        now>=r.collision_frame_->clock && now-r.collision_frame_->clock<=100000) {
                        const auto movement=add(root,scale(r.collision_frame_->root,-1));
                        if (dot(movement,movement)<=40000)
                            for (unsigned hand=0;hand<2;++hand)
                                if (targets.tracked[hand] && r.collision_frame_->tracked[hand] &&
                                    r.collision_frame_->hands[hand].blocked) {
                                    const auto relative=scale(add(r.collision_frame_->hands[hand].position,
                                        scale(frame.origin,-1)),1/frame.units_per_meter);
                                    targets.hands[hand].position={dot(relative,frame.right),
                                        dot(relative,frame.up),dot(relative,frame.backward)};
                                    targets.world_contact[hand]=true;
                                }
                    }
                    if (!r.body_ik_.owns(skeleton) && basic==0 && !special && !r.body_frame_->native.pawn_crouched) {
                        std::array<INT,20> bones{};
                        for (std::size_t i=0;i<bones.size();++i) {
                            UINT64 name=0;r.bone_name_(&name,outlast_rig_names[i],1);
                            bones[i]=r.bone_index_(component,name);
                        }
                        std::array<INT,16> skin_bones{};
                        for (std::size_t i=0;i<skin_bones.size();++i) {
                            UINT64 name=0;r.bone_name_(&name,outlast_skinning_names[i],1);
                            skin_bones[i]=r.bone_index_(component,name);
                        }
                        r.body_ik_.calibrate(skeleton,bones,targets,skin_bones);
                        log("Native full-body rig calibrated from actual hero bone indices; component-space IK enabled");
                    }
                    if (r.body_ik_.owns(skeleton)) {
                        if (r.camcorder_mesh_ && r.camcorder_mesh_->parent()==component &&
                            r.camcorder_mesh_->hand()>=0 && targets.tracked[r.camcorder_mesh_->hand()]) {
                            camcorder_hand=r.camcorder_mesh_->hand();
                            targets.mirror_camera_grip=camcorder_hand==0;
                            targets.controller_camera_grip[camcorder_hand]=true;
                            r.camcorder_mesh_->update_held_tracking(frame,r.body_frame_->motion);
                        }
                        if (retain) {before=skeleton.atoms;applied_targets=targets;}
                        solved=r.body_ik_.apply(skeleton,targets);
                        for (unsigned side=0;side<2;++side) {
                            const bool limited=targets.tracked[side] && solved->hand_target_error[side]>.03f;
                            if (limited && !r.hand_reach_warned_[side])
                                log("Tracked hand exceeds bounded shoulder/arm reach; side="+std::to_string(side)+
                                    "; controller_error_units="+std::to_string(solved->hand_target_error[side]));
                            r.hand_reach_warned_[side]=limited;
                        }
                        std::memcpy(skeleton.destination,skeleton.atoms.data(),skeleton.atoms.size()*sizeof(BoneAtom));
                    }
                }
            }
            if (r.probe_draw_contact_ || r.probe_draw_jump_) {
                const bool tracked=solved && solved->solved_hands==2 && applied_targets &&
                    applied_targets->tracked[0] && applied_targets->tracked[1];
                const bool valid=tracked && applied_targets->compensate_native_crouch;
                const bool contact_valid=valid || (r.recovery_draw_probe_ && tracked);
                auto stability=[&](std::atomic<UINT64>& since,bool ready) {
                    if (!ready) since.store(0,std::memory_order_release);
                    else if (!since.load(std::memory_order_acquire)) since.store(now,std::memory_order_release);
                };
                stability(r.probe_body_ready_since_us_,r.probe_draw_jump_?
                    native_draw_jump_ready(field<BYTE>(owner,0x7bc),field<BYTE>(owner,0x7bd),tracked):
                    valid && applied_targets->head.position.y<-.25f &&
                        applied_targets->native_crouched && !applied_targets->stance_transition);
                stability(r.probe_contact_ready_since_us_,contact_valid &&
                    (applied_targets->world_contact[0] || applied_targets->world_contact[1]));
                if (r.recovery_draw_probe_)
                    stability(r.probe_recovery_ready_since_us_,tracked &&
                        r.probe_recovery_hand_mask_.load(std::memory_order_acquire)==3 &&
                        !applied_targets->world_contact[0] && !applied_targets->world_contact[1]);
            }
            if (solved && r.camcorder_mesh_ && r.camcorder_mesh_->parent()==component) {
                const auto updated=outlast_skeleton(component);
                lock.unlock();
                r.camcorder_mesh_->update_transform(updated);
                r.sample_native_camcorder_skeleton(owner,r.camcorder_mesh_->component(),true,
                    applied_targets?&*applied_targets:nullptr);
                lock.lock();
            }
            if (!r.probe_skeleton_.is_open()) return;
            const auto serial=++r.skeleton_observations_;
            if (!r.probe_skeleton_all_ && serial>60 && serial%30!=0) return;
            const auto skeleton=outlast_skeleton(component);
            auto& output=r.probe_skeleton_;
            output<<std::setprecision(9)<<"{\"serial\":"<<serial<<",\"clock_us\":"<<r.input_clock()
                <<",\"body_request_schema_version\":1,\"ik_clock_us\":"<<now
                <<",\"body_request_clock_us\":";
            if (body_request_clock) output<<*body_request_clock;
            else output<<"null";
            output<<",\"body_request_age_us\":";
            if (body_request_clock && now>=*body_request_clock) output<<now-*body_request_clock;
            else output<<"null";
            output<<",\"body_request_current\":"<<(body_current?"true":"false")
                <<",\"motion_focused_at_ik\":"<<(body_current?(motion_focused?"true":"false"):"null")
                <<",\"thread\":"<<GetCurrentThreadId()<<",\"component\":"<<reinterpret_cast<std::uintptr_t>(component)
                <<",\"asset\":"<<reinterpret_cast<std::uintptr_t>(skeleton.asset)
                <<",\"hero\":"<<reinterpret_cast<std::uintptr_t>(owner)
                <<",\"basic\":"<<unsigned(field<BYTE>(owner,0x7bc))
                <<",\"special\":"<<unsigned(field<BYTE>(owner,0x7bd))
                <<",\"pawn_crouched\":"<<((field<UINT>(owner,0x288)&0x10)?"true":"false")
                <<",\"callsite_rva\":"<<(callsite-r.base_)
                <<",\"pose_restored\":"<<(restored?"true":"false")
                <<",\"ik_hands\":"<<(solved?solved->solved_hands:0)
                <<",\"root\":[";
            const auto root=field<Vec3>(owner,0x80);
            output<<root.x<<','<<root.y<<','<<root.z<<"],\"world\":[";
            for (unsigned i=0;i<16;++i) output<<(i?",":"")<<skeleton.world[i];
            output<<"],\"parents\":[";
            for (std::size_t i=0;i<skeleton.parents.size();++i) output<<(i?",":"")<<skeleton.parents[i];
            output<<"],\"atoms\":[";
            for (std::size_t i=0;i<skeleton.atoms.size();++i) {
                const auto& atom=skeleton.atoms[i];
                output<<(i?",":"")<<'['<<atom.rotation.x<<','<<atom.rotation.y<<','<<atom.rotation.z
                    <<','<<atom.rotation.w<<','<<atom.position.x<<','<<atom.position.y<<','<<atom.position.z
                    <<','<<atom.scale<<']';
            }
            output<<"],\"reference_records\":";
            if (skeleton.asset!=r.probe_reference_asset_) {
                const auto* reference=skeleton_field<const unsigned char*>(skeleton.asset,0xc4);
                output<<'[';
                for (std::size_t bone=0;bone<skeleton.atoms.size();++bone) {
                    output<<(bone?",":"")<<'[';
                    for (unsigned word=0;word<20;++word)
                        output<<(word?",":"")<<skeleton_field<UINT>(reference,bone*0x50+word*4);
                    output<<']';
                }
                output<<']';r.probe_reference_asset_=skeleton.asset;
            } else output<<"null";
            output<<",\"before_atoms\":[";
            for (std::size_t i=0;i<before.size();++i) {
                const auto& atom=before[i];
                output<<(i?",":"")<<'['<<atom.rotation.x<<','<<atom.rotation.y<<','<<atom.rotation.z
                    <<','<<atom.rotation.w<<','<<atom.position.x<<','<<atom.position.y<<','<<atom.position.z
                    <<','<<atom.scale<<']';
            }
            output<<"],\"targets\":";
            if (applied_targets) {
                const auto& target=*applied_targets;
                auto vector=[&](Vec3 v){output<<'['<<v.x<<','<<v.y<<','<<v.z<<']';};
                output<<"{\"physical\":"<<(target.compensate_native_crouch?"true":"false")
                    <<",\"palm_axes_version\":2"
                    <<",\"reference_upper_body\":true"
                    <<",\"grip_anchored\":"<<(target.grip_anchored?"true":"false")
                    <<",\"camera_hand\":"<<camcorder_hand
                    <<",\"mirrored_camera_grip\":"<<(target.mirror_camera_grip?"true":"false")
                    <<",\"controller_fingers\":"<<(target.controller_fingers?"true":"false")
                    <<",\"hand_wristward_m\":"<<target.hand_wristward_m
                    <<",\"standing_height_delta\":"<<target.standing_height_delta
                    <<",\"head_position\":";
                vector(target.head.position);
                output<<",\"origin\":";vector(target.tracking.origin);
                output<<",\"right\":";vector(target.tracking.right);
                output<<",\"up\":";vector(target.tracking.up);
                output<<",\"backward\":";vector(target.tracking.backward);
                output<<",\"units_per_meter\":"<<target.tracking.units_per_meter<<",\"hands\":[";
                for (unsigned side=0;side<2;++side) {
                    const auto& hand=target.hands[side];
                    output<<(side?",":"")<<"{\"tracked\":"<<(target.tracked[side]?"true":"false")
                        <<",\"finger_trigger\":"<<target.finger_trigger[side]
                        <<",\"finger_squeeze\":"<<target.finger_squeeze[side]
                        <<",\"camera_grip\":"<<(target.controller_camera_grip[side]?"true":"false")
                        <<",\"world_contact\":"<<(target.world_contact[side]?"true":"false")
                        <<",\"controller_position\":";
                    vector(target.controllers[side].position);
                    const auto controller_rotation=target.controllers[side].orientation;
                    output<<",\"controller_orientation\":["<<controller_rotation.x<<','<<controller_rotation.y
                        <<','<<controller_rotation.z<<','<<controller_rotation.w<<']';
                    output<<",\"raw_position\":";
                    vector(target.raw_hand_positions[side]);
                    output<<",\"position\":";
                    vector(hand.position);
                    output<<",\"orientation\":["<<hand.orientation.x<<','<<hand.orientation.y<<','
                        <<hand.orientation.z<<','<<hand.orientation.w<<"],\"solved_component\":";
                    vector(solved->hands[side]);
                    if (target.grip_anchored) {
                        const auto& anchor=r.body_ik_.grip_anchor(side);
                        output<<",\"palm_center_local\":";vector(anchor.center);
                        output<<",\"palm_x_local\":";vector(anchor.x);
                        output<<",\"palm_z_local\":";vector(anchor.z);
                    }
                    const auto raw_world=add(target.tracking.origin,scale(map_tracking(target.tracking,
                        target.raw_hand_positions[side]),target.tracking.units_per_meter));
                    const auto raw_component=bone_world_to_component(skeleton,raw_world);
                    const auto raw_error=add(r.body_ik_.hand_anchor(skeleton,side,target.grip_anchored),
                        scale(raw_component,-1));
                    const auto controller_world=add(target.tracking.origin,scale(map_tracking(target.tracking,
                        target.controllers[side].position),target.tracking.units_per_meter));
                    const auto controller_error=add(r.body_ik_.hand_anchor(skeleton,side,target.grip_anchored),
                        scale(bone_world_to_component(skeleton,controller_world),-1));
                    output<<",\"controller_error_units\":"<<std::sqrt(dot(raw_error,raw_error))
                        <<",\"uncalibrated_controller_error_units\":"<<std::sqrt(dot(controller_error,controller_error))
                        <<",\"contact_target_error_units\":"<<solved->hand_target_error[side]
                        <<",\"shoulder_adjustment_units\":"<<solved->shoulder_adjustment[side]<<'}';
                }
                output<<"]}";
            } else output<<"null";
            output<<",\"bindings\":[";
            for (std::size_t i=0;i<std::size(outlast_rig_names)+std::size(outlast_skinning_names);++i) {
                const auto* rig_name=i<std::size(outlast_rig_names)?outlast_rig_names[i]:
                    outlast_skinning_names[i-std::size(outlast_rig_names)];
                UINT64 name=0;r.bone_name_(&name,rig_name,1);
                const auto index=r.bone_index_(component,name);
                output<<(i?",":"")<<"{\"name\":\"";
                for (const auto* c=rig_name;*c;++c) output<<char(*c);
                output<<"\",\"index\":"<<index;
                if (index>=0 && std::size_t(index)<skeleton.atoms.size()) {
                    alignas(16) std::array<float,16> actual{};
                    r.bone_matrix_(component,actual.data(),index);
                    for (auto value:actual)
                        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite native bone matrix observation");
                    const auto expected=skeleton_bone_matrix(skeleton,index);
                    float error=0;
                    for (unsigned j=0;j<16;++j) error=std::max(error,std::abs(actual[j]-expected[j]));
                    output<<",\"matrix_error\":"<<error<<",\"native_matrix\":[";
                    for (unsigned j=0;j<16;++j) output<<(j?",":"")<<actual[j];
                    output<<']';
                }
                output<<'}';
            }
            output<<"]}\n";output.flush();
            if (!output) throw std::runtime_error("Cannot retain native skeletal ownership evidence");
        });
    }
    void refresh_native_menu_state() {
        if (test_) return;
        const auto* engine=field<const void*>(reinterpret_cast<const void*>(base_),0x201b8e0);
        const auto count=engine?field<INT>(engine,0x44):0;
        if (count<0 || count>16 || (count && !field<const void*>(engine,0x3c)))
            throw std::runtime_error("Invalid native focused movie inventory");
        if (!menu_movie_) throw std::logic_error("Native focused movie entry is unavailable");
        const auto* record=engine?menu_movie_(engine,0):nullptr;
        const auto* owner=record?field<const void*>(record,0x70):nullptr;
        const bool interactive=owner && field<const void*>(record,0x3c) &&
            outlast_interactive_movie(field<INT>(record,0x58),field<INT>(record,0x68),
                field<UINT>(owner,0x114));
        native_interactive_menu_.store(interactive,std::memory_order_release);
    }
    void native_menu_motion(float elapsed) {
        if (!desktop_cursor_) return;
        const auto point=desktop_cursor_->menu_position();
        std::vector<MenuEdge> edges;
        {
            std::lock_guard<std::mutex> lock(menu_edge_mutex_);
            edges.swap(menu_edges_);
        }
        if (!point && edges.empty()) {menu_native_position_.reset();return;}
        if (edges.empty() && menu_native_position_ && point &&
            menu_native_position_->x==point->x && menu_native_position_->y==point->y) return;
        const auto* image=reinterpret_cast<const void*>(base_);
        const auto count=field<INT>(image,0x201e378);
        const auto viewports=field<void**>(image,0x201e370);
        if (count<0 || count>16 || (count && !viewports))
            throw std::runtime_error("Invalid native menu viewport inventory");
        void* selected=nullptr;
        for (INT i=0;i<count;++i)
            if (viewports[i] && field<HWND>(viewports[i],0xbc)==menu_window_) {
                if (selected) throw std::runtime_error("Tracked menu has multiple native viewport owners");
                selected=viewports[i];
            }
        if (!selected) throw std::runtime_error("Tracked menu lost its native viewport owner");
        auto* client=field<void*>(selected,0x4c);
        if (!client) throw std::runtime_error("Tracked menu lost its native input client");
        const auto axis=entry<MenuAxisEntry>(client,7);
        const auto key=entry<MenuKeyEntry>(client,5);
        if (reinterpret_cast<std::uintptr_t>(axis)!=base_+0x61f840 ||
            reinterpret_cast<std::uintptr_t>(key)!=base_+0xcc76e0)
            throw std::runtime_error("Tracked menu has an unexpected retail input dispatcher");
        const auto* engine=probe_menu_fixture_?field<const void*>(image,0x201b8e0):nullptr;
        const auto dispatch=[&](POINT position,std::optional<bool> down) {
            struct PositionScope {
                std::optional<POINT> previous=menu_dispatch_position_;
                explicit PositionScope(POINT value) {menu_dispatch_position_=value;}
                ~PositionScope() {menu_dispatch_position_=previous;}
            } position_scope(position);
            const auto before=engine?field<POINT>(engine,0x208):POINT{-1,-1};
            // Bypass the Win32 mouse path, whose SetFocus can reset a queued release.
            axis(client,static_cast<BYTE*>(selected)+8,0,field<UINT64>(image,0x20086d8),0,elapsed,0);
            menu_native_position_=position;
            const auto after=engine?field<POINT>(engine,0x208):POINT{-1,-1};
            if (probe_menu_fixture_) {
                std::lock_guard<std::mutex> lock(probe_input_mutex_);
                probe_input_events_<<"{\"kind\":\"menu_native_motion\",\"clock_us\":"<<input_clock()
                    <<",\"screen\":["<<position.x<<','<<position.y<<"],\"cache_before\":["<<before.x<<','<<before.y
                    <<"],\"cache_after\":["<<after.x<<','<<after.y<<"],\"engine_present\":"
                    <<(engine?"true":"false")<<"}\n";probe_input_events_.flush();
                if (!probe_input_events_) throw std::runtime_error("Cannot retain native GFx pointer refresh");
            }
            if (down) {
                const INT event=*down?0:1;
                const auto handled=key(client,static_cast<BYTE*>(selected)+8,0,
                    field<UINT64>(image,0x20086f8),event,1.f,0);
                if (!handled) log("Native controller menu edge was not consumed; event="+std::to_string(event));
                if (probe_menu_fixture_) {
                    const auto cursor=engine?field<POINT>(engine,0x208):POINT{-1,-1};
                    std::lock_guard<std::mutex> lock(probe_input_mutex_);
                    probe_input_events_<<"{\"kind\":\"menu_native_edge\",\"clock_us\":"<<input_clock()
                        <<",\"event\":"<<event<<",\"handled\":"<<handled
                        <<",\"cursor\":["<<cursor.x<<','<<cursor.y<<"]}\n";probe_input_events_.flush();
                    if (!probe_input_events_) throw std::runtime_error("Cannot retain native menu edge dispatch");
                }
            }
        };
        for (const auto& edge:edges) dispatch(edge.position,edge.down);
        if (point && (edges.empty() || point->x!=edges.back().position.x || point->y!=edges.back().position.y))
            dispatch(*point,std::nullopt);
    }
    static void WINAPI probe_input_tick(void* client,float elapsed) {
        guarded([&] {
            auto& r=self();
            r.input_heartbeat_.store(GetTickCount64(),std::memory_order_release);
            r.input_thread_.store(GetCurrentThreadId(),std::memory_order_release);
            struct PollScope {
                bool previous=native_input_poll_;
                PollScope() {native_input_poll_=true;}
                ~PollScope() {native_input_poll_=previous;}
            } poll_scope;
            r.refresh_native_menu_state();
            if (r.motion_enabled_ || r.probe_startup_pad_) {
                if (!r.gamepads_ || !r.gamepad_count_)
                    throw std::logic_error("VR native input has no validated controller inventory");
                outlast_virtual_pad_presence(*r.gamepads_,*r.gamepad_count_);
            }
            if (r.original_focus_pause_ && (!headless_probe ||
                r.probe_focus_policy_==ProbeFocusPolicy::Tracked)) {
                std::lock_guard<std::recursive_mutex> lock(r.focus_pause_mutex_);
                const bool focused=r.motion_controls_.sample(r.input_clock()).focused;
                if (const auto change=r.focus_pause_.refresh(focused))
                    r.apply_focus_pause(*change,focused,"xr_focus");
            }
            r.native_menu_motion(elapsed);
            if (!headless_probe) {
                r.original_input_tick_(client,elapsed);
                return;
            }
            if (r.probe_desktop_unfocused_ && r.motion_controls_.sample(r.input_clock()).focused) {
                DWORD process=0;
                const auto window=GetFocus();
                if (window && GetWindowThreadProcessId(window,&process) && process==GetCurrentProcessId()) {
                    SetFocus(nullptr);
                    if (GetFocus()==window)
                        throw std::runtime_error("Cannot clear the probe-owned window focus for the explicit negative-focus fixture");
                }
            }
            const auto mouse=field<void*>(reinterpret_cast<const void*>(r.base_),0x201e218);
            if (mouse && !r.probe_mouse_) {
                GraphicsScope graphics(r);
                r.hooks_->add(entry<MouseDataEntry>(mouse,10),&probe_mouse_data,r.original_mouse_data_);
                r.probe_mouse_.store(mouse,std::memory_order_release);
                r.hooks_->enable_pending();
            } else if (mouse && mouse!=r.probe_mouse_) {
                throw std::runtime_error("Headless input isolation requires the original owned mouse device");
            }
            r.original_input_tick_(client,elapsed);
            LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);
            std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
            if (!r.probe_input_events_.is_open()) return;
            r.probe_input_events_<<"{\"kind\":\"input_tick\",\"clock\":"<<clock.QuadPart
                <<",\"elapsed\":"<<elapsed<<",\"mouse_attached\":"<<(r.probe_mouse_?"true":"false")
                <<",\"suppressed_mouse_records\":"<<r.suppressed_mouse_records_.load(std::memory_order_relaxed)
                <<"}\n";
            r.probe_input_events_.flush();
            if (!r.probe_input_events_) throw std::runtime_error("Native probe input evidence write failed");
        });
    }
    static void WINAPI probe_input_enqueue(void* client,void* viewport,UINT message,WPARAM key,LPARAM bits) {
        guarded([&] {
            auto& r=self();
            const bool marked=(key&native_probe_mouse_marker)!=0;
            const bool accepted=r.probe_keys_.accept_message(message,key,bits);
            if (accepted)
                r.original_input_enqueue_(client,viewport,message,key,bits);
            if (r.probe_menu_fixture_ && message>=WM_MOUSEMOVE && message<=WM_LBUTTONUP &&
                r.probe_input_events_.is_open()) {
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                r.probe_input_events_<<"{\"kind\":\"menu_mouse_message\",\"clock_us\":"<<r.input_clock()
                    <<",\"message\":"<<message<<",\"marked\":"<<(marked?"true":"false")
                    <<",\"accepted\":"<<(accepted?"true":"false")<<"}\n";
                r.probe_input_events_.flush();
                if (!r.probe_input_events_) throw std::runtime_error("Cannot retain native menu event acceptance");
            }
        });
    }
    static void WINAPI probe_menu_consume(void* viewport,const void* queued) {
        guarded([&] {
            auto& r=self();
            const auto message=field<UINT>(queued,8);
            if (message>=WM_MOUSEMOVE && message<=WM_LBUTTONUP) {
                using Getter=INT(WINAPI*)(void*);
                auto* cursor=static_cast<BYTE*>(viewport)+8;
                const auto x=entry<Getter>(cursor,18)(cursor),y=entry<Getter>(cursor,19)(cursor);
                const auto width=entry<Getter>(cursor,2)(cursor),height=entry<Getter>(cursor,3)(cursor);
                auto* client=field<void*>(viewport,0x4c);
                const auto callback_rva=[&](unsigned index) ->std::uintptr_t {
                    if (!client) return 0;
                    const auto address=reinterpret_cast<std::uintptr_t>(entry<void*>(client,index));
                    if (address<r.base_ || address-r.base_>=r.module_extent_)
                        throw std::runtime_error("Native menu callback lies outside the verified retail image");
                    return address-r.base_;
                };
                const auto key=callback_rva(5),move=callback_rva(18),capture=callback_rva(19),
                    touch=callback_rva(10);
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                r.probe_input_events_<<"{\"kind\":\"menu_native_consumer\",\"clock_us\":"<<r.input_clock()
                    <<",\"message\":"<<message<<",\"cursor\":["<<x<<','<<y<<']'
                    <<",\"viewport_size\":["<<width<<','<<height<<']'
                    <<",\"touch_emulation\":"<<field<INT>(viewport,0x54c)
                    <<",\"touch_down\":"<<field<INT>(viewport,0x550)
                    <<",\"mouse_captured\":"<<field<INT>(viewport,0x4f4)
                    <<",\"foreground\":"<<(GetForegroundWindow()==field<HWND>(viewport,0xbc)?"true":"false")
                    <<",\"client_present\":"<<(client?"true":"false")
                    <<",\"input_key_rva\":"<<key<<",\"mouse_move_rva\":"<<move
                    <<",\"captured_move_rva\":"<<capture<<",\"input_touch_rva\":"<<touch<<"}\n";
                r.probe_input_events_.flush();
                if (!r.probe_input_events_) throw std::runtime_error("Cannot retain actual native menu consumption");
            }
            r.original_menu_consume_(viewport,queued);
        });
    }
    static INT WINAPI probe_menu_key(void* client,void* viewport,INT controller,UINT64 key,
                                    INT event,float amount,INT gamepad) {
        return guarded([&] {
            auto& r=self();
            const bool left=key==field<UINT64>(reinterpret_cast<const void*>(r.base_),0x20086f8);
            std::uintptr_t global_key=0,global_gate=0;
            std::vector<std::uintptr_t> interactions;
            if (left) {
                const auto callback_rva=[&](void* object,unsigned index) ->std::uintptr_t {
                    const auto address=reinterpret_cast<std::uintptr_t>(entry<void*>(object,index));
                    if (address<r.base_ || address-r.base_>=r.module_extent_)
                        throw std::runtime_error("Native interaction callback lies outside the verified retail image");
                    return address-r.base_;
                };
                if (auto* global=field<BYTE*>(reinterpret_cast<const void*>(r.base_),0x201ebc0)) {
                    global_key=callback_rva(global+8,5);
                    global_gate=callback_rva(global,8);
                }
                const auto count=field<INT>(client,0x28);
                const auto values=field<void**>(client,0x20);
                if (count<0 || count>64 || (count && !values))
                    throw std::runtime_error("Invalid observed native menu interaction inventory");
                for (INT i=0;i<count;++i)
                    interactions.push_back(values[i]?callback_rva(values[i],78):0);
            }
            const auto result=r.original_menu_key_(client,viewport,controller,key,event,amount,gamepad);
            if (left) {
                std::lock_guard<std::mutex> lock(r.probe_input_mutex_);
                r.probe_input_events_<<"{\"kind\":\"menu_native_key\",\"clock_us\":"<<r.input_clock()
                    <<",\"event\":"<<event<<",\"controller\":"<<controller<<",\"gamepad\":"<<gamepad
                    <<",\"handled\":"<<result<<",\"global_key_rva\":"<<global_key
                    <<",\"global_gate_rva\":"<<global_gate<<",\"interaction_key_rvas\":[";
                for (std::size_t i=0;i<interactions.size();++i)
                    r.probe_input_events_<<(i?",":"")<<interactions[i];
                r.probe_input_events_<<"]}\n";r.probe_input_events_.flush();
                if (!r.probe_input_events_) throw std::runtime_error("Cannot retain actual native menu key routing");
            }
            return result;
        });
    }
    static SHORT WINAPI probe_key_state(int key) {
        const auto caller=__builtin_return_address(0);
        return guarded([&] {
            auto& r=self();
            if (r.game_caller(caller) && key>=0 && key<256)
                return SHORT(r.probe_keys_.down(UINT(key))?0x8000:0);
            return r.original_key_state_(key);
        });
    }
    void apply_focus_pause(const FocusPauseChange& change,bool focused,const char* source) {
        if (change.invoke) original_focus_pause_(change.engine,change.applied);
        if (!headless_probe) return;
        LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);
        std::lock_guard<std::mutex> lock(probe_input_mutex_);
        if (!probe_input_events_.is_open()) return;
        probe_input_events_<<"{\"kind\":\"focus_pause_policy\",\"clock\":"<<clock.QuadPart
            <<",\"clock_us\":"<<input_clock()<<",\"requested\":"<<change.requested
            <<",\"applied\":"<<change.applied<<",\"xr_focused\":"<<(focused?"true":"false")
            <<",\"called\":"<<(change.invoke?"true":"false")
            <<",\"source\":\""<<source<<"\",\"engine\":"
            <<reinterpret_cast<std::uintptr_t>(change.engine)
            <<",\"native_pause_enabled\":"<<((field<UINT>(change.engine,0x3f8)&0x80000)?"true":"false")
            <<"}\n";
        probe_input_events_.flush();
        if (!probe_input_events_) throw std::runtime_error("Native probe focus evidence write failed");
    }
    static void WINAPI focus_pause(void* engine,int requested) {
        guarded([&] {
            auto& r=self();
            std::lock_guard<std::recursive_mutex> lock(r.focus_pause_mutex_);
            const bool focused=r.motion_enabled_ && r.motion_controls_.sample(r.input_clock()).focused;
            auto change=FocusPauseChange{engine,requested,requested};
            if (headless_probe && r.probe_focus_policy_==ProbeFocusPolicy::Legacy)
                change.applied=0;
            else if (!headless_probe || r.probe_focus_policy_==ProbeFocusPolicy::Tracked)
                change=r.focus_pause_.request(engine,requested,focused);
            r.apply_focus_pause(change,focused,"native_request");
        });
    }
    void render(void* renderer,RendererEntry original_entry) {
        render_heartbeat_.store(GetTickCount64(),std::memory_order_release);
        DiagnosticScope diagnostic(render_phase_,1);
        GraphicsScope graphics(*this);
        RenderState state{renderer,ownership_.owns_render(renderer),ownership_.world_panel(renderer),{},nullptr};
        if (state.main) {
            present_panel_=state.panel;
        }
        const auto previous=render_;render_=&state;
        try {
            const auto count=field<UINT>(renderer,0x70);
            const auto views=field<const unsigned char*>(renderer,0x68);
            if (count>16 || (count && !views)) throw std::runtime_error("Invalid native renderer view array");
            for (UINT index=0;index<count;++index)
                if (auto* decoded=decode_view(state,views+index*0x12c0))
                    if (!state.current) state.current=decoded;
            original_entry(renderer);
            render_=previous;
        } catch (...) {render_=previous;throw;}
    }
    OutlastStereoCulling* culling(const void* frustum) {
        if (!render_ || !render_->main) return nullptr;
        for (const auto& view:render_->views)
            if (view->culling && view->culling->owns(frustum)) return view->culling.get();
        return nullptr;
    }
    static const BYTE* WINAPI probe_precomputed_visibility(void* state,const void* view,const void* scene) {
        return guarded([&] {
            auto& r=self();
            const auto result=r.original_probe_visibility_(state,view,scene);
            if (!render_ || !render_->main || render_->panel) return result;
            for (const auto& owned:render_->views) {
                if (owned->view!=view) continue;
                const auto origin=field<Vec3>(view,0x3a0);
                auto& out=r.probe_visibility_;
                out<<std::setprecision(9)<<"{\"native_frame\":"<<r.stats_.frames
                    <<",\"precomputed_handler\":"<<(field<const void*>(scene,0x5178)?"true":"false")
                    <<",\"returned_cell\":"<<(result?"true":"false")
                    <<",\"native_origin\":["<<origin.x<<','<<origin.y<<','<<origin.z<<"],\"eye_origins\":[";
                for (unsigned eye=0;eye<2;++eye) {
                    const auto& values=owned->eyes[eye];
                    out<<(eye?",[":"[")<<values[16]-values[20]<<','<<values[17]-values[21]
                        <<','<<values[18]-values[22]<<']';
                }
                out<<"]}\n";out.flush();
                if (!out) throw std::runtime_error("Cannot retain native visibility-query evidence");
                break;
            }
            return result;
        });
    }
    D3D9StereoPipeline& pipeline() {
        Internal internal;
        DWORD scissor=0;gpu_check(device_->GetRenderState(D3DRS_SCISSORTESTENABLE,&scissor),"Read native scissor state");
        const auto samplers=variants_->bound_atlas_samplers();
        Com<IDirect3DSurface9> target;
        gpu_check(device_->GetRenderTarget(0,target.put()),"Identify native final UI target");
        ui_draw_=!present_panel_ && target.get()==final_surface_.get() && !samplers &&
            (canvas_ || scaleform_ || variants_->orthographic_view());
        if (ui_draw_ && begin_frame()) {
            D3DVIEWPORT9 viewport{};
            gpu_check(device_->GetViewport(&viewport),"Read original native UI viewport");
            auto canvas=identity_matrix();
            canvas[0]=float(viewport.Width)/(2*width_);
            canvas[3]=float(viewport.X)/width_+canvas[0]-1;
            canvas[5]=float(viewport.Height)/height_;
            canvas[7]=1-float(viewport.Y)*2/height_-canvas[5];
            ui_eyes_=menu_overlay_?panel_anchor_.locate_ui(tracked_,fovs_,float(width_)/height_):
                headlocked_ui_constants(tracked_,fovs_,float(width_)/height_);
            for (auto& eye:ui_eyes_) {
                Matrix4 plane{};std::copy_n(eye.begin(),16,plane.begin());
                const auto mapped=matrix_product(plane,canvas);
                std::copy(mapped.begin(),mapped.end(),eye.begin());
            }
        }
        const bool tracked=!present_panel_ && !ui_draw_ && render_ && render_->main && render_->current && frame_open_;
        const bool eye_view=!ui_draw_ && render_ && render_->current;
        auto& result=[&]() ->D3D9StereoPipeline& {
            try {
                return variants_->current(samplers,tracked,eye_view,scissor!=0,true,tracked,ui_draw_ && frame_open_);
            } catch (const std::exception& error) {
                log("Native shader transformation refused: "+std::string(error.what()));
                if (headless_probe) {
                    try {
                        std::array<wchar_t,32768> folder{};
                        const auto count=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder.data(),DWORD(folder.size()));
                        if (!count || count>=folder.size())
                            throw std::runtime_error("Refused-shader evidence directory is missing");
                        NativeDrawProbe failure(std::filesystem::path(std::wstring(folder.data(),count)),0,L"shader-failure");
                        failure.retain_bound_shaders(*device_.get());
                    } catch (const std::exception& diagnostic) {
                        log("Refused-shader evidence could not be retained: "+std::string(diagnostic.what()));
                    }
                }
                throw;
            }
        }();
        result.set_native_view_columns(eye_view);
        if (tracked) {
            if (std::any_of(result.diagnostic_binding().tracked_uniforms.begin(),
                result.diagnostic_binding().tracked_uniforms.end(),[](const sm3::TrackedUniform& uniform) {
                    return uniform.kind==sm3::TrackedUniformKind::camera_direction;
                })) {
                if (particle_parameter_view_!=render_->current->view || particle_parameter_frame_!=stats_.frames)
                    throw std::runtime_error("Tracked particle camera axes lack their native factory/view binding");
                result.set_camera_directions_view_dependent(particle_camera_axes_view_dependent_);
            }
            StereoReconstructionContext context{};
            context.original_camera=render_->current->camera.tracking;
            context.original_projection=outlast_view_matrix(render_->current->view,0x12c0);
            result.set_reconstruction_context(context);
            if (camcorder_capture_ && !camcorder_world_depth_.get()) {
                Com<IDirect3DSurface9> depth;
                const auto status=device_->GetDepthStencilSurface(depth.put());
                if (status!=D3DERR_NOTFOUND) gpu_check(status,"Identify physical camcorder world depth");
                OutlastAtlasTag tag{};
                if (depth.get() && atlas_tag(*depth.get(),tag) &&
                    tag.role==OutlastTargetRole::scene_depth) {
                    depth->AddRef();
                    *camcorder_world_depth_.put()=depth.get();
                    const auto& camera=render_->current->camera;
                    camcorder_near_=camera.near_plane/camera.tracking.units_per_meter;
                    camcorder_far_=camera.far_plane/camera.tracking.units_per_meter;
                    camcorder_depth_scale_=camera.depth_scale;
                }
            }
        }
        return result;
    }
    StereoViewConstants eye_records() const {
        if (ui_draw_ && frame_open_) return ui_eyes_;
        return render_ && render_->current?render_->current->eyes:StereoViewConstants{};
    }
    void submitted(const SubmissionResult& result) {
        if (!result.succeeded()) throw std::runtime_error("Single-pass operation/restoration failed: "+
            std::to_string(result.operation_status)+"/"+std::to_string(result.restoration_status));
        ++stats_.geometry_draws;
        if (draw_probe_) {
            const auto now=(probe_draw_contact_ || probe_draw_jump_ || probe_draw_camera_)?input_clock():0;
            bool camera_ready=false;
            if (probe_draw_camera_) {
                std::lock_guard<std::mutex> lock(camcorder_mutex_);
                const auto& camera=physical_camera_;
                const bool held=camera.frame.held && camera.frame.hand==1 && camera.native_state==1 &&
                    !camera.night_vision && camera.native_mesh_ready && now>=camera.clock &&
                    now-camera.clock<100000;
                if (!held) probe_camera_ready_since_us_=0;
                else if (!probe_camera_ready_since_us_) probe_camera_ready_since_us_=now;
                camera_ready=probe_camera_ready_since_us_ && now-probe_camera_ready_since_us_>=50000;
            }
            auto ready=[&](const std::atomic<UINT64>& value) {
                const auto since=value.load(std::memory_order_acquire);
                return since && now>=since && now-since>=50000;
            };
            draw_probe_->capture(*device_.get(),stats_.frames,
                camera_ready || ((probe_draw_contact_ || probe_draw_jump_) && ready(probe_body_ready_since_us_)));
            if (contact_draw_probe_)
                contact_draw_probe_->capture(*device_.get(),stats_.frames,ready(probe_contact_ready_since_us_));
            if (recovery_draw_probe_)
                recovery_draw_probe_->capture(*device_.get(),stats_.frames,ready(probe_recovery_ready_since_us_));
        }
        if (ui_draw_) ++stats_.ui_draws;
        if (ui_draw_ && frame_open_) ++stats_.ui_plane_draws;
    }
    static void WINAPI main_viewport(void* a,float* b,void* c) {
        guarded([&] {auto& r=self();
            bool panel=false;
            if (!r.test_) {
                const auto* world=field<const void*>(reinterpret_cast<const void*>(r.base_),0x200b5a8);
                if (!world || field<int>(world,0x78)==0) panel=true;
                else {
                    const bool menu=r.menu_state_()!=0;
                    r.menu_seen_ |= menu;
                    panel=menu || !r.menu_seen_;
                }
            }
            OutlastRendererOwnership::MainScope scope(r.ownership_,a,panel);
            r.original_.main_viewport(a,b,c);});
    }
    static void WINAPI enqueue(void* a,const void* family) {
        guarded([&] {auto& r=self();OutlastRendererOwnership::FamilyScope scope(r.ownership_,family);
            r.original_.enqueue(a,family);});
    }
    static void* WINAPI constructor(void* renderer,const void* family,std::uint64_t p,const void* data,UINT flags) {
        return guarded([&] {auto& r=self();auto* result=r.original_.constructor(renderer,family,p,data,flags);
            r.ownership_.constructed(result,family);return result;});
    }
    static void WINAPI dispatch(void* renderer) {
        guarded([&] {auto& r=self();OutlastRendererOwnership::DispatchScope scope(r.ownership_,renderer);
            r.original_.dispatch(renderer);});
    }
    static void WINAPI normal(void* renderer) {guarded([&] {auto& r=self();r.render(renderer,r.original_.normal);});}
    static void WINAPI alternate(void* renderer) {guarded([&] {auto& r=self();r.render(renderer,r.original_.alternate);});}
    static INT WINAPI translucency_bounds(void* renderer,UINT depth_group,RECT* bounds) {
        return guarded([&] {
            auto& r=self();
            if (!render_ || !render_->main || render_->panel || render_->renderer!=renderer ||
                render_->views.empty())
                return r.original_.translucency_bounds(renderer,depth_group,bounds);
            if (!bounds) throw std::invalid_argument("Native translucency bounds output cannot be null");
            RECT full{LONG_MAX,LONG_MAX,0,0};
            for (const auto& view:render_->views) {
                const auto viewport=field<std::array<INT,4>>(view->view,0x6c);
                if (viewport[0]<0 || viewport[1]<0 || viewport[2]<0 || viewport[3]<0 ||
                    viewport[0]>LONG_MAX-viewport[2] || viewport[1]>LONG_MAX-viewport[3])
                    throw std::invalid_argument("Invalid native translucency viewport");
                if (!viewport[2] || !viewport[3]) continue;
                full.left=std::min(full.left,LONG(viewport[0]));
                full.top=std::min(full.top,LONG(viewport[1]));
                full.right=std::max(full.right,LONG(viewport[0]+viewport[2]));
                full.bottom=std::max(full.bottom,LONG(viewport[1]+viewport[3]));
            }
            if (full.left>=full.right || full.top>=full.bottom)
                return r.original_.translucency_bounds(renderer,depth_group,bounds);
            // Mono-projected glass bounds can be empty or stale after a tracked head turn.
            *bounds=full;
            return INT(1);
        });
    }
    static void WINAPI view(void* rhi,const void* native_view) {
        guarded([&] {auto& r=self();
            if (render_) {
                render_->current=nullptr;
                for (const auto& candidate:render_->views)
                    if (candidate->view==native_view) render_->current=candidate.get();
            }
            r.original_.view(rhi,native_view);});
    }
    static int WINAPI sphere(const void* f,const float* c,const float* size) {
        return guarded([&] {auto& r=self();if (auto* owned=r.culling(f)) {
                ++r.stats_.owned_culls;
                try {return owned->sphere(f,c,size)?1:0;}
                catch (const std::exception& error) {
                    const auto center=field<Vec3>(c,0);const auto radius=field<float>(size,0);
                    throw std::runtime_error(std::string(error.what())+"; native_center="+
                        std::to_string(center.x)+","+std::to_string(center.y)+","+std::to_string(center.z)+
                        "; native_radius="+std::to_string(radius)+
                        "; radius_bits="+std::to_string(field<UINT>(size,0)));
                }}
            return r.original_.sphere(f,c,size);});
    }
    static int WINAPI box(const void* f,const float* c,const float* size) {
        return guarded([&] {auto& r=self();if (auto* owned=r.culling(f)) {
                ++r.stats_.owned_culls;return owned->box(f,c,size)?1:0;}
            return r.original_.box(f,c,size);});
    }
    static int WINAPI sphere_inside(const void* f,const float* c,const float* size,int* inside) {
        return guarded([&] {auto& r=self();if (auto* owned=r.culling(f)) {
                if (!inside) throw std::invalid_argument("Native sphere full-inside output cannot be null");
                bool full=false;const auto visible=owned->sphere(f,c,size,&full);*inside=full?1:0;
                ++r.stats_.owned_culls;++r.stats_.sphere_inside_culls;return visible?1:0;}
            return r.original_.sphere_inside(f,c,size,inside);});
    }
    static void WINAPI crosshair_update(void* hud,float elapsed) {
        guarded([&] {auto& r=self();
            if (!hud) throw std::invalid_argument("Native crosshair update has no HUD");
            r.original_.crosshair_update(hud,elapsed);
            // The native reticle draw tests this alpha after updating its other state.
            const float hidden=0;
            std::memcpy(static_cast<unsigned char*>(hud)+0x6e4,&hidden,sizeof(hidden));
            ++r.stats_.crosshair_updates;
        });
    }
    static int WINAPI box_inside(const void* f,const float* c,const float* size,int* inside) {
        return guarded([&] {auto& r=self();if (auto* owned=r.culling(f)) {
                if (!inside) throw std::invalid_argument("Native full-inside output cannot be null");
                bool full=false;const auto visible=owned->box(f,c,size,&full);*inside=full?1:0;
                ++r.stats_.owned_culls;return visible?1:0;}
            return r.original_.box_inside(f,c,size,inside);});
    }
    static void WINAPI indexed(void* rhi,void* indices,UINT topology,INT base,UINT minimum,UINT vertices,UINT start,UINT primitives) {
        guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (indices && primitives && vertices && topology<=2 && r.device_.get() &&
                r.target_tag(tag) && field<UINT>(rhi,0x284)>1 && field<UINT>(rhi,0x288)) {
                const UINT mapping[]{D3DPT_TRIANGLELIST,D3DPT_TRIANGLESTRIP,D3DPT_LINELIST};
                OutlastReplayCompactor compact(rhi,0x3cc,*r.device_.get(),
                    {mapping[topology],base,minimum,vertices,start,primitives});
                if (replay_) throw std::logic_error("Nested native object replay is unsupported");
                replay_=&compact;
                r.original_.indexed(rhi,indices,topology,base,minimum,vertices,start,primitives);
                replay_=nullptr;
                compact.finish(compact.observed_calls()==0);
            } else r.original_.indexed(rhi,indices,topology,base,minimum,vertices,start,primitives);});
    }
    static std::uint64_t WINAPI batched(void* data,const void* transform,UINT width,
        std::uintptr_t height,UINT flags,std::uint32_t gamma_bits) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            const auto matrix=field<std::array<float,16>>(transform,0);
            for (float value:matrix) if (!std::isfinite(value))
                throw std::invalid_argument("Native Canvas transform contains nonfinite values");
            const bool previous=canvas_;
            canvas_=std::abs(matrix[3])+std::abs(matrix[7])+std::abs(matrix[11])<.0001f &&
                std::abs(matrix[15]-1)<.0001f;
            try {
                const auto result=r.original_.batched(data,transform,width,height,flags,gamma_bits);
                canvas_=previous;return result;
            } catch (...) {canvas_=previous;throw;}
        });
    }
    static std::uint64_t WINAPI ui(void* command) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            const bool previous=scaleform_;scaleform_=true;
            try {
                const auto result=r.original_.ui(command);
                scaleform_=previous;return result;
            } catch (...) {scaleform_=previous;throw;}
        });
    }
    static std::uint64_t WINAPI offscreen_movie(void* command) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            auto* capture=r.camcorder_probe_?r.camcorder_probe_.get():r.camcorder_capture_.get();
            const auto previous=owned_hud_display_root_;owned_hud_display_root_=nullptr;
            try {
                const auto count=field<INT>(command,0x18);
                auto* records=field<unsigned char*>(command,0x10);
                if (count<0 || count>128 || (count && !records))
                    throw std::runtime_error("Invalid native offscreen movie command records");
                for (INT i=0;capture && i<count;++i) {
                    auto* record=records+i*0x8c;
                    if (capture->owns_hud_record(field<void*>(record,0x70),field<void*>(record,0x78))) {
                        auto* root=r.movie_display_root_(record+0x44);
                        if (owned_hud_display_root_ && root!=owned_hud_display_root_)
                            throw std::runtime_error("Owned HUD command contains conflicting native display roots");
                        owned_hud_display_root_=root;
                        capture->observe_hud_command(record,root,UINT(r.stats_.frames),i,count);
                    }
                }
                const auto result=r.original_offscreen_movie_(command);
                owned_hud_display_root_=previous;return result;
            } catch (...) {owned_hud_display_root_=previous;throw;}
        });
    }
    static void WINAPI movie_display(void* renderer,void* root) {
        guarded([&] {auto& r=self();
            const auto previous=rendering_native_hud_;
            rendering_native_hud_=root && root==owned_hud_display_root_;
            try {
                r.original_movie_display_(renderer,root);
                rendering_native_hud_=previous;
            } catch (...) {rendering_native_hud_=previous;throw;}
        });
    }
    static void* WINAPI texture(void* out,UINT w,UINT h,BYTE format,UINT mips,UINT flags,std::uintptr_t data) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastTargetRole role=OutlastTargetRole::shared;
            const bool known=caller>=r.base_ && outlast_allocation_role(caller-r.base_,role);
            Allocation next{role,w,h};auto* previous=allocation_;
            if (known && stereo_target(role)) allocation_=&next;
            Com<IDirect3DTexture9> created;
            auto* previous_created=probe_created_texture_;
            const bool lens_allocation=(r.camcorder_probe_ || r.camcorder_capture_) && w==512 && h==288 &&
                caller>=r.base_+0x2fa4f0 && caller<r.base_+0x2fa7a0;
            if (lens_allocation) probe_created_texture_=&created;
            struct RestoreCreated {
                Com<IDirect3DTexture9>* previous;
                ~RestoreCreated() {probe_created_texture_=previous;}
            } restore_created{previous_created};
            auto* result=r.original_.texture(out,w,h,format,mips,flags,data);
            allocation_=previous;
            const auto handle=field<std::uintptr_t>(out,0);
            if (lens_allocation) {
                if (!created.get()) throw std::runtime_error("Native lens factory did not create an observed D3D texture");
                (r.camcorder_probe_?r.camcorder_probe_:r.camcorder_capture_)->remember_texture(handle,*created.get());
            }
            r.native_textures_.erase(handle);
            if (known && stereo_target(role)) r.native_textures_.emplace(handle,next);
            return result;});
    }
    static void* WINAPI surface(void* out,UINT w,UINT h,BYTE format,std::uintptr_t tex,UINT flags,std::uintptr_t name) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);auto* previous=allocation_;
            Allocation next{OutlastTargetRole::shared,w,h};
            const bool known=caller>=r.base_ && outlast_allocation_role(caller-r.base_,next.role);
            const auto found=r.native_textures_.find(tex);
            if (found!=r.native_textures_.end()) {
                if (known && next.role!=found->second.role)
                    throw std::runtime_error("Native surface allocation site conflicts with its actual texture role");
                next.role=found->second.role;
            }
            if (stereo_target(next.role)) allocation_=&next;
            auto* result=r.original_.surface(out,w,h,format,tex,flags,name);allocation_=previous;return result;});
    }
    static IDirect3D9* WINAPI factory(UINT version) {
        return guarded([&] {auto& r=self();
            if (version!=D3D_SDK_VERSION) throw std::runtime_error("Unexpected native D3D9 SDK version");
            auto* result=r.xr().graphics().get();result->AddRef();return result;});
    }
    static UINT adapter(UINT requested) {return internal_depth?requested:self().xr().graphics().adapter_ordinal();}
    static UINT WINAPI adapter_count(IDirect3D9* f) {
        return guarded([&] {return internal_depth?self().original<AdapterCount>(f,4)(f):1u;});
    }
    static HRESULT WINAPI adapter_identifier(IDirect3D9* f,UINT a,DWORD flags,D3DADAPTER_IDENTIFIER9* out) {
        return guarded([&] {return self().original<AdapterIdentifier>(f,5)(f,adapter(a),flags,out);});
    }
    static UINT WINAPI mode_count(IDirect3D9* f,UINT a,D3DFORMAT format) {
        return guarded([&] {return self().original<ModeCount>(f,6)(f,adapter(a),format);});
    }
    static HRESULT WINAPI enum_mode(IDirect3D9* f,UINT a,D3DFORMAT format,UINT mode,D3DDISPLAYMODE* out) {
        return guarded([&] {return self().original<EnumMode>(f,7)(f,adapter(a),format,mode,out);});
    }
    static HRESULT WINAPI display_mode(IDirect3D9* f,UINT a,D3DDISPLAYMODE* out) {
        return guarded([&] {return self().original<DisplayMode>(f,8)(f,adapter(a),out);});
    }
    static HRESULT WINAPI check_type(IDirect3D9* f,UINT a,D3DDEVTYPE t,D3DFORMAT display,D3DFORMAT back,BOOL windowed) {
        return guarded([&] {return self().original<CheckType>(f,9)(f,adapter(a),t,display,back,windowed);});
    }
    static HRESULT WINAPI check_format(IDirect3D9* f,UINT a,D3DDEVTYPE t,D3DFORMAT display,DWORD usage,D3DRESOURCETYPE resource,D3DFORMAT format) {
        return guarded([&] {return self().original<CheckFormat>(f,10)(f,adapter(a),t,display,usage,resource,format);});
    }
    static HRESULT WINAPI check_samples(IDirect3D9* f,UINT a,D3DDEVTYPE t,D3DFORMAT format,BOOL windowed,D3DMULTISAMPLE_TYPE sample,DWORD* quality) {
        return guarded([&] {return self().original<CheckSamples>(f,11)(f,adapter(a),t,format,windowed,sample,quality);});
    }
    static HRESULT WINAPI check_depth(IDirect3D9* f,UINT a,D3DDEVTYPE t,D3DFORMAT display,D3DFORMAT render,D3DFORMAT depth) {
        return guarded([&] {return self().original<CheckDepth>(f,12)(f,adapter(a),t,display,render,depth);});
    }
    static HRESULT WINAPI check_conversion(IDirect3D9* f,UINT a,D3DDEVTYPE t,D3DFORMAT source,D3DFORMAT destination) {
        return guarded([&] {return self().original<CheckConversion>(f,13)(f,adapter(a),t,source,destination);});
    }
    static HRESULT WINAPI adapter_caps(IDirect3D9* f,UINT a,D3DDEVTYPE t,D3DCAPS9* out) {
        return guarded([&] {
            const auto result=self().original<AdapterCaps>(f,14)(f,adapter(a),t,out);
            if (!internal_depth && SUCCEEDED(result) && out) out->AdapterOrdinal=0;
            return result;});
    }
    static HMONITOR WINAPI adapter_monitor(IDirect3D9* f,UINT a) {
        return guarded([&] {return self().original<AdapterMonitor>(f,15)(f,adapter(a));});
    }
    static void configure_presentation(D3DPRESENT_PARAMETERS& p) {
        log("Desktop presentation interval "+std::to_string(p.PresentationInterval)+
            " -> immediate; frame pacing owned by OpenXR");
        p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        if (headless_probe) {
            p.Windowed=TRUE;
            p.FullScreen_RefreshRateInHz=0;
        }
    }
    static HRESULT WINAPI create_device(IDirect3D9* f,UINT,D3DDEVTYPE type,HWND window,DWORD flags,D3DPRESENT_PARAMETERS* p,IDirect3DDevice9** out) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (!p || !out || type!=D3DDEVTYPE_HAL) throw std::invalid_argument("Unsupported native D3D9 creation request");
            if (p->MultiSampleType!=D3DMULTISAMPLE_NONE) throw std::runtime_error("VR final output requires single-sample native rendering");
            configure_presentation(*p);
            flags=(flags&~(D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_MIXED_VERTEXPROCESSING))|
                D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED;
            HRESULT result=D3DERR_INVALIDCALL;
            {
                Internal internal;
                result=r.original<CreateDevice>(f,16)(f,r.xr().graphics().adapter_ordinal(),type,window,flags,p,out);
            }
            if (SUCCEEDED(result)) r.attach_device(**out);
            return result;});
    }
    static HRESULT WINAPI backbuffer(IDirect3DDevice9* d,UINT chain,UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface9** out) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (!internal_depth && d==r.device_.get() && chain==0 && index==0 && type==D3DBACKBUFFER_TYPE_MONO && r.final_surface_.get()) {
                if (!out) return D3DERR_INVALIDCALL;
                *out=r.final_surface_.get();(*out)->AddRef();return HRESULT(D3D_OK);}
            return r.original<GetBackBuffer>(d,18)(d,chain,index,type,out);});
    }
    static HRESULT WINAPI set_target(IDirect3DDevice9* d,DWORD slot,IDirect3DSurface9* surface) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (!internal_depth && slot==0 && surface==r.desktop_.get()) surface=r.final_surface_.get();
            const auto status=r.original<SetTarget>(d,37)(d,slot,surface);
            if (!internal_depth && slot==0 && SUCCEEDED(status)) r.trace_camcorder_gpu("target");
            return status;});
    }
    static HRESULT WINAPI set_viewport(IDirect3DDevice9* d,const D3DVIEWPORT9* v) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);D3DVIEWPORT9 physical{};OutlastAtlasTag tag{};
            const auto requested=v?*v:D3DVIEWPORT9{};
            if (!internal_depth && v && d==r.device_.get() && r.target_tag(tag)) {
                physical=*v;
                if (v->X>UINT_MAX/2 || v->Width>UINT_MAX/2) return HRESULT(D3DERR_INVALIDCALL);
                physical.X*=2;physical.Width*=2;v=&physical;
            }
            const auto status=r.original<SetViewport>(d,47)(d,v);
            if (!internal_depth && SUCCEEDED(status)) r.trace_camcorder_gpu("viewport",&requested);
            return status;});
    }
    static HRESULT WINAPI get_viewport(IDirect3DDevice9* d,D3DVIEWPORT9* v) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);const auto result=r.original<GetViewport>(d,48)(d,v);
            OutlastAtlasTag tag{};
            if (!internal_depth && SUCCEEDED(result) && v && d==r.device_.get() && r.target_tag(tag)) {v->X/=2;v->Width/=2;}
            return result;});
    }
    static HRESULT WINAPI create_texture(IDirect3DDevice9* d,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9** out,HANDLE* shared) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);const bool atlas=!internal_depth && allocation_ && stereo_target(allocation_->role);
            const UINT logical=w;
            if (atlas) {
                if (levels!=1 || w>UINT_MAX/2 || w!=allocation_->width || h!=allocation_->height)
                    throw std::runtime_error("Native role allocation violates the verified one-level atlas contract");
                w*=2;
            }
            const auto result=r.original<CreateTexture>(d,23)(d,w,h,levels,usage,format,pool,out,shared);
            if (SUCCEEDED(result) && out && *out) {
                if (probe_created_texture_) {
                    if (probe_created_texture_->get())
                        throw std::runtime_error("Native lens RHI factory created more than one D3D texture");
                    (*out)->AddRef();*probe_created_texture_->put()=*out;
                }
                Internal internal;r.hook_texture(**out);
                if (atlas) {
                    tag_atlas_texture(**out,allocation_->role,logical,h);
                    Com<IDirect3DSurface9> s;gpu_check((*out)->GetSurfaceLevel(0,s.put()),"Get created native atlas level");
                    r.hook_surface(*s.get());++r.stats_.tagged_allocations;
                }
            }
            return result;});
    }
    static HRESULT create_surface(IDirect3DDevice9* d,UINT w,UINT h,D3DFORMAT format,D3DMULTISAMPLE_TYPE sample,DWORD quality,BOOL discard,IDirect3DSurface9** out,HANDLE* shared,unsigned index) {
        auto& r=self();GraphicsScope graphics(r);const bool atlas=!internal_depth && allocation_ && stereo_target(allocation_->role);
        const UINT logical=w;if (atlas) {
            if (w>UINT_MAX/2 || sample!=D3DMULTISAMPLE_NONE) throw std::runtime_error("Unsupported native atlas surface allocation");
            w*=2;
        }
        const auto result=r.original<CreateTarget>(d,index)(d,w,h,format,sample,quality,discard,out,shared);
        if (SUCCEEDED(result) && out && *out) {
            Internal internal;r.hook_surface(**out);
            if (atlas) {tag_surface(**out,{allocation_->role,logical,h});++r.stats_.tagged_allocations;}
        }
        return result;
    }
    static HRESULT WINAPI create_target(IDirect3DDevice9* d,UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE s,DWORD q,BOOL b,IDirect3DSurface9** o,HANDLE* hnd) {
        return guarded([&] {return create_surface(d,w,h,f,s,q,b,o,hnd,28);});
    }
    static HRESULT WINAPI create_depth(IDirect3DDevice9* d,UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE s,DWORD q,BOOL b,IDirect3DSurface9** o,HANDLE* hnd) {
        return guarded([&] {return create_surface(d,w,h,f,s,q,b,o,hnd,29);});
    }
    static HRESULT WINAPI texture_desc(IDirect3DTexture9* t,UINT level,D3DSURFACE_DESC* out) {
        void* caller=__builtin_return_address(0);
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);const auto result=r.original<TextureDesc>(t,17)(t,level,out);
            if (!internal_depth && r.game_caller(caller) && SUCCEEDED(result) && out) {
                OutlastAtlasTag tag{};if (atlas_tag(*t,tag)) {out->Width=std::max(1u,tag.logical_width>>std::min(level,31u));}
            }return result;});
    }
    static HRESULT WINAPI surface_desc(IDirect3DSurface9* s,D3DSURFACE_DESC* out) {
        void* caller=__builtin_return_address(0);
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);const auto result=r.original<SurfaceDesc>(s,12)(s,out);
            if (!internal_depth && r.game_caller(caller) && SUCCEEDED(result) && out) {
                OutlastAtlasTag tag{};if (atlas_tag(*s,tag)) out->Width=tag.logical_width;
            }return result;});
    }
    static HRESULT WINAPI texture_surface(IDirect3DTexture9* t,UINT level,IDirect3DSurface9** out) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);const auto result=r.original<TextureSurface>(t,18)(t,level,out);
            if (!internal_depth && SUCCEEDED(result) && out && *out) {
                Internal internal;r.hook_surface(**out);OutlastAtlasTag tag{};
                if (atlas_tag(*t,tag)) {
                    if (level) throw std::runtime_error("Unexpected mip level on a one-level native atlas");
                    gpu_check((*out)->SetPrivateData(outlast_atlas_guid,&tag,sizeof(tag),0),"Propagate native surface role");
                }
            }return result;});
    }
    static HRESULT WINAPI clear(IDirect3DDevice9* d,DWORD count,const D3DRECT* rectangles,DWORD flags,D3DCOLOR color,float z,DWORD stencil) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (internal_depth || d!=r.device_.get() || !r.target_tag(tag))
                return r.original<Clear>(d,43)(d,count,rectangles,flags,color,z,stencil);
            if (count && !rectangles) return HRESULT(D3DERR_INVALIDCALL);
            std::vector<D3DRECT> mapped;
            Internal internal;D3DVIEWPORT9 viewport{};
            gpu_check(d->GetViewport(&viewport),"Read native clear viewport");
            const D3DRECT clip{LONG(viewport.X/2),LONG(viewport.Y),
                LONG(viewport.X/2+viewport.Width/2),LONG(viewport.Y+viewport.Height)};
            for (DWORD i=0;i<std::max(1ul,count);++i) {
                D3DRECT a=count?rectangles[i]:clip;
                a.x1=std::max(a.x1,clip.x1);a.y1=std::max(a.y1,clip.y1);
                a.x2=std::min(a.x2,clip.x2);a.y2=std::min(a.y2,clip.y2);
                if (a.x1>=a.x2 || a.y1>=a.y2) continue;
                mapped.push_back(a);
                mapped.push_back({a.x1+LONG(tag.logical_width),a.y1,a.x2+LONG(tag.logical_width),a.y2});
            }
            if (mapped.empty()) return HRESULT(D3D_OK);
            StateRestore restore(*d);
            gpu_check(d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE),"Keep native atlas clear independent of the previous draw scissor");
            D3DVIEWPORT9 full{0,0,tag.logical_width*2,tag.logical_height,viewport.MinZ,viewport.MaxZ};
            gpu_check(d->SetViewport(&full),"Clear both native atlas halves");
            const auto status=r.original<Clear>(d,43)(d,DWORD(mapped.size()),mapped.empty()?nullptr:mapped.data(),flags,color,z,stencil);
            gpu_check(restore.apply(),"Restore native clear state");return status;});
    }
    static RECT rect(const RECT* input,UINT w,UINT h) {return input?*input:RECT{0,0,LONG(w),LONG(h)};}
    static HRESULT WINAPI update_surface(IDirect3DDevice9* d,IDirect3DSurface9* src,const RECT* source,
        IDirect3DSurface9* dst,const POINT* dest) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag a{},b{};
            if (!src || !dst) return HRESULT(D3DERR_INVALIDCALL);
            if (internal_depth || !atlas_tag(*dst,b))
                return r.original<UpdateSurface>(d,30)(d,src,source,dst,dest);
            Internal internal;
            const bool sa=atlas_tag(*src,a);
            D3DSURFACE_DESC sd{};gpu_check(src->GetDesc(&sd),"Read native surface-upload source");
            auto s=rect(source,sa?a.logical_width:sd.Width,sd.Height);
            POINT t=dest?*dest:POINT{};
            const bool full=!sa && dst==r.final_surface_.get() && !t.x && !t.y &&
                s.right-s.left==LONG(b.logical_width) && s.bottom-s.top==LONG(b.logical_height);
            const auto first=r.original<UpdateSurface>(d,30)(d,src,&s,dst,&t);
            if (FAILED(first)) return first;
            if (sa) {s.left+=LONG(a.logical_width);s.right+=LONG(a.logical_width);}
            t.x+=LONG(b.logical_width);
            const auto result=r.original<UpdateSurface>(d,30)(d,src,&s,dst,&t);
            if (SUCCEEDED(result) && full) r.present_panel_=true;
            return result;
        });
    }
    static HRESULT WINAPI stretch(IDirect3DDevice9* d,IDirect3DSurface9* src,const RECT* source,IDirect3DSurface9* dst,const RECT* dest,D3DTEXTUREFILTERTYPE filter) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (!src || !dst) return HRESULT(D3DERR_INVALIDCALL);
            if (internal_depth) return r.original<Stretch>(d,34)(d,src,source,dst,dest,filter);
            Internal internal;OutlastAtlasTag a{},b{};
            const bool sa=src && atlas_tag(*src,a),da=dst && atlas_tag(*dst,b);
            if (!sa && !da) {
                const auto status=r.original<Stretch>(d,34)(d,src,source,dst,dest,filter);
                if (SUCCEEDED(status) && dst) {
                    auto* capture=r.camcorder_probe_?r.camcorder_probe_.get():r.camcorder_capture_.get();
                    if (capture) capture->surface_written(*dst);
                }
                return status;
            }
            D3DSURFACE_DESC sd{},dd{};gpu_check(src->GetDesc(&sd),"Read native blit source");gpu_check(dst->GetDesc(&dd),"Read native blit destination");
            RECT s=rect(source,sa?a.logical_width:sd.Width,sd.Height),t=rect(dest,da?b.logical_width:dd.Width,dd.Height);
            const bool full=!sa && da && dst==r.final_surface_.get() && !t.left && !t.top &&
                t.right==LONG(b.logical_width) && t.bottom==LONG(b.logical_height);
            const auto first=r.original<Stretch>(d,34)(d,src,&s,dst,&t,filter);
            if (FAILED(first) || !da) {
                auto* capture=r.camcorder_probe_?r.camcorder_probe_.get():r.camcorder_capture_.get();
                if (SUCCEEDED(first) && capture) capture->surface_written(*dst);
                return first;
            }
            if (sa) {s.left+=LONG(a.logical_width);s.right+=LONG(a.logical_width);}
            t.left+=LONG(b.logical_width);t.right+=LONG(b.logical_width);
            const auto result=r.original<Stretch>(d,34)(d,src,&s,dst,&t,filter);
            if (SUCCEEDED(result) && full) r.present_panel_=true;
            return result;});
    }
    static HRESULT WINAPI fill(IDirect3DDevice9* d,IDirect3DSurface9* dst,const RECT* rectangle,D3DCOLOR color) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (internal_depth || !dst || !atlas_tag(*dst,tag)) return r.original<Fill>(d,35)(d,dst,rectangle,color);
            RECT left=rect(rectangle,tag.logical_width,tag.logical_height),right=left;
            right.left+=LONG(tag.logical_width);right.right+=LONG(tag.logical_width);
            const auto first=r.original<Fill>(d,35)(d,dst,&left,color);
            return FAILED(first)?first:r.original<Fill>(d,35)(d,dst,&right,color);});
    }
    template<class Submit> HRESULT native_mono_draw(bool eligible,Submit submit) {
        if (internal_depth || !eligible) return submit();
        Internal internal;
        std::array<float,4> native_gamma{};
        const bool ldr_copy=capture_finish_view_ && capture_finish_ldr_source_;
        if (ldr_copy) {
            Com<IDirect3DPixelShader9> shader;
            gpu_check(device_->GetPixelShader(shader.put()),"Read owned native LDR copy shader");
            if (!shader.get()) throw std::runtime_error("Native LDR copy has no pixel shader");
            if (shader.get()!=capture_gamma_shader_.get()) {
                UINT bytes=0;
                gpu_check(shader->GetFunction(nullptr,&bytes),"Read native gamma shader extent");
                if (bytes<8 || bytes%4 || bytes>1048576)
                    throw std::runtime_error("Invalid native gamma shader extent");
                std::vector<DWORD> code(bytes/4);
                const auto expected=bytes;
                gpu_check(shader->GetFunction(code.data(),&bytes),"Read native gamma shader");
                if (bytes!=expected || sha256_bytes(code.data(),bytes)!=
                    "34e89a9dc259fa1dcdda13c69cad1c657abb95e522a1e589d3b0bc9a4d659dde")
                    throw std::runtime_error("Owned native LDR copy does not use the recovered retail gamma shader");
                *capture_gamma_shader_.put()=shader.get();shader->AddRef();
            }
            gpu_check(device_->GetPixelShaderConstantF(9,native_gamma.data(),1),"Save native copy gamma");
            auto gamma=native_gamma;gamma[0]=1;
            gpu_check(device_->SetPixelShaderConstantF(9,gamma.data(),1),"Preserve already-tonemapped native LDR color");
        }
        if (draw_probe_) draw_probe_->retain_bound_shaders(*device_.get(),stats_.frames);
        auto* capture=camcorder_probe_?camcorder_probe_.get():camcorder_capture_.get();
        Com<IDirect3DSurface9> target;
        const bool compare=capture && capture->diagnostic_ready() && camcorder_resolve_probe_ &&
            camcorder_resolve_probe_->due(UINT(stats_.frames)) && render_ && !render_->main;
        const bool trace_hud=headless_probe && capture && capture->needs_hud_draw_trace(UINT(stats_.frames));
        if (capture && (capture->needs_image() || capture->needs_hud_image() || compare ||
            trace_hud))
            gpu_check(device_->GetRenderTarget(0,target.put()),"Track the pending native lens output");
        if (trace_hud && target.get())
            capture->trace_hud_draw(*device_.get(),*target.get(),UINT(stats_.frames),base_,
                rendering_native_hud_,false,reinterpret_cast<std::uintptr_t>(owned_hud_display_root_));
        const auto corrected=[&] {
            const auto draw=[&] {
                if (trace_hud && rendering_native_hud_ && target.get())
                    capture->trace_hud_draw(*device_.get(),*target.get(),UINT(stats_.frames),base_,true,true,
                        reinterpret_cast<std::uintptr_t>(owned_hud_display_root_));
                return variants_->draw_mono(submit,owned_camera_view());
            };
            return rendering_native_hud_ && capture?capture->draw_hud_alpha(*device_.get(),draw):draw();
        };
        HRESULT status=D3D_OK;
        D3DSURFACE_DESC extent{};
        if (compare && target.get()) gpu_check(target->GetDesc(&extent),"Inspect native lens reference scope");
        const auto samplers=compare && extent.Width==512 && extent.Height==288?variants_->bound_atlas_samplers():0;
        if (samplers)
            status=camcorder_resolve_probe_->compare(*device_.get(),*target.get(),UINT(stats_.frames),
                samplers,submit,corrected);
        else status=corrected();
        if (ldr_copy) {
            gpu_check(device_->SetPixelShaderConstantF(9,native_gamma.data(),1),"Restore native RHI gamma cache agreement");
            if (trace_native_postprocess()) {
                native_postprocess_dispatch_<<"{\"kind\":\"final_copy_gamma\",\"frame\":"<<stats_.frames
                    <<",\"native_inverse_gamma\":"<<native_gamma[0]
                    <<",\"selected_inverse_gamma\":1,\"restored\":true}\n";
                native_postprocess_dispatch_.flush();
                if (!native_postprocess_dispatch_) throw std::runtime_error("Cannot retain native LDR gamma restoration");
            }
        }
        if (SUCCEEDED(status) && target.get()) capture->surface_written(*target.get());
        return status;
    }
    static void WINAPI particle_parameters(void* parameters,void* shader,void* factory,void* view) {
        guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (!factory || !view) throw std::runtime_error("Native particle factory/view binding is null");
            r.original_particle_parameters_(parameters,shader,factory,view);
            particle_parameter_view_=view;
            particle_parameter_frame_=r.stats_.frames;
            particle_camera_axes_view_dependent_=field<BYTE>(factory,0x26d)==0;
            return 0;
        });
    }
    template<class NativeDraw,class StereoDraw>
    SubmissionResult reference_material_draw(IDirect3DDevice9* device,const OutlastAtlasTag& tag,
                                             const StereoViewConstants& views,D3D9StereoPipeline& pipeline,
                                             NativeDraw native_draw,StereoDraw stereo_draw) {
        if (!glass_reference_probe_ || tag.role!=OutlastTargetRole::scene_color ||
            !render_ || !render_->main || !render_->current ||
            !glass_reference_probe_->matches(*device,stats_.frames)) return stereo_draw();
        StereoReconstructionContext context{};
        context.original_camera=render_->current->camera.tracking;
        context.original_projection=outlast_view_matrix(render_->current->view,0x12c0);
        return glass_reference_probe_->compare(*device,stats_.frames,views,context,native_draw,stereo_draw,pipeline);
    }
    static HRESULT WINAPI draw_indexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT min,UINT count,UINT start,UINT primitives) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (!internal_depth) r.trace_camcorder_gpu("indexed");
            if (internal_depth || d!=r.device_.get() || !primitives || !count ||
                r.owned_camera_view() || !r.target_tag(tag)) {
                if (!internal_depth) ++r.stats_.non_scene_draws;
                return r.native_mono_draw(d==r.device_.get() && primitives && count,
                    [&] {return r.original<DrawIndexed>(d,82)(d,type,base,min,count,start,primitives);});
            }
            auto& pipeline=r.pipeline();const Draw draw{UINT(type),base,min,count,start,primitives};
            const auto views=r.eye_records();Internal internal;
            if (replay_) {
                const bool first=replay_->observed_calls()==0;
                const auto result=replay_->consume(draw,pipeline,views);
                if (first) r.submitted(result);else ++r.stats_.consumed_replay_calls;
                return HRESULT(result.operation_status);
            }
            const auto result=r.reference_material_draw(d,tag,views,pipeline,
                [&] {return r.original<DrawIndexed>(d,82)(d,type,base,min,count,start,primitives);},
                [&] {return pipeline.draw_indexed(draw,views);});
            r.submitted(result);return HRESULT(result.operation_status);});
    }
    static HRESULT WINAPI draw_primitive(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT first,UINT primitives) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (!internal_depth) r.trace_camcorder_gpu("primitive");
            if (internal_depth || d!=r.device_.get() || !primitives || r.owned_camera_view() || !r.target_tag(tag))
                return r.native_mono_draw(d==r.device_.get() && primitives,
                    [&] {return r.original<DrawPrimitive>(d,81)(d,type,first,primitives);});
            auto& pipeline=r.pipeline();const auto views=r.eye_records();Internal internal;
            const auto result=r.reference_material_draw(d,tag,views,pipeline,
                [&] {return r.original<DrawPrimitive>(d,81)(d,type,first,primitives);},
                [&] {return pipeline.draw_non_indexed(type,first,primitives,views);});
            r.submitted(result);return HRESULT(result.operation_status);});
    }
    static UINT elements(D3DPRIMITIVETYPE type,UINT primitives) {
        const std::uint64_t n=type==D3DPT_TRIANGLELIST?std::uint64_t(primitives)*3:
            type==D3DPT_TRIANGLESTRIP?std::uint64_t(primitives)+2:
            type==D3DPT_LINELIST?std::uint64_t(primitives)*2:0;
        if (!n || n>UINT_MAX) throw std::runtime_error("Unsupported native UP topology or extent");
        return UINT(n);
    }
    static HRESULT WINAPI draw_up(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT primitives,const void* data,UINT stride) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (!internal_depth) r.trace_camcorder_gpu("up");
            if (internal_depth || d!=r.device_.get() || !primitives || r.owned_camera_view() || !r.target_tag(tag))
                return r.native_mono_draw(d==r.device_.get() && primitives,
                    [&] {return r.original<DrawUP>(d,83)(d,type,primitives,data,stride);});
            auto& pipeline=r.pipeline();const auto views=r.eye_records();Internal internal;
            const auto result=r.reference_material_draw(d,tag,views,pipeline,
                [&] {return r.original<DrawUP>(d,83)(d,type,primitives,data,stride);},
                [&] {return pipeline.draw_primitive_up(type,primitives,data,
                    std::size_t(elements(type,primitives))*stride,stride,views);});
            r.submitted(result);return HRESULT(result.operation_status);});
    }
    static HRESULT WINAPI draw_indexed_up(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT minimum,UINT vertices,UINT primitives,const void* indices,D3DFORMAT format,const void* data,UINT stride) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);OutlastAtlasTag tag{};
            if (!internal_depth) r.trace_camcorder_gpu("indexed-up");
            if (internal_depth || d!=r.device_.get() || !primitives || !vertices ||
                r.owned_camera_view() || !r.target_tag(tag))
                return r.native_mono_draw(d==r.device_.get() && primitives && vertices,
                    [&] {return r.original<DrawIndexedUP>(d,84)(d,type,minimum,vertices,primitives,indices,format,data,stride);});
            auto& pipeline=r.pipeline();const auto views=r.eye_records();Internal internal;
            const auto result=r.reference_material_draw(d,tag,views,pipeline,
                [&] {return r.original<DrawIndexedUP>(d,84)(d,type,minimum,vertices,primitives,indices,format,data,stride);},
                [&] {return pipeline.draw_indexed_up({UINT(type),0,minimum,vertices,0,primitives},
                    indices,std::size_t(elements(type,primitives))*(format==D3DFMT_INDEX16?2:4),format,
                    data,(std::size_t(minimum)+vertices)*stride,stride,views);});
            r.submitted(result);return HRESULT(result.operation_status);});
    }
    static HRESULT WINAPI present(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND window,const RGNDATA* dirty) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (internal_depth || d!=r.device_.get()) return r.original<Present>(d,17)(d,a,b,window,dirty);
            if (render_) throw std::logic_error("Presentation cannot interrupt a native renderer");
            r.on_render_thread();Internal internal;
            if (r.probe_desktop_cursor_.is_open()) {
                RECT rectangle{};
                if (!GetClipCursor(&rectangle)) throw std::runtime_error("Cannot inspect desktop cursor confinement");
                DWORD foreground=0;
                GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
                const LONG left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
                const RECT desktop{left,top,left+GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    top+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
                r.probe_desktop_cursor_<<"{\"frame\":"<<r.stats_.frames
                    <<",\"foreground_is_outlast\":"<<(foreground==GetCurrentProcessId()?"true":"false")
                    <<",\"clip\":["<<rectangle.left<<','<<rectangle.top<<','<<rectangle.right<<','<<rectangle.bottom
                    <<"],\"desktop\":["<<desktop.left<<','<<desktop.top<<','<<desktop.right<<','<<desktop.bottom
                    <<"],\"suppressed_clips\":"<<r.suppressed_cursor_clips_.load(std::memory_order_relaxed)
                    <<",\"suppressed_warps\":"<<r.suppressed_cursor_warps_.load(std::memory_order_relaxed)
                    <<",\"nonexclusive_mouse_requests\":"<<r.nonexclusive_mouse_requests_.load(std::memory_order_relaxed)<<"}\n";
                r.probe_desktop_cursor_.flush();
                if (!r.probe_desktop_cursor_) throw std::runtime_error("Cannot retain desktop cursor evidence");
                if (foreground==GetCurrentProcessId() && (rectangle.left!=desktop.left || rectangle.top!=desktop.top ||
                    rectangle.right!=desktop.right || rectangle.bottom!=desktop.bottom))
                    throw std::runtime_error("Outlast still confines the desktop cursor");
            }
            const auto cooperative=d->TestCooperativeLevel();
            if (cooperative==D3DERR_DEVICELOST || cooperative==D3DERR_DEVICENOTRESET || !r.final_surface_.get()) {
                if (r.gpu_profile_) r.gpu_profile_.reset();
                if (r.frame_open_) {r.xr().cancel_frame();r.frame_open_=false;}
                r.motion_controls_.publish({},r.input_clock());
                r.controller_menu_pointer(false,nullptr);
                r.frame_attempted_=false;
                return r.original<Present>(d,17)(d,a,b,window,dirty);
            }
            gpu_check(cooperative,"Read VR presentation recovery state");
            if (r.camcorder_probe_) r.camcorder_probe_->sample(*d,UINT(r.stats_.frames));
            if (r.camcorder_capture_) {
                r.camcorder_capture_->advance_frame(UINT(r.stats_.frames));
                if (!r.camcorder_resolve_probe_ || r.camcorder_resolve_probe_->ready(UINT(r.stats_.frames)))
                    r.camcorder_capture_->sample(*d,UINT(r.stats_.frames));
                if (r.camcorder_resolve_probe_) r.camcorder_resolve_probe_->sampled(r.camcorder_capture_->sampled());
            }
            if (!r.frame_open_) r.begin_frame();
            if (r.frame_open_) {
                if (r.present_panel_!=r.last_present_panel_) {
                    if (r.present_panel_) r.panel_anchor_.reset();
                    log(r.present_panel_?"Native flat content: world-anchored two-eye panel":
                        "Native gameplay: tracked single-pass world");
                    r.last_present_panel_=r.present_panel_;
                }
                r.compose_camcorder();
                if (!r.present_panel_ && !r.menu_overlay_ &&
                    (r.night_vision_enabled_ || r.vignette_strength_>0)) {
                    if (!r.night_vision_filter_)
                        r.night_vision_filter_=std::make_unique<D3D9NightVision>(*d);
                    r.night_vision_filter_->draw(*r.final_surface_.get(),r.night_vision_enabled_,r.vignette_strength_);
                }
                const bool direct=!r.present_panel_ && r.width_==r.xr().width() && r.height_==r.xr().height();
                if (!direct && !r.submission_.get()) {
                    gpu_check(d->CreateTexture(r.xr().width()*2,r.xr().height(),1,D3DUSAGE_RENDERTARGET,
                        D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,r.submission_.put(),nullptr),"Create exact XR output extent");
                    gpu_check(r.submission_->GetSurfaceLevel(0,r.submission_surface_.put()),"Get exact XR output surface");
                }
                if (r.present_panel_) {
                    if (!r.panel_renderer_) r.panel_renderer_=std::make_unique<D3D9WorldPanel>(*d);
                    r.panel_renderer_->draw(*r.final_.get(),*r.submission_surface_.get(),
                        r.xr().width(),r.xr().height(),
                        r.panel_anchor_.locate(r.tracked_,r.fovs_,float(r.width_)/r.height_));
                    r.controller_menu_pointer(true,r.submission_surface_.get());
                    ++r.stats_.geometry_draws;
                    ++r.stats_.world_panel_draws;
                } else {
                    if (!direct) for (unsigned eye=0;eye<2;++eye) {
                    const RECT src{LONG(eye*r.width_),0,LONG((eye+1)*r.width_),LONG(r.height_)};
                    const RECT dst{LONG(eye*r.xr().width()),0,LONG((eye+1)*r.xr().width()),LONG(r.xr().height())};
                    gpu_check(d->StretchRect(r.final_surface_.get(),&src,r.submission_surface_.get(),&dst,D3DTEXF_LINEAR),
                        "Copy native eye region to XR without geometry replay");
                    }
                    if (r.menu_overlay_ && !r.panel_renderer_)
                        r.panel_renderer_=std::make_unique<D3D9WorldPanel>(*d);
                    r.controller_menu_pointer(r.menu_overlay_,
                        r.menu_overlay_?(direct?r.final_surface_.get():r.submission_surface_.get()):nullptr);
                }
                if (r.gpu_profile_) {
                    r.gpu_profile_->end();
                    r.stats_.gpu_timing_status=r.gpu_profile_->status;
                    r.stats_.gpu_timing_frame=r.gpu_profile_->frame;
                    r.stats_.gpu_work_us=r.gpu_profile_->microseconds;
                }
                LARGE_INTEGER end{};
                QueryPerformanceCounter(&end);
                r.stats_.cpu_render_us=UINT64(double(end.QuadPart-r.work_start_.QuadPart)*1000000/r.clock_frequency_.QuadPart);
                r.stats_.shader_variants=UINT(r.variants_->size());
                r.xr().submit(*d,direct?*r.final_.get():*r.submission_.get());
                r.record_performance(r.input_clock()-UINT64(double(end.QuadPart)*1000000/
                    r.clock_frequency_.QuadPart));
                if (direct) ++r.stats_.direct_xr_submissions;
                r.frame_open_=false;++r.stats_.submitted;
            }
            const RECT left{0,0,LONG(r.width_),LONG(r.height_)};
            gpu_check(d->StretchRect(r.final_surface_.get(),&left,r.desktop_.get(),nullptr,D3DTEXF_LINEAR),
                "Mirror native left eye to the unchanged desktop");
            ++r.stats_.frames;
            r.camcorder_world_depth_.reset();
            r.frame_attempted_=false;
            const auto result=r.original<Present>(d,17)(d,a,b,window,dirty);
            if (FAILED(result) && result!=D3DERR_DEVICELOST) gpu_check(result,"Native desktop presentation");
            return result;});
    }
    static HRESULT WINAPI reset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p) {
        return guarded([&] {auto& r=self();GraphicsScope graphics(r);
            if (internal_depth || d!=r.device_.get()) return r.original<Reset>(d,16)(d,p);
            if (render_) throw std::logic_error("Reset cannot interrupt a native renderer");
            if (GetCurrentThreadId()!=r.creation_thread_)
                throw std::logic_error("D3D9 reset requires the device creation thread");
            if (p) configure_presentation(*p);
            r.on_render_thread();Internal internal;
            if (r.frame_open_) {r.xr().cancel_frame();r.frame_open_=false;}
            r.frame_attempted_=false;
            r.xr().drain_transfers();
            r.variants_->release_device_resources();
            r.gpu_profile_.reset();
            r.performance_=PerformanceBatch{};
            r.panel_renderer_.reset();
            r.camcorder_renderer_.reset();
            r.night_vision_filter_.reset();
            r.camcorder_world_depth_.reset();
            if (r.camcorder_capture_) r.camcorder_capture_->release_device_resources();
            if (r.camcorder_probe_) r.camcorder_probe_->release_device_resources();
            const auto cooperative=d->TestCooperativeLevel();
            if (cooperative!=D3DERR_DEVICELOST && cooperative!=D3DERR_DEVICENOTRESET) {
                gpu_check(cooperative,"Read native reset device state");
                gpu_check(d->SetRenderTarget(0,r.desktop_.get()),"Detach the stereo atlas before native reset");
                gpu_check(d->SetDepthStencilSurface(nullptr),"Detach stereo depth before native reset");
            }
            r.final_surface_.reset();r.submission_surface_.reset();r.desktop_.reset();
            r.final_.reset();r.submission_.reset();r.native_textures_.clear();
            const auto result=r.original<Reset>(d,16)(d,p);
            if (SUCCEEDED(result)) {
                r.create_final();
                r.gpu_profile_=std::make_unique<D3D9FrameProfile>(*d);
                ++r.stats_.reset_count;
            }
            return result;});
    }
    void bind_native(const RuntimeBindings& bindings) {
#define BIND(name) if (bindings.name) hooks_->add(bindings.name,&Runtime::name,original_.name)
        BIND(main_viewport);BIND(enqueue);BIND(constructor);BIND(dispatch);
        BIND(normal);BIND(alternate);BIND(view);BIND(indexed);BIND(sphere);
        BIND(box);BIND(box_inside);BIND(sphere_inside);BIND(texture);BIND(surface);
        BIND(batched);BIND(crosshair_update);BIND(translucency_bounds);
        BIND(ui);
#undef BIND
        if (!bindings.camera_base || !bindings.camera_update || !bindings.lean_tick || !bindings.hero_cast)
            throw std::invalid_argument("Native camera stabilization requires complete pose and lean bindings");
        hooks_->add(bindings.camera_base,&camera_base,original_camera_base_);
        hooks_->add(bindings.camera_update,&camera_update,original_camera_update_);
        hooks_->add(bindings.lean_tick,&lean_tick,original_lean_tick_);
        hero_cast_=bindings.hero_cast;
        if (bindings.viewport_focus)
            hooks_->add(bindings.viewport_focus,&viewport_focus,original_viewport_focus_);
        if (bindings.input_tick || bindings.gamepad_state || bindings.gamepad_vibration) {
            if (!bindings.input_tick || !bindings.gamepad_state || !bindings.gamepad_vibration ||
                !bindings.gamepads || !bindings.gamepad_count)
                throw std::invalid_argument("Native motion input requires polling, state and pad inventory bindings");
            gamepads_=bindings.gamepads;gamepad_count_=bindings.gamepad_count;
            hooks_->add(bindings.input_tick,&probe_input_tick,original_input_tick_);
            hooks_->add(bindings.gamepad_state,&gamepad_state,original_gamepad_state_);
            hooks_->add(bindings.gamepad_vibration,&gamepad_vibration,original_gamepad_vibration_);
        }
    }
    template<class F> F retail_entry(const OutlastNativeImage& image,UINT rva) {
        const auto expected=image.body(rva,16);
        auto* loaded=reinterpret_cast<unsigned char*>(base_+rva);
        if (rva>=module_extent_ || module_extent_-rva<expected.size() ||
            std::memcmp(loaded,expected.data(),expected.size())!=0)
            throw std::runtime_error("Native hook prologue differs from the exact retail image");
        return reinterpret_cast<F>(loaded);
    }
    template<class F> void patch_input_import(void** slot,F replacement,F& original) {
        DWORD protection=0,ignored=0;
        if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection))
            throw std::runtime_error("Cannot protect native desktop mouse import");
        const auto previous=InterlockedExchangePointer(slot,reinterpret_cast<void*>(replacement));
        desktop_input_imports_.push_back({slot,previous});
        static_assert(sizeof(original)==sizeof(previous));
        std::memcpy(&original,&previous,sizeof(original));
        if (!VirtualProtect(slot,sizeof(void*),protection,&ignored))
            throw std::runtime_error("Cannot restore native desktop mouse import protection");
    }
    void restore_input_imports() noexcept {
        for (auto it=desktop_input_imports_.rbegin();it!=desktop_input_imports_.rend();++it) {
            DWORD protection=0,ignored=0;
            if (!VirtualProtect(it->slot,sizeof(void*),PAGE_READWRITE,&protection)) std::terminate();
            InterlockedExchangePointer(it->slot,it->previous);
            if (!VirtualProtect(it->slot,sizeof(void*),protection,&ignored)) std::terminate();
        }
        desktop_input_imports_.clear();
    }
    void patch_factory_import() {
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base_);
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base_+dos->e_lfanew);
        const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || directory.VirtualAddress>=module_extent_)
            throw std::runtime_error("Native image import directory is invalid");
        auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base_+directory.VirtualAddress);
        void** clip=nullptr;void** position=nullptr;void** query_position=nullptr;void** direct_input=nullptr;
        for (;imports->Name;++imports) {
            const auto* name=reinterpret_cast<const char*>(base_+imports->Name);
            const bool graphics=_stricmp(name,"d3d9.dll")==0;
            const bool mouse=_stricmp(name,"user32.dll")==0;
            const bool input=_stricmp(name,"dinput8.dll")==0;
            if (!graphics && !mouse && !input) continue;
            if (!imports->OriginalFirstThunk) throw std::runtime_error("Native D3D9 import names are unavailable");
            auto* names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base_+imports->OriginalFirstThunk);
            auto* slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base_+imports->FirstThunk);
            for (;names->u1.AddressOfData;++names,++slots) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                const auto* symbol=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base_+names->u1.AddressOfData);
                const auto* imported=reinterpret_cast<const char*>(symbol->Name);
                void*** selected=nullptr;
                if (graphics && std::strcmp(imported,"Direct3DCreate9")==0) selected=&factory_iat_;
                if (mouse && std::strcmp(imported,"ClipCursor")==0) selected=&clip;
                if (mouse && std::strcmp(imported,"SetCursorPos")==0) selected=&position;
                if (mouse && std::strcmp(imported,"GetCursorPos")==0) selected=&query_position;
                if (input && std::strcmp(imported,"DirectInput8Create")==0) selected=&direct_input;
                if (!selected) continue;
                if (*selected) throw std::runtime_error("Ambiguous native graphics/desktop input import");
                *selected=reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
        if (!factory_iat_) throw std::runtime_error("Native Direct3DCreate9 import was not found");
        if (!clip || !position || !query_position || !direct_input)
            throw std::runtime_error("Native desktop mouse imports do not match the admitted retail image");
        desktop_input_imports_.reserve(4);
        patch_input_import(clip,&desktop_clip_cursor,original_clip_cursor_);
        patch_input_import(position,&desktop_set_cursor_pos,original_set_cursor_pos_);
        patch_input_import(query_position,&desktop_get_cursor_pos,original_get_cursor_pos_);
        patch_input_import(direct_input,&desktop_direct_input,original_direct_input_);
        DWORD old=0,ignored=0;
        if (!VirtualProtect(factory_iat_,sizeof(void*),PAGE_READWRITE,&old))
            throw std::runtime_error("Cannot protect the native factory import");
        saved_factory_=InterlockedExchangePointer(factory_iat_,reinterpret_cast<void*>(&factory));
        if (!VirtualProtect(factory_iat_,sizeof(void*),old,&ignored))
            throw std::runtime_error("Cannot restore native factory import protection");
    }
public:
    explicit Runtime(const RuntimeTestConfiguration* test) {
        if (instance_) throw std::logic_error("Runtime is already initialized");
        base_=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base_);
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base_+dos->e_lfanew);
        module_extent_=nt->OptionalHeader.SizeOfImage;
        if (!QueryPerformanceFrequency(&clock_frequency_) || !clock_frequency_.QuadPart)
            throw std::runtime_error("Cannot read frame profiling clock frequency");
        test_=test!=nullptr;
        present_panel_=!test_;
        if (test_) {
            if (module_path(nullptr).filename()!=L"test_integrated_mod.exe" ||
                test->size!=sizeof(*test) || test->version!=10 || !test->xr_resolve)
                throw std::invalid_argument("Controlled surrogate initialization is not admitted");
            world_up_=test->world_up;
            config_.comfort=test->comfort;
            motion_controls_.configure(config_.comfort);
            test_xr_=std::make_unique<OpenXRStereoSession>(test->xr_resolve);
        } else {
            OutlastNativeImage image(module_path(nullptr));
            const auto mod_directory=module_path(own_module).parent_path();
            config_=load_outlast_config(mod_directory);
            motion_controls_.configure(config_.comfort);
            menu_pointer_hand_=config_.comfort.primary_hand();
            wchar_t mode[8]{};
            const auto length=GetEnvironmentVariableW(L"OLVR_HEADLESS_PROBE",mode,UINT(std::size(mode)));
            if (length && (length!=1 || mode[0]!=L'1'))
                throw std::invalid_argument("Invalid headset-free verification mode");
            headless_probe=length!=0;
            wchar_t motion[8]{};
            const auto motion_length=GetEnvironmentVariableW(L"OLVR_PROBE_MOTION",motion,UINT(std::size(motion)));
            if (motion_length && (!headless_probe || motion_length!=1 || motion[0]!=L'1'))
                throw std::invalid_argument("Simulated native motion requires explicit headset-free probe mode");
            motion_enabled_=!headless_probe || motion_length!=0;
            wchar_t hand_offset[64]{};
            const auto hand_length=GetEnvironmentVariableW(L"OLVR_HAND_WRISTWARD_CM",hand_offset,UINT(std::size(hand_offset)));
            if (hand_length>=std::size(hand_offset)) throw std::invalid_argument("Hand offset value is too long");
            if (hand_length) hand_wristward_m_=outlast_hand_offset_cm(hand_offset);
            log("Controller-local wristward hand fit: "+std::to_string(hand_wristward_m_*100)+
                " cm; raw XR poses retained; applies to native palm, contact and camera");
            wchar_t native_size[8]{},probe_size[8]{};
            const auto native_size_length=GetEnvironmentVariableW(L"OLVR_NATIVE_RESOLUTION",native_size,UINT(std::size(native_size)));
            const auto probe_size_length=GetEnvironmentVariableW(L"OLVR_PROBE_RECOMMENDED_SIZE",probe_size,UINT(std::size(probe_size)));
            if ((native_size_length && (native_size_length!=1 || native_size[0]!=L'1')) ||
                (probe_size_length && (!headless_probe || probe_size_length!=1 || probe_size[0]!=L'1')))
                throw std::invalid_argument("Invalid native/recommended viewport policy");
            recommended_render_size_=!native_size_length && (!headless_probe || probe_size_length!=0);
            if (headless_probe) {
                wchar_t folder[32768]{};
                const auto count=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder,UINT(std::size(folder)));
                if (!count || count>=std::size(folder))
                    throw std::runtime_error("Native camera evidence requires the explicit probe path");
                probe_cameras_.open(std::filesystem::path(std::wstring(folder,count))/L"native-cameras.jsonl");
                if (!probe_cameras_) throw std::runtime_error("Cannot create native camera evidence");
                probe_camera_poses_.open(std::filesystem::path(std::wstring(folder,count))/L"camera-poses.jsonl");
                if (!probe_camera_poses_) throw std::runtime_error("Cannot create native camera-pose evidence");
                probe_camera_updates_.open(std::filesystem::path(std::wstring(folder,count))/L"camera-updates.jsonl");
                if (!probe_camera_updates_) throw std::runtime_error("Cannot create native final-camera evidence");
                probe_desktop_cursor_.open(std::filesystem::path(std::wstring(folder,count))/L"desktop-cursor.jsonl");
                if (!probe_desktop_cursor_) throw std::runtime_error("Cannot create native desktop cursor evidence");
                probe_menu_pointer_.open(std::filesystem::path(std::wstring(folder,count))/L"native-menu-pointer.jsonl");
                if (!probe_menu_pointer_) throw std::runtime_error("Cannot create native menu pointer evidence");
                wchar_t menu_fixture[16]{};
                probe_menu_fixture_=GetEnvironmentVariableW(L"OLVR_PROBE_MENU_POINTER",menu_fixture,16)!=0;
                probe_lean_weights_.open(std::filesystem::path(std::wstring(folder,count))/L"lean-weights.jsonl");
                if (!probe_lean_weights_) throw std::runtime_error("Cannot create native lean-blend evidence");
                probe_input_events_.open(std::filesystem::path(std::wstring(folder,count))/L"native-input-events.jsonl");
                if (!probe_input_events_) throw std::runtime_error("Cannot create native probe input evidence");
                probe_native_use_.open(std::filesystem::path(std::wstring(folder,count))/L"native-use-events.jsonl");
                if (!probe_native_use_) throw std::runtime_error("Cannot create native controller-use evidence");
                wchar_t focus_policy[16]{};
                const auto focus_length=GetEnvironmentVariableW(L"OLVR_PROBE_FOCUS_POLICY",focus_policy,16);
                if (focus_length) {
                    const std::wstring value(focus_policy,std::min<DWORD>(focus_length,15));
                    if (value==L"Tracked") probe_focus_policy_=ProbeFocusPolicy::Tracked;
                    else if (value==L"Control") probe_focus_policy_=ProbeFocusPolicy::Control;
                    else throw std::invalid_argument("Invalid explicit native focus policy fixture");
                }
                wchar_t unfocused[2]{};
                probe_desktop_unfocused_=GetEnvironmentVariableW(L"OLVR_PROBE_DESKTOP_UNFOCUSED",unfocused,2)==1 &&
                    unfocused[0]==L'1';
                wchar_t skeleton[8]{};
                const auto skeleton_length=GetEnvironmentVariableW(L"OLVR_PROBE_SKELETON",skeleton,UINT(std::size(skeleton)));
                if (skeleton_length) {
                    if (skeleton_length!=1 || skeleton[0]!=L'1')
                        throw std::invalid_argument("Invalid native skeleton diagnostic selection");
                    probe_skeleton_.open(std::filesystem::path(std::wstring(folder,count))/L"native-skeleton.jsonl");
                    if (!probe_skeleton_) throw std::runtime_error("Cannot create native skeleton evidence");
                    wchar_t body[8]{};
                    const auto body_length=GetEnvironmentVariableW(L"OLVR_PROBE_BODY",body,UINT(std::size(body)));
                    if (body_length && (body_length!=1 || body[0]!=L'1'))
                        throw std::invalid_argument("Invalid complete body-pose diagnostic selection");
                    probe_skeleton_all_=body_length!=0;
                }
                if (motion_enabled_) {
                    probe_hand_contacts_.open(std::filesystem::path(std::wstring(folder,count))/L"native-hand-contacts.jsonl");
                    if (!probe_hand_contacts_) throw std::runtime_error("Cannot create native hand contact evidence");
                    probe_hand_recovery_.open(std::filesystem::path(std::wstring(folder,count))/L"native-hand-recovery.jsonl");
                    if (!probe_hand_recovery_) throw std::runtime_error("Cannot create native hand recovery evidence");
                }
                wchar_t selected[16]{};
                const auto selected_count=GetEnvironmentVariableW(L"OLVR_PROBE_DRAW_FRAME",selected,UINT(std::size(selected)));
                wchar_t hdr[8]{};
                const auto hdr_count=GetEnvironmentVariableW(L"OLVR_PROBE_DRAW_HDR",hdr,UINT(std::size(hdr)));
                if (hdr_count && (hdr_count!=1 || hdr[0]!=L'1' || !selected_count))
                    throw std::invalid_argument("HDR draw samples require an explicit diagnostic draw frame");
                wchar_t glass[8]{};
                const auto glass_count=GetEnvironmentVariableW(L"OLVR_PROBE_GLASS_REFERENCE",glass,UINT(std::size(glass)));
                if (glass_count && (glass_count!=1 || glass[0]!=L'1' || !selected_count))
                    throw std::invalid_argument("Original glass references require an explicit diagnostic draw frame");
                wchar_t sweep[8]{};
                const auto sweep_count=GetEnvironmentVariableW(L"OLVR_PROBE_WINDOW_SWEEP",sweep,UINT(std::size(sweep)));
                wchar_t occlusion[2]{},position[2]{};
                const auto occlusion_count=GetEnvironmentVariableW(L"OLVR_PROBE_OCCLUSION",occlusion,2);
                const bool passive_occlusion=headless_probe && occlusion_count==1 && occlusion[0]==L'1';
                if (occlusion_count && (!passive_occlusion || glass_count || hdr_count || !selected_count ||
                    sweep_count!=1 || sweep[0]!=L'1' ||
                    GetEnvironmentVariableW(L"OLVR_PROBE_WINDOW_POSITION",position,2)!=1 || position[0]!=L'1'))
                    throw std::invalid_argument("Passive occlusion requires explicit roomscale positions without glass replay or HDR readback");
                if (sweep_count && (sweep_count!=1 || sweep[0]!=L'1' || (!glass_count && !passive_occlusion)))
                    throw std::invalid_argument("Window sweep requires explicit original-glass references");
                wchar_t temporal[8]{};
                const auto temporal_count=GetEnvironmentVariableW(L"OLVR_PROBE_TEMPORAL_WINDOWS",
                    temporal,UINT(std::size(temporal)));
                if (temporal_count && (temporal_count!=1 || temporal[0]!=L'1' || !sweep_count || passive_occlusion))
                    throw std::invalid_argument("Continuous native windows require the original window sweep");
                wchar_t reflective[2]{},contribution[2]{},particle[2]{};
                const auto reflective_count=GetEnvironmentVariableW(L"OLVR_PROBE_REFLECTIVE_GLASS",reflective,2);
                const auto contribution_count=GetEnvironmentVariableW(L"OLVR_PROBE_GLASS_CONTRIBUTION",contribution,2);
                const auto particle_count=GetEnvironmentVariableW(L"OLVR_PROBE_PARTICLE_WINDOWS",particle,2);
                if ((reflective_count && (reflective_count!=1 || reflective[0]!=L'1')) ||
                    (contribution_count && (contribution_count!=1 || contribution[0]!=L'1')) ||
                    (particle_count && (particle_count!=1 || particle[0]!=L'1' || reflective_count)) ||
                    ((reflective_count || contribution_count || particle_count) && (!headless_probe || !glass_count || !temporal_count ||
                     GetEnvironmentVariableW(L"OLVR_PROBE_WINDOW_POSITION",position,2)!=1 || position[0]!=L'1')))
                    throw std::invalid_argument("Material contribution requires owned headless roomscale glass references");
                wchar_t no_images[2]{};
                const auto no_images_count=GetEnvironmentVariableW(L"OLVR_PROBE_NO_PROJECTION_IMAGES",no_images,2);
                if (no_images_count && (no_images_count!=1 || no_images[0]!=L'1'))
                    throw std::invalid_argument("Invalid diagnostic projection-readback policy");
                const bool material_readbacks=no_images_count && glass_count && sweep_count &&
                    temporal_count && (reflective_count || contribution_count || particle_count) && !hdr_count;
                if (no_images_count && glass_count && !material_readbacks)
                    throw std::invalid_argument("Bounded material readback must retain complete original-shader witnesses");
                if (selected_count) {
                    if (selected_count>=std::size(selected)) throw std::invalid_argument("Invalid diagnostic draw frame");
                    probe_draw_contact_=std::wstring(selected,selected_count)==L"contact";
                    probe_draw_jump_=std::wstring(selected,selected_count)==L"jump";
                    probe_draw_camera_=std::wstring(selected,selected_count)==L"camera";
                    std::uint64_t frame=0;
                    if (probe_draw_jump_) {
                        wchar_t fixture[2]{};
                        if (GetEnvironmentVariableW(L"OLVR_PROBE_JUMP",fixture,2)!=1 || fixture[0]!=L'1')
                            throw std::invalid_argument("Airborne GPU capture requires the actual native jump fixture");
                    }
                    if (probe_draw_camera_) {
                        wchar_t fixture[2]{};
                        if (GetEnvironmentVariableW(L"OLVR_PROBE_PHYSICAL_CAMCORDER",fixture,2)!=1 || fixture[0]!=L'1')
                            throw std::invalid_argument("Held-camera GPU selection requires the actual physical-camera fixture");
                    }
                    if (!probe_draw_contact_ && !probe_draw_jump_ && !probe_draw_camera_) {
                        std::size_t consumed=0;
                        frame=std::stoull(selected,&consumed);
                        if (consumed!=selected_count || !frame || frame>10000)
                            throw std::invalid_argument("Invalid diagnostic draw frame");
                    }
                    if (hdr_count && (probe_draw_contact_ || probe_draw_jump_ || probe_draw_camera_))
                        throw std::invalid_argument("HDR draw samples require a fixed diagnostic frame");
                    if (passive_occlusion && (probe_draw_contact_ || probe_draw_jump_ || probe_draw_camera_ || frame!=600))
                        throw std::invalid_argument("Passive occlusion requires the explicit frame-600 fixture");
                    if (glass_count) {
                        if (probe_draw_contact_ || probe_draw_jump_ || probe_draw_camera_)
                            throw std::invalid_argument("Original glass references require a fixed diagnostic frame");
                        if (sweep_count && frame!=600)
                            throw std::invalid_argument("Window sweep requires the explicit frame-600 fixture");
                        glass_reference_probe_=std::make_unique<NativeGlassReferenceProbe>(
                            std::filesystem::path(std::wstring(folder,count)),frame,sweep_count!=0,
                            temporal_count!=0,reflective_count!=0,contribution_count!=0,particle_count!=0);
                    }
                    wchar_t body_fixture[2]{};
                    const bool pose_constants=((GetEnvironmentVariableW(L"OLVR_PROBE_BODY",body_fixture,2)==1 &&
                        body_fixture[0]==L'1') || occlusion_count) && !hdr_count && !glass_count;
                    draw_probe_=std::make_unique<NativeDrawProbe>(
                        std::filesystem::path(std::wstring(folder,count)),frame,L"draw-probe",hdr_count!=0,
                        (pose_constants || material_readbacks)?NativeDrawReadback::constants_only:
                            NativeDrawReadback::images);
                    if (probe_draw_contact_)
                        contact_draw_probe_=std::make_unique<NativeDrawProbe>(
                            std::filesystem::path(std::wstring(folder,count)),0,L"contact-draw-probe",false,
                            NativeDrawReadback::constants_only);
                    wchar_t corner[2]{};
                    if (GetEnvironmentVariableW(L"OLVR_PROBE_CORNER_RECOVERY",corner,2)==1 && corner[0]==L'1')
                        recovery_draw_probe_=std::make_unique<NativeDrawProbe>(
                            std::filesystem::path(std::wstring(folder,count)),0,L"recovery-draw-probe",false,
                            NativeDrawReadback::constants_only);
                }
            }
            const auto library=headless_probe?mod_directory/L"OutlastVRHeadlessProbe.dll":
                config_path(mod_directory,config_.openxr_loader.empty()?
                    std::filesystem::path(L"openxr_loader.dll"):config_.openxr_loader);
            loaded_xr_=std::make_unique<LoadedOpenXRStereoSession>(library,
                headless_probe?"Outlast native headless probe":nullptr,motion_enabled_);
            if (headless_probe) log("Headset-free native verification: isolated probe runtime; no headset submission");
        }
        hooks_=std::make_unique<NativeHookSet>();instance_=this;
        try {
            desktop_cursor_.emplace(base_,module_extent_);
            if (test_) bind_native(test->bindings);
            else {
                OutlastNativeImage image(module_path(nullptr));
                RuntimeBindings b{};
                if (recommended_render_size_) {
                    if (module_extent_<0x2006f08)
                        throw std::runtime_error("Recovered native engine pointer lies outside the exact retail image");
                    hooks_->add(retail_entry<ResizeFrameEntry>(image,0xc461f0),&resize_frame,original_resize_frame_);
                    hooks_->add(retail_entry<ResizeRHIEntry>(image,0xc44350),&resize_rhi,original_resize_rhi_);
                    log("Engine viewport follows OpenXR recommended eye extent: "+
                        std::to_string(xr().width())+"x"+std::to_string(xr().height())+
                        "; native-resolution mode remains available");
                }
                b.main_viewport=retail_entry<MainViewportEntry>(image,0x62fbc0);
                b.enqueue=retail_entry<EnqueueEntry>(image,0x8d0d10);
                b.constructor=retail_entry<ConstructorEntry>(image,0x8c7670);
                b.dispatch=retail_entry<RendererEntry>(image,0x8d0be0);
                b.normal=retail_entry<RendererEntry>(image,0x8cfc00);
                b.alternate=retail_entry<RendererEntry>(image,0x8cff20);
                b.view=retail_entry<ViewEntry>(image,0xbf7d40);
                b.indexed=retail_entry<IndexedEntry>(image,0xbf5b60);
                b.sphere=retail_entry<SphereEntry>(image,0x148780);
                b.sphere_inside=retail_entry<SphereInsideEntry>(image,0x148850);
                b.crosshair_update=retail_entry<CrosshairUpdateEntry>(image,0xd1d260);
                b.box=retail_entry<BoxEntry>(image,0x148340);
                b.box_inside=retail_entry<BoxInsideEntry>(image,0x148600);
                b.translucency_bounds=retail_entry<TranslucencyBoundsEntry>(image,0x8aaba0);
                wchar_t position_fixture[2]{};
                if (headless_probe && GetEnvironmentVariableW(L"OLVR_PROBE_WINDOW_POSITION",position_fixture,2)==1 &&
                    position_fixture[0]==L'1') {
                    wchar_t folder[32768]{};
                    const auto length=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder,UINT(std::size(folder)));
                    if (!length || length>=std::size(folder))
                        throw std::runtime_error("Roomscale visibility diagnostics require an explicit evidence directory");
                    probe_visibility_.open(std::filesystem::path(std::wstring(folder,length))/L"native-visibility.jsonl");
                    if (!probe_visibility_) throw std::runtime_error("Cannot create native visibility-query evidence");
                    hooks_->add(retail_entry<PrecomputedVisibility>(image,0x8a7810),
                        &probe_precomputed_visibility,original_probe_visibility_);
                }
                b.texture=retail_entry<TextureEntry>(image,0x6050);
                b.surface=retail_entry<SurfaceEntry>(image,0x6280);
                b.batched=retail_entry<BatchedEntry>(image,0x8514b0);
                b.ui=retail_entry<UICommandEntry>(image,0xb754f0);
                menu_movie_=retail_entry<MenuMovieEntry>(image,0xb75880);
                b.camera_base=retail_entry<CameraBaseEntry>(image,0xcedec0);
                b.camera_update=retail_entry<CameraUpdateEntry>(image,0xd03f30);
                b.lean_tick=retail_entry<CameraUpdateEntry>(image,0xd50a30);
                b.hero_cast=retail_entry<HeroCastEntry>(image,0xd50270);
                hooks_->add(retail_entry<ParticleParameters>(image,0x951a90),
                    &particle_parameters,original_particle_parameters_);
                wchar_t camcorder[8]{};
                const auto camcorder_length=GetEnvironmentVariableW(L"OLVR_PROBE_CAMCORDER",camcorder,UINT(std::size(camcorder)));
                wchar_t physical[2]{};
                const auto physical_length=GetEnvironmentVariableW(L"OLVR_PROBE_PHYSICAL_CAMCORDER",physical,2);
                if (physical_length && (!headless_probe || physical_length!=1 || physical[0]!=L'1'))
                    throw std::invalid_argument("Invalid explicit physical-camera diagnostic selection");
                const bool physical_camcorder=motion_enabled_ &&
                    (headless_probe?physical_length!=0:config_.physical_camcorder);
                config_.physical_camcorder=physical_camcorder;
                if (camcorder_length || physical_camcorder) {
                    if (camcorder_length && (!headless_probe || camcorder_length!=1 || camcorder[0]!=L'1'))
                        throw std::invalid_argument("Native lens experiment requires explicit headset-free probe mode");
                    wchar_t folder[32768]{};
                    const auto count=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder,UINT(std::size(folder)));
                    if (headless_probe && (!count || count>=std::size(folder)))
                        throw std::runtime_error("Native lens probe requires an evidence path");
                    NativeCamcorderProbeBindings bindings{};
                    bindings.capture_class=retail_entry<NativeCamcorderProbeBindings::Class>(image,0x41b2f0);
                    bindings.target_class=retail_entry<NativeCamcorderProbeBindings::Class>(image,0x1f75d0);
                    bindings.construct=retail_entry<NativeCamcorderProbeBindings::Construct>(image,0xe2450);
                    bindings.initialize=retail_entry<NativeCamcorderProbeBindings::Initialize>(image,0x2f1520);
                    bindings.attach=retail_entry<NativeCamcorderProbeBindings::Attach>(image,0x65b7d0);
                    bindings.detach=retail_entry<NativeCamcorderProbeBindings::Attach>(image,0x66ddf0);
                    bindings.set_view=retail_entry<NativeCamcorderProbeBindings::SetView>(image,0x6525e0);
                    bindings.projection=retail_entry<NativeCamcorderProbeBindings::Projection>(image,0x652370);
                    bindings.activate_night_vision=retail_entry<NativeCamcorderProbeBindings::Activate>(image,0xd5b010);
                    bindings.local_player=retail_entry<NativeCamcorderProbeBindings::LocalPlayer>(image,0x2f5670);
                    bindings.show_flags=retail_entry<NativeCamcorderProbeBindings::ShowFlags>(image,0x655720);
                    bindings.close_movie=&close_camera_hud_movie;
                    bindings.set_scale=retail_entry<NativeCamcorderProbeBindings::SetScale>(image,0xb732a0);
                    bindings.get_scale=retail_entry<NativeCamcorderProbeBindings::GetScale>(image,0xeaa3a0);
                    bindings.errors=reinterpret_cast<void**>(base_+0x1ec1dd0);
                    bindings.engine=reinterpret_cast<void**>(base_+0x2020f38);
                    wchar_t night_vision[8]{};
                    const auto nv_length=GetEnvironmentVariableW(L"OLVR_PROBE_CAMCORDER_NV",night_vision,UINT(std::size(night_vision)));
                    if (nv_length && (nv_length!=1 || night_vision[0]!=L'1'))
                        throw std::invalid_argument("Invalid diagnostic camcorder night-vision selection");
                    bindings.night_vision_experiment=nv_length!=0;
                    bindings.native_postprocess=camcorder_length==0;
                    if (camcorder_length) {
                        camcorder_probe_=std::make_unique<NativeCamcorderProbe>(bindings,std::wstring(folder,count));
                        log("Diagnostic independent native lens capture enabled");
                    } else {
                        camcorder_capture_=std::make_unique<NativeCamcorderProbe>(bindings,
                            headless_probe?std::filesystem::path(std::wstring(folder,count)):std::filesystem::path{});
                        raise_camcorder_=retail_entry<CamcorderRaise>(image,0xd781a0);
                        lower_camcorder_=retail_entry<CamcorderLower>(image,0xd6a540);
                        normal_camera_effect_=retail_entry<CamcorderLower>(image,0xd5b350);
                        enable_camera_component_=retail_entry<CamcorderEnable>(image,0x1d8570);
                        camcorder_engine_=bindings.engine;
                        camcorder_mesh_=std::make_unique<NativeCamcorderMesh>(NativeCamcorderMeshBindings{
                            retail_entry<BoneNameEntry>(image,0xcbc70),
                            retail_entry<BoneIndexEntry>(image,0x6e92b0),
                            retail_entry<NativeCamcorderMeshBindings::Attach>(image,0x704f30),
                            retail_entry<NativeCamcorderMeshBindings::Matrix>(image,0x24f3a0),
                            retail_entry<NativeCamcorderMeshBindings::Update>(image,0x34e440)});
                        find_script_function_=retail_entry<FindScriptFunction>(image,0xce240);
                        hooks_->add(retail_entry<MeshVisibility>(image,0xccb4e0),
                            &mesh_visibility,original_mesh_visibility_);
                        hooks_->add(retail_entry<CamcorderNV>(image,0xd5b010),
                            &camera_night_effect,original_nv_effect_);
                        hooks_->add(retail_entry<CamcorderLower>(image,0xd5b710),
                            &camera_color_effect,original_camera_effect_);
                        hooks_->add(retail_entry<CamcorderLower>(image,0xcee960),
                            &camera_hud_show,original_camera_hud_);
                        hooks_->add(retail_entry<FinishView>(image,0x8b3900),
                            &finish_view,original_finish_view_);
                        hooks_->add(retail_entry<ShaderTexture>(image,0x168dd0),
                            &shader_texture,original_shader_texture_);
                        hooks_->add(retail_entry<CaptureProxies>(image,0x661e70),
                            &capture_proxies,original_capture_proxies_);
                        hooks_->add(retail_entry<UberProxy>(image,0x30b9d0),
                            &uber_proxy,original_uber_proxy_);
                        hooks_->add(retail_entry<MaterialProxy>(image,0x544c70),
                            &material_proxy,original_material_proxy_);
                        movie_display_root_=retail_entry<DisplayRoot>(image,0xe97370);
                        hooks_->add(retail_entry<UICommandEntry>(image,0xb7cc70),
                            &offscreen_movie,original_offscreen_movie_);
                        hooks_->add(retail_entry<MovieDisplay>(image,0xe7f070),
                            &movie_display,original_movie_display_);
                        if (headless_probe) {
                            wchar_t stock_fx[8]{};
                            const auto stock_count=GetEnvironmentVariableW(L"OLVR_PROBE_STOCK_CAMERA_FX",
                                stock_fx,UINT(std::size(stock_fx)));
                            if (stock_count && (stock_count!=1 || stock_fx[0]!=L'1'))
                                throw std::invalid_argument("Invalid stock-camera full-scene positive control");
                            stock_camera_fx_control_=stock_count!=0;
                            if (stock_camera_fx_control_)
                                log("Headset-free stock full-scene camera FX positive control; not production acceptance");
                            native_postprocess_dispatch_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"native-postprocess-dispatch.jsonl");
                            if (!native_postprocess_dispatch_)
                                throw std::runtime_error("Cannot open actual native postprocess dispatch evidence");
                            native_postprocess_factory_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"native-postprocess-factory.jsonl");
                            if (!native_postprocess_factory_)
                                throw std::runtime_error("Cannot open native color-grading factory evidence");
                            native_effect_visibility_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"native-effect-visibility.jsonl");
                            if (!native_effect_visibility_)
                                throw std::runtime_error("Cannot open native effect visibility evidence");
                            hooks_->add(retail_entry<PostprocessDispatch>(image,0x89f530),
                                &postprocess_dispatch,original_postprocess_dispatch_);
                            hooks_->add(retail_entry<UberRender>(image,0x329510),
                                &uber_render,original_uber_render_);
                            hooks_->add(retail_entry<BoundShaders>(image,0x2d0930),
                                &bound_shaders,original_bound_shaders_);
                            camcorder_events_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"physical-camcorder-events.jsonl");
                            if (!camcorder_events_) throw std::runtime_error("Cannot open physical camcorder evidence");
                            camcorder_mesh_events_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"native-camera-mesh.jsonl");
                            if (!camcorder_mesh_events_) throw std::runtime_error("Cannot open native camera mesh evidence");
                            camcorder_gpu_events_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"physical-camcorder-gpu.jsonl");
                            if (!camcorder_gpu_events_) throw std::runtime_error("Cannot open native lens viewport evidence");
                            camcorder_lifecycle_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"native-camcorder-lifecycle.jsonl");
                            if (!camcorder_lifecycle_) throw std::runtime_error("Cannot open camera lifecycle evidence");
                            camcorder_composition_.open(std::filesystem::path(std::wstring(folder,count))/
                                L"native-camcorder-composition.jsonl");
                            if (!camcorder_composition_) throw std::runtime_error("Cannot open physical screen composition evidence");
                            wchar_t physical[8]{};
                            const auto length=GetEnvironmentVariableW(L"OLVR_PROBE_PHYSICAL_CAMCORDER",
                                physical,UINT(std::size(physical)));
                            if (length) {
                                if (length!=1 || physical[0]!=L'1')
                                    throw std::invalid_argument("Invalid physical camera lifecycle fixture");
                                request_camcorder_gc_=retail_entry<CamcorderNV>(image,0x5192d0);
                                camcorder_resolve_probe_=std::make_unique<NativeMonoResolveProbe>(std::wstring(folder,count));
                            }
                        }
                        log("Physical chest-grab camcorder enabled; independent native lens, screen-only NV");
                    }
                    hooks_->add(retail_entry<CollectGarbage>(image,0xf8d90),
                        &collect_garbage,original_collect_garbage_);
                }
                if (!physical_camcorder)
                    log("Simple full-view night-vision filter: right thumbstick click; physical camcorder disabled");
                if (motion_enabled_) {
                    world_trace_=retail_entry<NativeWorldTrace>(image,0x506920);
                    hooks_->add(retail_entry<NativeUseEntry>(image,0xce8f10),&native_pickup,original_pickup_);
                    hooks_->add(retail_entry<NativeUseEntry>(image,0xd7b300),&native_use_volume,original_use_volume_);
                    if (module_extent_<0x200b5b0)
                        throw std::runtime_error("Recovered collision world lies outside the exact retail image");
                    native_world_=reinterpret_cast<void**>(base_+0x200b5a8);
                    b.input_tick=retail_entry<InputTickEntry>(image,0xc4cf90);
                    b.gamepad_state=retail_entry<GamepadStateEntry>(image,0xe6b4ac);
                    b.gamepad_vibration=retail_entry<GamepadVibrationEntry>(image,0xe6b4a6);
                    b.viewport_focus=retail_entry<ViewportFocusEntry>(image,0xc427e0);
                    if (module_extent_<0x201e31c)
                        throw std::runtime_error("Native gamepad globals lie outside the exact retail image");
                    b.gamepads=reinterpret_cast<void**>(base_+0x201e310);
                    b.gamepad_count=reinterpret_cast<INT*>(base_+0x201e318);
                }
                menu_state_=retail_entry<MenuStateEntry>(image,0xd14e90);
                bind_native(b);
                if (motion_enabled_ || probe_skeleton_.is_open()) {
                    bone_name_=retail_entry<BoneNameEntry>(image,0xcbc70);
                    bone_index_=retail_entry<BoneIndexEntry>(image,0x6e92b0);
                    bone_matrix_=retail_entry<BoneMatrixEntry>(image,0x6ec730);
                    hooks_->add(retail_entry<SkeletonEvaluateEntry>(image,0x6ffc90),
                        &skeleton_evaluate,original_skeleton_evaluate_);
                }
                if (motion_enabled_ || headless_probe)
                    hooks_->add(retail_entry<FocusPauseEntry>(image,0x43a480),
                        &focus_pause,original_focus_pause_);
                if (headless_probe) {
                    if (!motion_enabled_) {
                        const auto address=GetProcAddress(GetModuleHandleW(L"OutlastVRHeadlessProbe.dll"),
                            "OutlastVRProbe_StartupPadV1");
                        if (!address) throw std::runtime_error("Headless menu confirmation lacks its versioned startup source");
                        static_assert(sizeof(probe_startup_pad_)==sizeof(address));
                        std::memcpy(&probe_startup_pad_,&address,sizeof(address));
                        gamepads_=reinterpret_cast<void**>(base_+0x201e310);
                        gamepad_count_=reinterpret_cast<INT*>(base_+0x201e318);
                        hooks_->add(retail_entry<InputTickEntry>(image,0xc4cf90),
                            &probe_input_tick,original_input_tick_);
                        hooks_->add(retail_entry<GamepadStateEntry>(image,0xe6b4ac),
                            &gamepad_state,original_gamepad_state_);
                    }
                    hooks_->add(retail_entry<InputEnqueueEntry>(image,0xc44570),
                        &probe_input_enqueue,original_input_enqueue_);
                    if (probe_menu_fixture_) {
                        hooks_->add(retail_entry<InputConsumeEntry>(image,0xc49b90),
                            &probe_menu_consume,original_menu_consume_);
                        hooks_->add(retail_entry<MenuKeyEntry>(image,0xcc76e0),
                            &probe_menu_key,original_menu_key_);
                    }
                    hooks_->add(&GetKeyState,&probe_key_state,original_key_state_);
                }
                const UINT offsets[]{0x1e09e58,0x1e09e64,0x1e0a234};
                for (unsigned i=0;i<3;++i) {
                    if (offsets[i]>module_extent_-4) throw std::runtime_error("Native policy lies outside the exact image");
                    saved_policy_[i]=field<UINT>(reinterpret_cast<void*>(base_),offsets[i]);
                }
                for (const auto offset:offsets) {const UINT disabled=0;std::memcpy(reinterpret_cast<void*>(base_+offset),&disabled,4);}
                policies_=true;
            }
            com_hook(xr().graphics().get(),16,&create_device);
            com_hook(xr().graphics().get(),4,&adapter_count);
            com_hook(xr().graphics().get(),5,&adapter_identifier);
            com_hook(xr().graphics().get(),6,&mode_count);com_hook(xr().graphics().get(),7,&enum_mode);
            com_hook(xr().graphics().get(),8,&display_mode);com_hook(xr().graphics().get(),9,&check_type);
            com_hook(xr().graphics().get(),10,&check_format);com_hook(xr().graphics().get(),11,&check_samples);
            com_hook(xr().graphics().get(),12,&check_depth);com_hook(xr().graphics().get(),13,&check_conversion);
            com_hook(xr().graphics().get(),14,&adapter_caps);com_hook(xr().graphics().get(),15,&adapter_monitor);
            hooks_->enable_pending();
            if (!test_) patch_factory_import();
            stats_.initialized=1;log("Exact-build native renderer hooks initialized; no dual-eye geometry replay");
            if (!test_) {
                render_heartbeat_=input_heartbeat_=GetTickCount64();
                if (!CreateTimerQueueTimer(&watchdog_,nullptr,&watch_progress,this,5000,5000,WT_EXECUTEDEFAULT))
                    throw std::runtime_error("Cannot start native gameplay stall diagnostics");
            }
        } catch (...) {
            restore_input_imports();
            if (factory_iat_ && saved_factory_) {
                DWORD old=0,ignored=0;
                if (!VirtualProtect(factory_iat_,sizeof(void*),PAGE_READWRITE,&old)) std::terminate();
                InterlockedExchangePointer(factory_iat_,saved_factory_);
                if (!VirtualProtect(factory_iat_,sizeof(void*),old,&ignored)) std::terminate();
            }
            if (policies_) {
                const UINT offsets[]{0x1e09e58,0x1e09e64,0x1e0a234};
                for (unsigned i=0;i<3;++i) std::memcpy(reinterpret_cast<void*>(base_+offsets[i]),&saved_policy_[i],4);
            }
            instance_=nullptr;throw;
        }
    }
    ~Runtime() {
        if (watchdog_ && !DeleteTimerQueueTimer(nullptr,watchdog_,INVALID_HANDLE_VALUE))
            std::terminate();
        Internal internal;
        restore_input_imports();
        if (frame_open_) {
            try {xr().cancel_frame();} catch (const std::exception& e) {log(e.what());}
        }
        if (factory_iat_ && saved_factory_) {
            DWORD old=0,ignored=0;
            if (!VirtualProtect(factory_iat_,sizeof(void*),PAGE_READWRITE,&old)) std::terminate();
            InterlockedExchangePointer(factory_iat_,saved_factory_);
            if (!VirtualProtect(factory_iat_,sizeof(void*),old,&ignored)) std::terminate();
        }
        hooks_.reset();
        if (policies_) {
            const UINT offsets[]{0x1e09e58,0x1e09e64,0x1e0a234};
            for (unsigned i=0;i<3;++i) std::memcpy(reinterpret_cast<void*>(base_+offsets[i]),&saved_policy_[i],4);
        }
        capture_gamma_shader_.reset();
        variants_.reset();final_surface_.reset();submission_surface_.reset();desktop_.reset();
        final_.reset();submission_.reset();device_.reset();instance_=nullptr;
    }
    IDirect3D9* graphics() {auto* result=xr().graphics().get();result->AddRef();return result;}
    RuntimeStatistics statistics() {
        GraphicsScope graphics(*this);
        if (variants_) {
            stats_.state_block_allocations=variants_->state_allocations();
            stats_.state_block_captures=variants_->state_captures();
        }
        return stats_;
    }
    HWND probe_window() {
        GraphicsScope graphics(*this);
        if (!headless_probe || !device_.get())
            throw std::logic_error("Native input window requires the attached headset-free device");
        D3DDEVICE_CREATION_PARAMETERS parameters{};
        gpu_check(device_->GetCreationParameters(&parameters),"Read native game focus window");
        DWORD process=0;
        GetWindowThreadProcessId(parameters.hFocusWindow,&process);
        if (!parameters.hFocusWindow || process!=GetCurrentProcessId())
            throw std::runtime_error("Native game focus window is not owned by this probe");
        return parameters.hFocusWindow;
    }
    bool render_extent(void* viewport,UINT& width,UINT& height) {
        const auto engine=field<void*>(reinterpret_cast<void*>(base_),0x2006f00);
        const auto game_viewport=engine?field<void*>(engine,0x6f4):nullptr;
        return outlast_render_size(field<void*>(viewport,0x4c),game_viewport,width,height,xr().width(),xr().height());
    }
    static void WINAPI resize_frame(void* viewport,UINT width,UINT height,INT fullscreen,INT x,INT y) {
        guarded([&] {
            auto& r=self();
            if (width && height && r.render_extent(viewport,width,height)) fullscreen=0;
            r.original_resize_frame_(viewport,width,height,fullscreen,x,y);
        });
    }
    static void WINAPI resize_rhi(void* viewport,UINT width,UINT height,INT fullscreen,INT save) {
        guarded([&] {
            auto& r=self();
            if (width && height && r.render_extent(viewport,width,height)) {
                fullscreen=0;
                log("Owned native logical/RHI viewport resized to "+std::to_string(width)+"x"+std::to_string(height));
            }
            r.original_resize_rhi_(viewport,width,height,fullscreen,save);
        });
    }
    bool test_mode() const {return test_;}
    void complete_probe() {
        if (!headless_probe || test_) throw std::logic_error("Only the actual headset-free probe can seal its evidence");
        auto close=[](std::ofstream& stream) {
            if (!stream.is_open()) return;
            stream.flush();stream.close();
            if (!stream) throw std::runtime_error("Cannot seal complete native diagnostic evidence");
        };
        {
            std::lock_guard<std::mutex> lock(camera_pose_mutex_);
            close(probe_camera_poses_);close(probe_camera_updates_);close(probe_lean_weights_);
        }
        {
            std::lock_guard<std::mutex> lock(skeleton_mutex_);
            close(probe_skeleton_);
        }
        {
            std::lock_guard<std::mutex> lock(contact_log_mutex_);
            close(probe_hand_contacts_);close(probe_hand_recovery_);
        }
        {
            std::lock_guard<std::mutex> lock(probe_input_mutex_);
            close(probe_input_events_);
        }
        close(probe_cameras_);
    }
};
Runtime* runtime=nullptr;
DWORD initialize(const RuntimeTestConfiguration* test) {
    std::lock_guard<std::mutex> lock(initialization_mutex);
    try {
        if (runtime) throw std::logic_error("Native mod was initialized twice");
        const auto folder=module_path(own_module).parent_path()/L"logs";
        std::filesystem::create_directories(folder);
        logfile.open(folder/L"OutlastVRSinglePass.log",std::ios::app);
        if (!logfile) throw std::runtime_error("Cannot create mod runtime log");
        auto created=std::make_unique<Runtime>(test);
        if (!test) {
            HMODULE pinned=nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&initialize),&pinned))
                throw std::runtime_error("Cannot pin the live native-hook module");
        }
        runtime=created.release();last_error.clear();return 0;
    } catch (const std::exception& error) {last_error=error.what();log("Initialization refused: "+last_error);return ERROR_DLL_INIT_FAILED;}
}
}

extern "C" __declspec(dllexport) DWORD WINAPI OutlastVR_Initialize(void*) {return initialize(nullptr);}
extern "C" __declspec(dllexport) DWORD WINAPI OutlastVR_TestInitialize(const RuntimeTestConfiguration* config) {return initialize(config);}
extern "C" __declspec(dllexport) IDirect3D9* WINAPI OutlastVR_TestGraphics() {
    if (!runtime) return nullptr;
    return runtime->graphics();
}
extern "C" __declspec(dllexport) BOOL WINAPI OutlastVR_Statistics(RuntimeStatistics* output) {
    if (!runtime || !output || output->size!=sizeof(*output)) return FALSE;
    *output=runtime->statistics();return TRUE;
}
extern "C" __declspec(dllexport) HWND WINAPI OutlastVR_ProbeWindow() {
    return guarded([] {
        if (!runtime) throw std::logic_error("Native probe window query preceded initialization");
        return runtime->probe_window();
    });
}
extern "C" __declspec(dllexport) void WINAPI OutlastVR_CompleteProbe() {
    guarded([] {
        if (!runtime) throw std::logic_error("Native probe completion preceded initialization");
        runtime->complete_probe();
    });
}
extern "C" __declspec(dllexport) DWORD WINAPI OutlastVR_Shutdown(void*) {
    std::lock_guard<std::mutex> lock(initialization_mutex);
    if (runtime && !runtime->test_mode()) return ERROR_BUSY;
    delete runtime;runtime=nullptr;return 0;
}
extern "C" __declspec(dllexport) const char* WINAPI OutlastVR_LastError() {return last_error.c_str();}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {own_module=module;DisableThreadLibraryCalls(module);}
    return TRUE;
}
