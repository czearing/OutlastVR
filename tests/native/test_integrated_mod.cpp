#define OLVR_OPENXR_MOCK_ONLY
#include "test_openxr_session.cpp"
#include "outlast_runtime_api.h"
#include "outlast_native_image.h"
#include "outlast_target_roles.h"
#include <d3dcompiler.h>
#include <thread>
#include <exception>
#include <cwchar>

namespace {
using Initialize=DWORD(WINAPI*)(const RuntimeTestConfiguration*);
using Shutdown=DWORD(WINAPI*)(void*);
using Graphics=IDirect3D9*(WINAPI*)();
using Statistics=BOOL(WINAPI*)(RuntimeStatistics*);
using LastError=const char*(WINAPI*)();
Com<IDirect3DDevice9> integrated_device;
std::array<unsigned char,0x100> renderer_data{};
std::array<unsigned char,0x12c0> view_data{};
std::array<unsigned char,0x3cc> rhi_data{};
std::array<unsigned char,32> native_stats{};
IndexedEntry native_indexed=nullptr;
Com<IDirect3DIndexBuffer9> geometry_indices;
Com<IDirect3DVertexBuffer9> geometry_vertices,object_records;
Com<IDirect3DVertexDeclaration9> geometry_declaration,replay_declaration;
Com<IDirect3DVertexShader9> original_vertex;
Com<IDirect3DPixelShader9> original_pixel;
std::vector<float> vertex_values;
unsigned scene_number=0;
bool fault_dual=false;
bool fault_mono=false;
bool fault_flat_mono=false;
bool material_constants=false;
unsigned cull_checks=0,nonfinite_spheres=0;
unsigned crosshair_updates=0;
unsigned translucency_checks=0;
float crosshair_elapsed=0;
bool thread_handoff=false,thread_overlap=false,fault_reset_thread=false,fault_reset_reentrant=false;
bool ui_owned=false;
bool ui_scaleform=false;
std::array<unsigned char,0x3300> camera_hero{};
std::array<unsigned char,0x240> camera_object{};
std::array<unsigned char,0x100> camera_mesh{};
std::array<unsigned char,0x150> lean_node{};
std::array<unsigned char,0xa80> native_engine{};
std::array<unsigned char,0x450> native_effects{};
std::array<unsigned char,0xa90> native_controller{};
void* native_engine_pointer=native_engine.data();
std::array<UINT,3> native_night_lights{0,0,1};
std::array<std::array<unsigned char,0x280>,3> native_light_components{};
std::array<unsigned char,0xa80> recreated_engine{};
unsigned light_ticks=0,light_pose_calls=0;
std::array<UINT,4> native_effect_calls{};
DWORD native_effect_thread=0;
using MoviePlaying=INT(WINAPI*)(void*,const wchar_t*);
std::array<MoviePlaying,9> native_movie_vtable{};
struct {MoviePlaying* vtable;} native_movie_object{native_movie_vtable.data()};
void* native_movie_player=&native_movie_object;
const wchar_t* native_movie_name=L"Intro";
unsigned native_movie_queries=0;
std::array<float,6> camera_base_pose{};
float native_lean_weight=0;
std::array<unsigned char,500*4> native_gamepads{};
void* gamepad_data=native_gamepads.data();
INT gamepad_count=4;
XINPUT_STATE observed_gamepad{};
unsigned motion_polls=0;
void* observed_skeleton=nullptr;
float observed_skeleton_elapsed=0,native_animation_time=0;
UINT observed_skeleton_secondary=0;
ViewportFocusEntry joystick_focus_site=nullptr;
INT observed_joystick_focus=0;
INT actual_viewport_focus=0;
std::array<unsigned char,0xc0> focus_viewport{},foreign_viewport{};
HANDLE render_entered=nullptr,present_requested=nullptr,present_finished=nullptr;
XrResult XRAPI_CALL mono_locate(XrSession session,const XrViewLocateInfo* info,XrViewState* state,
    uint32_t capacity,uint32_t* count,XrView* output) {
    const auto result=locate(session,info,state,capacity,count,output);
    if (result==XR_SUCCESS && (!fault_flat_mono || mock.flat_frame)) {
        output[1].pose=output[0].pose;output[1].fov=output[0].fov;
    }
    return result;
}
XrResult XRAPI_CALL mono_resolve(XrInstance instance,const char* name,PFN_xrVoidFunction* output) {
    if (std::strcmp(name,"xrLocateViews")==0) {export_function(output,&mono_locate);return XR_SUCCESS;}
    return resolve(instance,name,output);
}
void require(bool condition,const std::string& text) {if (!condition) throw std::runtime_error(text);}
void check_desktop_pacing() {
    Com<IDirect3DSwapChain9> chain;
    gpu_check(integrated_device->GetSwapChain(0,chain.put()),"Read actual native presentation chain");
    D3DPRESENT_PARAMETERS actual{};
    gpu_check(chain->GetPresentParameters(&actual),"Read actual native presentation interval");
    require(actual.PresentationInterval==D3DPRESENT_INTERVAL_IMMEDIATE,
        "Desktop mirror retained an independent vsync wait instead of OpenXR-owned pacing");
}
template<class T> void write(void* object,std::size_t offset,T value) {
    std::memcpy(static_cast<unsigned char*>(object)+offset,&value,sizeof(value));
}
template<class T> T exported(HMODULE dll,const char* name) {
    auto address=GetProcAddress(dll,name);T result=nullptr;
    std::memcpy(&result,&address,sizeof(result));
    require(result!=nullptr,std::string("DLL export is missing: ")+name);return result;
}
#define NATIVE __attribute__((noinline,noclone,noipa))
NATIVE INT WINAPI movie_playing_stub(void* player,const wchar_t* name) {
    require(player==&native_movie_object && name && std::wcscmp(name,L"LoadingScreen")==0,
        "Production loading query changed native movie identity, slot or name ABI");
    ++native_movie_queries;
    return std::wcscmp(native_movie_name,name)==0?1:0;
}
NATIVE DWORD WINAPI gamepad_state_stub(DWORD index,XINPUT_STATE* output) {
    asm volatile("nop;nop;nop;nop;nop");
    if (index==1 && output) {
        *output={};output->dwPacketNumber=123;output->Gamepad.wButtons=XINPUT_GAMEPAD_DPAD_LEFT;
        return ERROR_SUCCESS;
    }
    return ERROR_DEVICE_NOT_CONNECTED;
}
NATIVE DWORD WINAPI gamepad_vibration_stub(DWORD index,XINPUT_VIBRATION*) {
    asm volatile("nop;nop;nop;nop;nop");
    return index==1?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
NATIVE INT WINAPI viewport_focus_stub(void*) {
    asm volatile("nop;nop;nop;nop;nop");
    return actual_viewport_focus;
}
class JoystickFocusFixture {
    void* memory_=nullptr;
    HWND window_=nullptr;
public:
    JoystickFocusFixture() {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        memory_=VirtualAlloc(reinterpret_cast<void*>(base+0xc40000),0x10000,
            MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        require(memory_==reinterpret_cast<void*>(base+0xc40000),
            "Cannot reserve the exact recovered joystick focus callsite fixture");
        auto* code=static_cast<unsigned char*>(memory_)+0x7add;
        const unsigned char prefix[]{0x48,0x83,0xec,0x28,0x48,0xb8};
        std::memcpy(code,prefix,sizeof(prefix));
        const auto target=reinterpret_cast<std::uintptr_t>(&viewport_focus_stub);
        std::memcpy(code+6,&target,sizeof(target));
        const unsigned char suffix[]{0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};
        std::memcpy(code+14,suffix,sizeof(suffix));
        DWORD old=0;
        require(VirtualProtect(memory_,0x10000,PAGE_EXECUTE_READ,&old)!=FALSE &&
            FlushInstructionCache(GetCurrentProcess(),memory_,0x10000)!=FALSE,
            "Cannot seal the recovered joystick focus callsite fixture");
        joystick_focus_site=reinterpret_cast<ViewportFocusEntry>(code);
        window_=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Owned joystick focus fixture",
            WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window_!=nullptr,"Cannot create the owned unfocused joystick viewport fixture");
        write(focus_viewport.data(),0xb4,window_);
        write(foreign_viewport.data(),0xb4,GetDesktopWindow());
    }
    ~JoystickFocusFixture() {
        joystick_focus_site=nullptr;
        if (!DestroyWindow(window_) || !VirtualFree(memory_,0,MEM_RELEASE)) std::terminate();
    }
};
NATIVE void WINAPI input_tick_stub(void*,float) {
    asm volatile("nop;nop;nop;nop;nop");
    if (native_engine_pointer) {
        UINT flags=0;std::memcpy(&flags,static_cast<BYTE*>(native_engine_pointer)+0x3f8,4);
        require(flags==0xa5a50000,"VR pacing retained smoothing or overwrote unrelated engine flags");
    }
    UINT connected=0;std::memcpy(&connected,native_gamepads.data()+0x1f0,4);
    require(connected==1,"Virtual controller failed native cached-disconnection startup gate");
    observed_joystick_focus=joystick_focus_site(focus_viewport.data());
    require(joystick_focus_site(foreign_viewport.data())==0,
        "Joystick focus override changed a foreign viewport");
    require(viewport_focus_stub(nullptr)==0,"Joystick focus override leaked to an unrelated native callsite");
    require(gamepad_state_stub(0,&observed_gamepad)==ERROR_SUCCESS,"Native game did not receive virtual controller zero");
    XINPUT_STATE physical{};
    require(gamepad_state_stub(1,&physical)==ERROR_SUCCESS && physical.dwPacketNumber==123 &&
        physical.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_LEFT,"Native motion hook changed unrelated controller");
    ++motion_polls;
}
NATIVE void WINAPI camera_base_stub(void*,float* output) {
    asm volatile("nop;nop;nop;nop;nop");
    std::memcpy(output,camera_base_pose.data(),sizeof(camera_base_pose));
}
NATIVE void WINAPI camera_update_stub(void* camera,float) {
    asm volatile("nop;nop;nop;nop;nop");
    std::array<float,6> pose{};
    camera_base_stub(camera,pose.data());
    std::memcpy(static_cast<unsigned char*>(camera)+0x68,pose.data(),sizeof(pose));
}
NATIVE void WINAPI lean_tick_stub(void* node,float) {
    asm volatile("nop;nop;nop;nop;nop");
    write(node,0x140,native_lean_weight);
}
NATIVE void WINAPI skeleton_stub(void* component,float elapsed,UINT secondary) {
    asm volatile("nop;nop;nop;nop;nop");
    observed_skeleton=component;
    observed_skeleton_elapsed=elapsed;
    observed_skeleton_secondary=secondary;
    native_animation_time+=elapsed;
}
NATIVE void* WINAPI hero_cast_stub(void* actor) {
    // Native helper calls may overwrite volatile floating-point argument registers.
    asm volatile("pxor %%xmm1, %%xmm1" ::: "xmm1");
    return actor==camera_hero.data()?actor:nullptr;
}
UINT64 WINAPI menu_state_stub() {return 0;}
void check_native_effect(void* effects) {
    require(effects==native_effects.data() && GetCurrentThreadId()==native_effect_thread,
        "Native night vision changed effect ownership or gameplay thread");
}
void WINAPI night_effect_stub(void* effects,INT powered) {
    check_native_effect(effects);
    require(powered==1,"Default night vision used depleted native postprocessing");
    write(effects,0x3b8,BYTE(2));++native_effect_calls[0];
}
void WINAPI normal_effect_stub(void* effects) {
    check_native_effect(effects);
    write(effects,0x3b8,BYTE(0));++native_effect_calls[1];
}
void WINAPI camera_effect_stub(void* effects) {
    check_native_effect(effects);
    write(effects,0x3b8,BYTE(1));++native_effect_calls[2];
}
void WINAPI night_light_stub(void* light,UINT active) {
    const auto found=std::find_if(native_light_components.begin(),native_light_components.end(),
        [&](const auto& candidate) {return candidate.data()==light;});
    require(GetCurrentThreadId()==native_effect_thread && found!=native_light_components.end(),
        "Native night vision changed light ownership or gameplay thread");
    native_night_lights[found-native_light_components.begin()]=active;++native_effect_calls[3];
}
NATIVE void WINAPI light_tick_stub(void* hero,float elapsed) {
    asm volatile("nop;nop;nop;nop;nop");
    require(hero==camera_hero.data() && elapsed==.015f,"Native light tick ABI changed");
    ++light_ticks;
    write(native_light_components[0].data(),0x210,Vec3{-700,-800,-900});
}
float* WINAPI actor_matrix_stub(void* hero,float* output) {
    require(hero==camera_hero.data() && GetCurrentThreadId()==native_effect_thread,
        "Headset light matrix requested outside its native owner thread");
    INT yaw=0;Vec3 root{};
    std::memcpy(&yaw,static_cast<BYTE*>(hero)+0x90,4);
    std::memcpy(&root,static_cast<BYTE*>(hero)+0x80,sizeof(root));
    const float radians=float(double(yaw)*6.2831853071795864769/65536);
    const Matrix4 matrix{std::cos(radians),std::sin(radians),0,0,
        -std::sin(radians),std::cos(radians),0,0,0,0,1,0,root.x,root.y,root.z,1};
    std::copy(matrix.begin(),matrix.end(),output);
    return output;
}
void WINAPI light_rotation_stub(void* light,const INT* rotation) {
    require(light==native_light_components[0].data() && GetCurrentThreadId()==native_effect_thread,
        "Headset pose changed a foreign native light or gameplay thread");
    write(light,0x26c,std::array<INT,3>{rotation[0],rotation[1],rotation[2]});
    ++light_pose_calls;
}
NATIVE int WINAPI sphere_stub(const void*,const float*,const float*) {asm volatile("nop;nop;nop;nop;nop");return 0;}
NATIVE int WINAPI sphere_inside_stub(const void*,const float*,const float*,int* inside) {asm volatile("nop;nop;nop;nop;nop");*inside=0;return 0;}
NATIVE void WINAPI crosshair_update_stub(void* hud,float elapsed) {
    asm volatile("nop;nop;nop;nop;nop");
    ++crosshair_updates;crosshair_elapsed=elapsed;
    write(hud,0x6e4,.75f);write(hud,0x6ec,.25f);
}
NATIVE int WINAPI box_stub(const void*,const float*,const float*) {asm volatile("nop;nop;nop;nop;nop");return 0;}
NATIVE int WINAPI inside_stub(const void*,const float*,const float*,int* inside) {asm volatile("nop;nop;nop;nop;nop");*inside=0;return 0;}
NATIVE void* WINAPI surface_stub(void* output,UINT width,UINT height,BYTE format,
    std::uintptr_t texture,UINT flags,std::uintptr_t name) {
    require(format==11 && !texture && flags==9 && name==0x1357,
        "Direct native depth hook changed an original ABI argument");
    gpu_check(integrated_device->CreateDepthStencilSurface(width,height,D3DFMT_D24S8,
        D3DMULTISAMPLE_NONE,0,TRUE,static_cast<IDirect3DSurface9**>(output),nullptr),
        "Allocate direct native depth without an associated texture");
    return output;
}
class NativeDepthFixture {
    unsigned char* memory_=nullptr;
public:
    NativeDepthFixture() {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        memory_=static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(base+0x8d0000),
            0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        require(memory_==reinterpret_cast<unsigned char*>(base+0x8d0000),
            "Cannot reserve isolated allocation-callsite fixture outside the surrogate image");
        const unsigned char prefix[]{
            0x48,0x83,0xec,0x48,
            0x48,0x8b,0x44,0x24,0x70,0x48,0x89,0x44,0x24,0x20,
            0x48,0x8b,0x44,0x24,0x78,0x48,0x89,0x44,0x24,0x28,
            0x48,0x8b,0x84,0x24,0x80,0,0,0,0x48,0x89,0x44,0x24,0x30};
        for (const UINT site:{0x8da420,0x8da4f1,0x8da5b3,0x8df000}) {
            auto* entry=memory_+site-0x8d0000-64;
            std::memset(entry,0x90,59);std::memcpy(entry,prefix,sizeof(prefix));
            entry[59]=0xe8;
            const auto delta=reinterpret_cast<std::intptr_t>(&surface_stub)-
                reinterpret_cast<std::intptr_t>(entry+64);
            require(delta>=INT32_MIN && delta<=INT32_MAX,"Allocation fixture call is outside rel32 reach");
            const auto displacement=std::int32_t(delta);
            std::memcpy(entry+60,&displacement,4);
            const unsigned char end[]{0x48,0x83,0xc4,0x48,0xc3};
            std::memcpy(entry+64,end,sizeof(end));
        }
        const unsigned char description[]{
            0x48,0x83,0xec,0x28,0x48,0x8b,0x01,0xff,0x50,0x60,
            0x48,0x83,0xc4,0x28,0xc3};
        std::memcpy(memory_,description,sizeof(description));
        DWORD old=0;
        require(VirtualProtect(memory_,0x10000,PAGE_EXECUTE_READ,&old)!=FALSE &&
            FlushInstructionCache(GetCurrentProcess(),memory_,0x10000)!=FALSE,
            "Cannot publish isolated native allocation-callsite fixture");
    }
    ~NativeDepthFixture() {
        if (memory_ && !VirtualFree(memory_,0,MEM_RELEASE)) std::terminate();
    }
    void check() const {
        using Description=HRESULT(WINAPI*)(IDirect3DSurface9*,D3DSURFACE_DESC*);
        const auto physical=reinterpret_cast<Description>(memory_);
        for (const UINT site:{0x8da420,0x8da4f1,0x8da5b3,0x8df000}) {
            const auto call=reinterpret_cast<SurfaceEntry>(memory_+site-0x8d0000-64);
            Com<IDirect3DSurface9> depth;
            auto* slot=depth.put();
            require(call(slot,256,256,11,0,9,0x1357)==slot,"Native depth allocation return value changed");
            D3DSURFACE_DESC actual{},logical{};
            gpu_check(physical(depth.get(),&actual),"Read physical direct depth extent outside the game image");
            gpu_check(depth->GetDesc(&logical),"Read engine-visible direct depth extent");
            const bool classified=site!=0x8df000;
            require(actual.Width==(classified?512u:256u) && actual.Height==256,
                "Direct native depth surface did not cover both atlas halves or changed shared depth");
            require(logical.Width==256 && logical.Height==256,
                "Direct depth allocation exposed physical stereo dimensions to the engine");
            OutlastAtlasTag tag{};
            require(atlas_tag(*depth.get(),tag)==classified &&
                (!classified || tag.role==OutlastTargetRole::scene_depth),
                "Direct depth surface has incorrect native role ownership");
        }
        std::cout<<"PASS: three MCP-recovered direct-depth return sites; physical_width=512; logical_width=256;"
            <<" no_texture_handle; native_ABI_preserved; shared_depth_unchanged\n";
    }
};
NATIVE void WINAPI view_stub(void*,const void* view) {
    gpu_check(integrated_device->SetVertexShaderConstantF(0,
        reinterpret_cast<const float*>(static_cast<const unsigned char*>(view)+0x200),4),
        "Upload original native surrogate view");
}
NATIVE INT WINAPI translucency_bounds_stub(void*,UINT,RECT* bounds) {
    asm volatile("nop;nop;nop;nop;nop");
    *bounds=scene_number%2?RECT{}:RECT{96,96,128,128};
    return scene_number%2?0:1;
}
void render_contents() {
    const bool owned=!mock.ui || ui_owned;
    const RECT native_bounds=scene_number%2?RECT{}:RECT{96,96,128,128};
    RECT bounds{};
    const auto visible=translucency_bounds_stub(renderer_data.data(),scene_number%4,&bounds);
    require(visible==(owned?1:scene_number%2?0:1) &&
        bounds.left==(owned?0:native_bounds.left) && bounds.top==(owned?0:native_bounds.top) &&
        bounds.right==(owned?256:native_bounds.right) && bounds.bottom==(owned?256:native_bounds.bottom),
        "Tracked translucency retained empty/partial mono bounds or changed an unowned renderer");
    ++translucency_checks;
    if (owned) {
        const auto before=view_data;
        const std::array<INT,4> subviewport{16,24,192,160};
        write(view_data.data(),0x6c,subviewport);
        require(translucency_bounds_stub(renderer_data.data(),0,&bounds)==1 &&
            bounds.left==16 && bounds.top==24 && bounds.right==208 && bounds.bottom==184,
            "Tracked translucency bounds lost a native subviewport origin or extent");
        write(view_data.data(),0x6c,std::array<INT,4>{0,0,0,256});
        require(translucency_bounds_stub(renderer_data.data(),0,&bounds)==(scene_number%2?0:1) &&
            std::memcmp(&bounds,&native_bounds,sizeof(bounds))==0,
            "Tracked translucency bounds turned an empty native viewport into a visible pass");
        write(view_data.data(),0x6c,std::array<INT,4>{0,0,256,256});
        require(view_data==before,"Translucency bounds changed native camera/view fields");
        translucency_checks+=2;
    }
    int foreign_renderer=0;
    require(translucency_bounds_stub(&foreign_renderer,0,&bounds)==(scene_number%2?0:1) &&
        std::memcmp(&bounds,&native_bounds,sizeof(bounds))==0,
        "Tracked translucency bounds escaped native renderer ownership");
    ++translucency_checks;
    std::array<unsigned char,0x84c> hud{};
    hud.fill(0x5a);
    auto expected_hud=hud;
    write(expected_hud.data(),0x6e4,0.f);write(expected_hud.data(),0x6ec,.25f);
    crosshair_update_stub(hud.data(),.015f);
    require(hud==expected_hud && crosshair_elapsed==.015f,
        "Crosshair suppression skipped native updates, changed another HUD field or retained the reticle");
    auto& device=*integrated_device.get();
    view_stub(rhi_data.data(),view_data.data());
    const D3DVIEWPORT9 full{0,0,256,256,0,1};
    gpu_check(device.SetViewport(&full),"Restore full native fixture viewport before clear");
    gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear integrated stereo frame");
    if (mock.ui) {
        const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        gpu_check(device.SetVertexShaderConstantF(0,identity,4),"Upload native orthographic menu transform");
        mock.ui_viewport=scene_number%3?RECT{0,0,256,256}:RECT{24,16,224,240};
        const auto& v=mock.ui_viewport;
        const D3DVIEWPORT9 canvas{UINT(v.left),UINT(v.top),UINT(v.right-v.left),UINT(v.bottom-v.top),0,1};
        gpu_check(device.SetViewport(&canvas),"Exercise native menu subviewport placement");
        mock.ui_scissor=scene_number%2!=0;
        gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,mock.ui_scissor),"Exercise native menu clipping");
        const RECT clip{30,32,196,224};
        gpu_check(device.SetScissorRect(&clip),"Set native menu clipping bounds");
    }
    gpu_check(device.SetVertexShader(original_vertex.get()),"Bind integrated original vertex shader");
    gpu_check(device.SetPixelShader(original_pixel.get()),"Bind integrated original pixel shader");
    gpu_check(device.SetVertexDeclaration(geometry_declaration.get()),"Bind integrated original declaration");
    gpu_check(device.SetStreamSource(0,geometry_vertices.get(),0,24),"Bind integrated original geometry stream");
    gpu_check(device.SetStreamSource(3,nullptr,0,0),"Clear previous native instance source");
    gpu_check(device.SetIndices(geometry_indices.get()),"Bind integrated original indices");
    gpu_check(device.BeginScene(),"Begin integrated native scene");
    Com<IDirect3DQuery9> empty_query;
    gpu_check(device.CreateQuery(D3DQUERYTYPE_OCCLUSION,empty_query.put()),"Create native zero-visibility control");
    gpu_check(empty_query->Issue(D3DISSUE_BEGIN),"Begin native zero-visibility control");
    gpu_check(empty_query->Issue(D3DISSUE_END),"End native zero-visibility control");
    DWORD empty_samples=UINT_MAX;
    HRESULT query_status=S_FALSE;
    const auto query_start=GetTickCount64();
    while (query_status==S_FALSE && GetTickCount64()-query_start<5000) {
        query_status=empty_query->GetData(&empty_samples,sizeof(empty_samples),D3DGETDATA_FLUSH);
        if (query_status==S_FALSE) Sleep(1);
    }
    require(query_status==S_OK && empty_samples==0,
        "Production renderer synthesized visibility for a completed zero-sample GPU occlusion query");
    if (!mock.ui && mock.frame%5) {
        const float center[]{0,0,200,0},radius=5,extent[]{5,5,5};
        int inside=0;
        require(sphere_stub(view_data.data()+0x3b0,center,&radius)==1,"Owned native sphere stayed mono-culled");
        require(sphere_inside_stub(view_data.data()+0x3b0,center,&radius,&inside)==1 && inside==1,
            "Owned native sphere containment stayed mono-culled");
        require(sphere_inside_stub(view_data.data()+0x800,center,&radius,&inside)==0 && inside==0,
            "Unowned native sphere containment was changed");
        require(box_stub(view_data.data()+0x3b0,center,extent)==1,"Owned native box stayed mono-culled");
        require(inside_stub(view_data.data()+0x3b0,center,extent,&inside)==1 && inside==1,
            "Owned native full-inside classification is incorrect");
        require(sphere_stub(view_data.data()+0x800,center,&radius)==0,"Unowned volume was changed");
        const std::uint32_t sentinel_bits=0xffffffffu;
        float sentinel=0;
        std::memcpy(&sentinel,&sentinel_bits,sizeof(sentinel));
        const float sentinel_center[]{sentinel,sentinel,sentinel,0};
        require(sphere_stub(view_data.data()+0x3b0,sentinel_center,&sentinel)==1,
            "Production sphere hook rejected the captured all-FFFFFFFF native bound");
        require(sphere_inside_stub(view_data.data()+0x3b0,sentinel_center,&sentinel,&inside)==1 && inside==1,
            "Production sphere containment hook rejected the captured all-FFFFFFFF native bound");
        ++nonfinite_spheres;
        cull_checks+=8;
    }
    mock.counter->reset();mock.expected_instances=2;
    switch (scene_number%5) {
    case 0:
        gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,4,vertex_values.data(),24),"Integrated native UP draw");
        break;
    case 1:
        gpu_check(device.DrawPrimitive(D3DPT_TRIANGLELIST,0,4),"Integrated native nonindexed draw");
        break;
    case 2:
        gpu_check(device.DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,12,0,4),"Integrated native indexed draw");
        break;
    case 3: {
        const WORD indices[]{0,1,2,3,4,5,6,7,8,9,10,11};
        gpu_check(device.DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,12,4,indices,D3DFMT_INDEX16,
            vertex_values.data(),24),"Integrated native indexed UP draw");
        break;
    }
    default: {
        const UINT count=2+(scene_number%3);
        gpu_check(device.SetVertexDeclaration(replay_declaration.get()),"Bind real native replay declaration");
        gpu_check(device.SetStreamSource(3,object_records.get(),0,0),"Bind original native software replay source");
        write(rhi_data.data(),0x284,count);write(rhi_data.data(),0x288,UINT(1<<3));
        write(rhi_data.data(),0x28c+3*0x14,reinterpret_cast<std::uintptr_t>(object_records.get())+8);
        write(rhi_data.data(),0x294+3*0x14,UINT(32));write(rhi_data.data(),0x298+3*0x14,UINT(0));
        native_indexed(rhi_data.data(),reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(geometry_indices.get())+8),
            0,0,0,12,0,4);
        mock.expected_instances=count*2;
        UINT state=0;std::memcpy(&state,rhi_data.data()+0x284,4);
        require(state==1,"Integrated replay bypassed the original retail cleanup");
        break;
    }
    }
    if (fault_dual)
        gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,4,vertex_values.data(),24),"Deliberate integrated dual draw");
    gpu_check(device.EndScene(),"End integrated native scene");
    ++scene_number;
}
NATIVE void WINAPI render_stub(void*) {
    if (fault_reset_reentrant) {
        D3DPRESENT_PARAMETERS parameters{};
        integrated_device->Reset(&parameters);
        throw std::runtime_error("Reentrant reset was not rejected");
    }
    if (thread_overlap) {
        require(SetEvent(render_entered)!=0,"Cannot signal renderer ownership");
        require(WaitForSingleObject(present_requested,5000)==WAIT_OBJECT_0,"Concurrent Present was not requested");
        require(WaitForSingleObject(present_finished,100)==WAIT_TIMEOUT,
            "Present escaped graphics serialization while the renderer was still active");
    }
    render_contents();
}
NATIVE std::uint64_t WINAPI batched_stub(void* data,const void* transform,UINT width,
    std::uintptr_t height,UINT flags,std::uint32_t gamma_bits) {
    require(data==renderer_data.data() && transform && width==256 && height==256 &&
        flags==0x1357 && gamma_bits==0x3f800000,"Native Canvas hook changed an original ABI argument");
    render_contents();return 0x12345678abcdef00ull;
}
NATIVE std::uint64_t WINAPI ui_stub(void* command) {
    require(command==renderer_data.data(),"Native Scaleform hook changed the queued command");
    render_contents();return 0x28;
}
NATIVE void WINAPI dispatch_stub(void* renderer) {render_stub(renderer);}
NATIVE void* WINAPI constructor_stub(void* renderer,const void*,std::uint64_t,const void*,UINT) {
    asm volatile("nop;nop;nop;nop;nop");return renderer;
}
NATIVE void WINAPI enqueue_stub(void*,const void* family) {
    constructor_stub(renderer_data.data(),family,0,nullptr,0);
}
NATIVE void WINAPI main_stub(void* viewport,float*,void* family) {enqueue_stub(viewport,family);}

class RetailFixture {
    unsigned char* data_=nullptr;
public:
    explicit RetailFixture(const std::filesystem::path& executable) {
        const auto bytes=outlast_proof_body(executable,0xbf5b60,627,
            "ff6939df22849635bde7285b764f3d7f5c6fa5f53f5a0d991dcbcd98049ff263");
        data_=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        require(data_!=nullptr,"Cannot allocate owned retail RHI body");
        std::memcpy(data_,bytes.data(),bytes.size());const std::int32_t delta=4096-0x52;
        std::memcpy(data_+0x4e,&delta,4);DWORD old=0;
        require(VirtualProtect(data_,4096,PAGE_EXECUTE_READ,&old)!=0 &&
            FlushInstructionCache(GetCurrentProcess(),data_,bytes.size())!=0,"Cannot publish owned retail RHI body");
        native_indexed=reinterpret_cast<IndexedEntry>(data_);
    }
    ~RetailFixture(){if (data_ && !VirtualFree(data_,0,MEM_RELEASE)) std::terminate();}
};
void create_geometry(IDirect3DDevice9& device) {
    std::string vertex=R"(
        row_major float4x4 ViewProjectionMatrix : register(c0);
        struct Out {float4 position:POSITION;float3 color:TEXCOORD0;};
        Out main(float3 position:POSITION,float3 color:COLOR0) {
            Out result;result.position=mul(float4(position,1),ViewProjectionMatrix);result.color=color;return result;
        })";
    if (mock.ui && !ui_owned) {
        const std::string name="ViewProjectionMatrix";
        for (auto offset=vertex.find(name);offset!=std::string::npos;offset=vertex.find(name))
            vertex.replace(offset,name.size(),"Transform");
    }
    const char* pixel="float4 main(float3 color:TEXCOORD0):COLOR0 {return float4(color,1);}";
    if (material_constants) pixel=R"(
        float4 CameraPositionPS:register(c6);
        float4 FogCameraPosition:register(c12);
        float4 main(float3 color:TEXCOORD0):COLOR0 {
            float3 error=FogCameraPosition.xyz-CameraPositionPS.xyz;
            return float4(lerp(color,float3(1,0,1),step(.0001,dot(error,error))),1);
        })";
    auto compile=[](const char* source,const char* profile,Com<ID3DBlob>& code) {
        Com<ID3DBlob> errors;
        const auto status=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",profile,
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),errors.put());
        if (FAILED(status)) throw std::runtime_error(errors.get()?static_cast<const char*>(errors->GetBufferPointer()):"Shader compile failed");
    };
    Com<ID3DBlob> vs,ps;compile(vertex.c_str(),"vs_3_0",vs);compile(pixel,"ps_3_0",ps);
    gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),original_vertex.put()),"Create original integrated VS");
    gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),original_pixel.put()),"Create original integrated PS");
    const D3DVERTEXELEMENT9 declaration[]{
        {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
    const D3DVERTEXELEMENT9 replay[]{
        {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},
        {3,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},D3DDECL_END()};
    gpu_check(device.CreateVertexDeclaration(declaration,geometry_declaration.put()),"Create original integrated declaration");
    gpu_check(device.CreateVertexDeclaration(replay,replay_declaration.put()),"Create original retail replay declaration");
    const float corners[6][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    for (UINT group=0;group<2;++group) {
        const float z=mock.ui?.3f:group==0?200.f:600.f;
        const float x=mock.ui?(group==0?-.4f:.45f):group==0?-60.f:90.f;
        const float y=mock.ui?(group==0?-.25f:.35f):group==0?-35.f:60.f;
        for (const auto& corner:corners) {
            const float radius=mock.ui?.13f:z*.1f;
            const float values[]{x+corner[0]*radius,y+corner[1]*radius,z,group==0?1.f:0.f,group==1?1.f:0.f,0};
            vertex_values.insert(vertex_values.end(),std::begin(values),std::end(values));
        }
    }
    void* mapped=nullptr;
    gpu_check(device.CreateVertexBuffer(UINT(vertex_values.size()*4),0,0,D3DPOOL_MANAGED,geometry_vertices.put(),nullptr),"Create integrated geometry buffer");
    gpu_check(geometry_vertices->Lock(0,0,&mapped,0),"Lock integrated geometry");
    std::memcpy(mapped,vertex_values.data(),vertex_values.size()*4);gpu_check(geometry_vertices->Unlock(),"Unlock integrated geometry");
    gpu_check(device.CreateIndexBuffer(24,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,geometry_indices.put(),nullptr),"Create integrated indices");
    gpu_check(geometry_indices->Lock(0,0,&mapped,0),"Lock integrated indices");
    for (WORD i=0;i<12;++i) static_cast<WORD*>(mapped)[i]=i;
    gpu_check(geometry_indices->Unlock(),"Unlock integrated indices");
    gpu_check(device.CreateVertexBuffer(128,0,0,D3DPOOL_MANAGED,object_records.put(),nullptr),"Create integrated replay objects");
    gpu_check(object_records->Lock(0,0,&mapped,0),"Lock integrated replay objects");
    std::memset(mapped,0,128);gpu_check(object_records->Unlock(),"Unlock integrated replay objects");
    gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable integrated fixture depth");
    gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable integrated fixture face culling");
}
}

int main(int argc,char** argv) {
    std::cout.setf(std::ios::unitbuf);std::cerr.setf(std::ios::unitbuf);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HMODULE dll=nullptr;Shutdown shutdown=nullptr;
    std::unique_ptr<RetailFixture> native;
    std::unique_ptr<JoystickFocusFixture> focus;
    Com<IDirect3D9> factory;
    try {
        const std::string mode=argc==4?argv[3]:"";
        require(argc==3 || (argc==4 && (mode=="--fault-dual" || mode=="--fault-mono" ||
            mode=="--thread-handoff" || mode=="--thread-overlap" || mode=="--ui" ||
            mode=="--ui-in-renderer" || mode=="--fault-ui-mono" || mode=="--fault-ui-dual" ||
            mode=="--ui-scaleform" || mode=="--ui-scaleform-handoff" ||
            mode=="--camera-tilt" || mode=="--fault-camera-tilt" ||
            mode=="--camera-bob" || mode=="--fault-camera-bob" ||
            mode=="--camera-handoff" || mode=="--fault-camera-handoff" ||
            mode=="--fault-containment" || mode=="--fault-crosshair" || mode=="--fault-translucency-bounds" ||
            mode=="--fault-reset-thread" || mode=="--fault-reset-reentrant" || mode=="--fault-joystick-focus" ||
            mode=="--floor-startup" || mode=="--comfort" || mode=="--comfort-fault-scale" ||
            mode=="--comfort-fault-seated" || mode=="--comfort-fault-handedness" ||
            mode=="--comfort-fault-turn" || mode=="--snap" || mode=="--fault-snap" ||
            mode=="--flat-transitions" || mode=="--flat-upload" || mode=="--fault-flat-mono" ||
            mode=="--ipd-cycle" || mode=="--input-chords" || mode=="--input-recovery" ||
            mode=="--reset-retry" || mode=="--flat-wrap" || mode=="--snap-recovery" ||
            mode=="--loading-blackout" || mode=="--loading-upload-blackout" ||
            mode=="--fault-loading-blackout" || mode=="--material-constants" ||
            mode=="--fault-material-constants" || mode=="--fault-headset-light" ||
            mode=="--fault-frame-pacing")),
            "Usage: test_integrated_mod.exe mod.dll retail.exe [--fault-dual|--fault-mono|--thread-handoff|--thread-overlap|--ui|--fault-reset-thread|--fault-reset-reentrant]");
        fault_dual=mode=="--fault-dual" || mode=="--fault-ui-dual";
        fault_flat_mono=mode=="--fault-flat-mono";
        material_constants=mode=="--material-constants" || mode=="--fault-material-constants";
        fault_mono=mode=="--fault-mono" || mode=="--fault-ui-mono" || fault_flat_mono;
        const bool loading_blackout=mode=="--loading-blackout" || mode=="--loading-upload-blackout" ||
            mode=="--fault-loading-blackout";
        const bool flat_upload=mode=="--flat-upload" || mode=="--loading-upload-blackout";
        const bool input_chords=mode=="--input-chords";
        const bool input_recovery=mode=="--input-recovery";
        const bool reset_retry=mode=="--reset-retry";
        const bool flat_wrap=mode=="--flat-wrap";
        mock.flat_test=mode=="--flat-transitions" || flat_upload || fault_flat_mono || flat_wrap || loading_blackout;
        mock.dynamic_ipd=mode=="--ipd-cycle";
        thread_handoff=mode=="--thread-handoff" || mode=="--ui-scaleform-handoff";
        thread_overlap=mode=="--thread-overlap";
        fault_reset_thread=mode=="--fault-reset-thread";fault_reset_reentrant=mode=="--fault-reset-reentrant";
        ui_owned=mode=="--ui-in-renderer";
        ui_scaleform=mode=="--ui-scaleform" || mode=="--ui-scaleform-handoff";
        mock.ui=mode=="--ui" || ui_owned || ui_scaleform || mode=="--fault-ui-mono" || mode=="--fault-ui-dual";
        native=std::make_unique<RetailFixture>(std::filesystem::path(argv[2]));
        focus=std::make_unique<JoystickFocusFixture>();
        {D3D9On12Factory inventory;mock.luid=inventory.device()->GetAdapterLuid();}
        mock.geometry=true;mock.recentered=true;
        mock.floor_startup_test=mode=="--floor-startup";
        mock.stage_supported=mock.floor_startup_test;
        dll=LoadLibraryW(std::filesystem::path(argv[1]).c_str());
        require(dll!=nullptr,"Cannot load integrated mod DLL");
        const auto init=exported<Initialize>(dll,"OutlastVR_TestInitialize");
        const auto graphics=exported<Graphics>(dll,"OutlastVR_TestGraphics");
        const auto statistics=exported<Statistics>(dll,"OutlastVR_Statistics");
        const auto error=exported<LastError>(dll,"OutlastVR_LastError");
        shutdown=exported<Shutdown>(dll,"OutlastVR_Shutdown");
        RuntimeTestConfiguration config{};
        const bool snap_recovery=mode=="--snap-recovery";
        const bool snap_mode=mode=="--snap" || mode=="--fault-snap" || snap_recovery;
        if (snap_mode) {
            config.comfort.snap_turn_degrees=mode=="--fault-snap"?0.f:15.f;
            mock.snap_test=true;
        }
        const bool comfort_mode=mode.rfind("--comfort",0)==0;
        if (comfort_mode) {
            config.comfort.seated=mode!="--comfort-fault-seated";
            config.comfort.left_handed=mode!="--comfort-fault-handedness";
            config.comfort.snap_turn_degrees=mode=="--comfort-fault-turn"?0.f:30.f;
            config.comfort.world_scale=mode=="--comfort-fault-scale"?1.f:2.f;
            mock.comfort_seated=true;
            mock.comfort_world_scale=2.f;
        }
        const bool camera_tilt=mode=="--camera-tilt" || mode=="--fault-camera-tilt";
        const bool camera_handoff=mode=="--camera-handoff" || mode=="--fault-camera-handoff";
        const bool camera_bob=mode=="--camera-bob" || mode=="--fault-camera-bob" || camera_handoff;
        mock.upright_camera=camera_tilt || camera_bob || comfort_mode;
        const float pitch=camera_tilt?.35f:0.f,roll=camera_tilt?.18f:0.f;
        const TrackingFrame frame{{0,0,0},
            {std::cos(roll),std::sin(roll)*std::cos(pitch),std::sin(roll)*std::sin(pitch)},
            {-std::sin(roll),std::cos(roll)*std::cos(pitch),std::cos(roll)*std::sin(pitch)},
            {0,std::sin(pitch),-std::cos(pitch)},100,{0,0,0}};
        if (mode=="--fault-camera-tilt") config.world_up=frame.up;
        config.xr_resolve=fault_mono?&mono_resolve:&resolve;
        config.bindings={&main_stub,&enqueue_stub,&constructor_stub,&dispatch_stub,&render_stub,
            nullptr,&view_stub,native_indexed,&sphere_stub,&box_stub,&inside_stub,nullptr,&surface_stub,&batched_stub,&ui_stub,
            &sphere_inside_stub,&crosshair_update_stub};
        config.bindings.camera_base=&camera_base_stub;
        config.bindings.camera_update=&camera_update_stub;
        config.bindings.lean_tick=&lean_tick_stub;
        config.bindings.hero_cast=&hero_cast_stub;
        config.bindings.skeleton_evaluate=&skeleton_stub;
        config.bindings.night_vision_engine=&native_engine_pointer;
        config.bindings.menu_state=&menu_state_stub;
        config.bindings.night_effect=&night_effect_stub;
        config.bindings.normal_effect=&normal_effect_stub;
        config.bindings.camera_effect=&camera_effect_stub;
        config.bindings.night_light=&night_light_stub;
        if (mode!="--fault-frame-pacing") config.bindings.frame_rate_engine=&native_engine_pointer;
        if (mode!="--fault-headset-light") {
            config.bindings.night_light_tick=&light_tick_stub;
            config.bindings.actor_matrix=&actor_matrix_stub;
            config.bindings.light_rotation=&light_rotation_stub;
        }
        native_movie_vtable[8]=&movie_playing_stub;
        config.bindings.movie_player=&native_movie_player;
        config.bindings.input_tick=&input_tick_stub;
        config.bindings.gamepad_state=&gamepad_state_stub;
        config.bindings.gamepad_vibration=&gamepad_vibration_stub;
        config.bindings.gamepads=&gamepad_data;
        config.bindings.gamepad_count=&gamepad_count;
        config.bindings.viewport_focus=mode=="--fault-joystick-focus"?nullptr:&viewport_focus_stub;
        config.bindings.translucency_bounds=mode=="--fault-translucency-bounds"?nullptr:&translucency_bounds_stub;
        write(native_gamepads.data(),8,INT(3));
        mock.motion_focus_test=!input_chords;
        write(camera_object.data(),0x60,camera_hero.data());
        write(camera_object.data(),0x118,UINT(2));
        write(camera_hero.data(),0x360,165.f);
        write(camera_hero.data(),0x444,camera_mesh.data());
        write(camera_hero.data(),0x2bbc,native_controller.data());
        write(native_controller.data(),0xa84,UINT(2));
        write(native_engine.data(),0xa74,native_effects.data());
        write(native_engine.data(),0x3f8,UINT(0xa5a50200));
        recreated_engine=native_engine;
        write(native_effects.data(),0x3b4,UINT(0x5a5a1234));
        for (unsigned light=0;light<native_night_lights.size();++light)
            write(camera_hero.data(),0x2d40+light*sizeof(void*),native_light_components[light].data());
        native_effect_thread=GetCurrentThreadId();
        write(camera_mesh.data(),0x78,camera_hero.data());
        write(lean_node.data(),0x84,camera_mesh.data());
        native_lean_weight=mode=="--fault-camera-bob"?1.f:0.f;
        write(camera_hero.data(),0x30b0,native_lean_weight);
        if (mode=="--fault-containment") config.bindings.sphere_inside=nullptr;
        if (mode=="--fault-crosshair") config.bindings.crosshair_update=nullptr;
        const auto initialized=init(&config);
        require(initialized==0,std::string("Integrated DLL initialization failed: ")+error());
        std::array<unsigned char,0x400> skeleton{};
        float expected_animation_time=0;
        for (void* owner:{static_cast<void*>(nullptr),static_cast<void*>(camera_hero.data())}) {
            write(skeleton.data(),0x78,owner);
            for (const float elapsed:{1.f/60,.25f,0.f,1.f/30}) {
                const UINT secondary=elapsed==0?0:1;
                skeleton_stub(skeleton.data(),elapsed,secondary);
                expected_animation_time+=elapsed;
                require(observed_skeleton==skeleton.data() &&
                    observed_skeleton_elapsed==elapsed && observed_skeleton_secondary==secondary &&
                    native_animation_time==expected_animation_time,
                    "Production skeleton hook corrupted native animation delta time or arguments");
            }
        }
        std::cout<<"PASS: native_skeleton_delta_forwarded; hero_and_NPC; volatile_XMM1_clobbered; "
            <<"positive_and_zero_time; original_secondary_preserved\n";
        actual_viewport_focus=1;
        require(joystick_focus_site(focus_viewport.data())==1,
            "Joystick focus hook changed an originally focused native viewport");
        actual_viewport_focus=0;
        native_engine_pointer=nullptr;
        input_tick_stub(nullptr,0);
        UINT deferred_flags=0;std::memcpy(&deferred_flags,native_engine.data()+0x3f8,4);
        require(deferred_flags==0xa5a50200,"Missing engine changed pacing state during startup");
        native_engine_pointer=native_engine.data();
        require(observed_gamepad.Gamepad.wButtons==0 && observed_gamepad.Gamepad.sThumbLX==0,
            "Native virtual controller was not neutral before first XR frame");
        motion_polls=0;
        *factory.put()=graphics();
        require(factory->GetAdapterCount()==1,"The engine adapter inventory differs from the XR-selected device");
        D3DADAPTER_IDENTIFIER9 identifier{};
        gpu_check(factory->GetAdapterIdentifier(0,0,&identifier),"Read XR-selected logical adapter");
        D3DCAPS9 caps{};gpu_check(factory->GetDeviceCaps(0,D3DDEVTYPE_HAL,&caps),"Read XR-selected native adapter caps");
        require(caps.VertexShaderVersion>=D3DVS_VERSION(3,0) && caps.PixelShaderVersion>=D3DPS_VERSION(3,0),
            "The virtual adapter does not preserve the real SM3 capability contract");
        HiddenWindow window(256,256);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.BackBufferWidth=p.BackBufferHeight=256;
        p.BackBufferFormat=D3DFMT_A8R8G8B8;p.hDeviceWindow=window.get();
        const DWORD creation_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING|
            ((thread_handoff || thread_overlap)?D3DCREATE_MULTITHREADED:0);
        gpu_check(factory->CreateDevice(0,D3DDEVTYPE_HAL,window.get(),creation_flags,
            &p,integrated_device.put()),"Create device through the integrated mod hook");
        check_desktop_pacing();
        {NativeDepthFixture depth;depth.check();}
        if (thread_handoff || thread_overlap) {
            D3DDEVICE_CREATION_PARAMETERS created{};
            gpu_check(integrated_device->GetCreationParameters(&created),"Read cross-thread device creation contract");
            require((created.BehaviorFlags&D3DCREATE_MULTITHREADED)!=0,"Cross-thread native device lacks D3D9 multithread protection");
        }
        if (thread_overlap) {
            render_entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            present_requested=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            present_finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            require(render_entered && present_requested && present_finished,"Cannot create explicit overlap barriers");
        }
        create_geometry(*integrated_device.get());
        Com<IDirect3DTexture9> stale_atlas;
        gpu_check(integrated_device->CreateTexture(16,8,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,stale_atlas.put(),nullptr),"Create retained unused atlas sampler fixture");
        tag_atlas_texture(*stale_atlas.get(),OutlastTargetRole::scene_color,8,8);
        D3DSURFACE_DESC logical{};
        gpu_check(stale_atlas->GetLevelDesc(0,&logical),"Read native logical atlas description");
        require(logical.Width==8 && logical.Height==8,"Native texture description exposed the physical atlas extent");
        gpu_check(integrated_device->SetTexture(13,stale_atlas.get()),"Retain an atlas on an undeclared native sampler");
        IsolatedD3D12DrawCounter counter(*mock.device,true,mock.queue);mock.counter=&counter;
        write(renderer_data.data(),0x68,view_data.data());write(renderer_data.data(),0x70,UINT(1));
        write(view_data.data(),0x6c,std::array<INT,4>{0,0,256,256});
        write(rhi_data.data(),0x34,integrated_device.get());write(rhi_data.data(),0x10,native_stats.data());
        write(rhi_data.data(),0x18,UINT(1));
        const auto matrix=eye_constants(frame,{{0,0,0},{0,0,0,1}},{-.8f,.85f,.75f,-.7f},1,10000);
        std::array<float,16> native_matrix{};
        for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column)
            native_matrix[row*4+column]=matrix[column*4+row];
        std::memcpy(view_data.data()+0x200,native_matrix.data(),64);
        int viewport=0,family=0;
        motion_mock::fake.neutral_controls=true;
        motion_mock::fake.fixture_aim_tracked=!mock.flat_test;
        motion_mock::fake.fixture_use=.75f;
        motion_mock::fake.fixture_grab={.25f,.75f};
        motion_mock::fake.fixture_move=comfort_mode?XrVector2f{-.4f,.6f}:XrVector2f{.3f,.6f};
        motion_mock::fake.fixture_turn=comfort_mode?XrVector2f{.3f,.6f}:
            XrVector2f{-.4f,input_chords?0.f:.6f};
        unsigned crouch_edges=0;
        BYTE resumed_effect_mode=0;
        unsigned scripted_effect_checks=0;
        unsigned loading_mirror_checks=0;
        bool head_light_checked=false;
        Com<IDirect3DTexture9> flat_texture,offscreen_atlas;
        Com<IDirect3DSurface9> upload_source,flat_readback;
        for (unsigned f=0;f<20;++f) {
            mock.flat_frame=mock.flat_test && (f%5==1 || f%5==2);
            mock.loading_blackout_frame=loading_blackout && f%5>=1 && (f/5)%2==0;
            native_movie_name=mock.loading_blackout_frame && mode!="--fault-loading-blackout"?
                L"LoadingScreen":L"Intro";
            native_movie_player=loading_blackout && f==6?nullptr:&native_movie_object;
            const bool flat_only=mock.flat_test && f%5==1;
            mock.flat_after_world=mock.flat_frame && !flat_only;
            if (snap_mode) motion_mock::fake.fixture_turn.x=f==1?1.f:0.f;
            if (snap_recovery) {
                motion_mock::fake.axis_active[1]=f!=6;
                if (f==6 || f==7) motion_mock::fake.fixture_turn.x=1;
            }
            motion_mock::fake.fixture_crouch=!comfort_mode && f!=6 && f!=12;
            motion_mock::fake.fixture_reload=comfort_mode && f!=6 && f!=12;
            if (!input_chords) {
                const bool fresh=f==1 || f==7 || f==13;
                motion_mock::fake.fixture_jump=motion_mock::fake.fixture_nv=!comfort_mode && fresh;
                motion_mock::fake.fixture_notes=motion_mock::fake.fixture_sprint=comfort_mode && fresh;
            }
            if (input_recovery) {
                const bool held=f!=0 && f!=6 && f!=12 && f!=18;
                motion_mock::fake.fixture_notes=motion_mock::fake.fixture_jump=held;
                motion_mock::fake.fixture_reload=motion_mock::fake.fixture_crouch=held;
                motion_mock::fake.fixture_menu=held;
                motion_mock::fake.fixture_sprint=motion_mock::fake.fixture_nv=false;
                motion_mock::fake.fixture_use=0;
                motion_mock::fake.fixture_grab={};
                motion_mock::fake.fixture_move=motion_mock::fake.fixture_turn={};
            } else if (input_chords) {
                const unsigned step=f+1;
                motion_mock::fake.fixture_sprint=step==2 || step==6 || step==7 ||
                    (step>=17 && step<=19);
                motion_mock::fake.fixture_nv=step==2 || step==3 || step==6 || step==9;
                motion_mock::fake.fixture_crouch=step==12 || step==13;
                motion_mock::fake.fixture_reload=step==12;
                motion_mock::fake.menu_button=step<11 || step>=16;
                motion_mock::fake.face_buttons=step<16;
                motion_mock::fake.fixture_move=step==17?XrVector2f{0,1}:
                    step==18?XrVector2f{}:step==19?XrVector2f{0,-1}:XrVector2f{.3f,.6f};
            }
            if (camera_bob) {
                auto animated=frame;
                animated.origin=f && (!camera_handoff || f%10!=4)?
                    Vec3{10*std::sin(float(f)),12*std::sin(float(f)*.7f),
                                       6*std::sin(float(f)*.8f)}:Vec3{};
                camera_base_pose[0]=animated.origin.x;
                camera_base_pose[1]=animated.origin.y;
                camera_base_pose[2]=animated.origin.z;
                if (camera_handoff) {
                    const bool unlinked=f%10==2 || f%10==3;
                    const bool forged=unlinked && mode=="--fault-camera-handoff";
                    write(camera_hero.data(),0x7bc,BYTE(unlinked && !forged?2:0));
                    write(camera_hero.data(),0x7bd,BYTE(unlinked && !forged?70:0));
                    write(camera_object.data(),0x118,UINT(unlinked && !forged?0:2));
                    if (unlinked) animated.origin={42,-73,5};
                    mock.native_camera_offset_m=unlinked?XrVector3f{.42f,-.73f,.05f}:XrVector3f{};
                    if (forged) {
                        camera_base_pose[0]=animated.origin.x;
                        camera_base_pose[1]=animated.origin.y;
                        camera_base_pose[2]=animated.origin.z;
                    }
                }
                const auto moved=eye_constants(animated,{{0,0,0},{0,0,0,1}},
                    {-.8f,.85f,.75f,-.7f},1,10000);
                for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column)
                    native_matrix[row*4+column]=moved[column*4+row];
                std::memcpy(view_data.data()+0x200,native_matrix.data(),64);
                write(view_data.data(),0x3a0,animated.origin);
            }
            lean_tick_stub(lean_node.data(),.015f);
            const bool scripted_effect=(f>=2 && f<=4) || (camera_handoff && f>=12 && f<=14);
            if (f==2 || (camera_handoff && f==12)) resumed_effect_mode=native_effects[0x3b8];
            if (scripted_effect)
                native_effects[0x3b8]=f%10==2?3:f%10==3?4:255;
            else if (f==5 || (camera_handoff && f==15)) native_effects[0x3b8]=resumed_effect_mode;
            const auto effects_before=native_effects;
            const auto lights_before=native_night_lights;
            const auto calls_before=native_effect_calls;
            camera_update_stub(camera_object.data(),.015f);
            if (scripted_effect) {
                require(native_effects==effects_before && native_night_lights==lights_before &&
                    native_effect_calls==calls_before,
                    "Night vision overwrote a native scripted camera effect or light");
                ++scripted_effect_checks;
            } else if (f==5 || (camera_handoff && f==15)) {
                require(scripted_effect_checks==(f==5?3u:6u),
                    "Native camera updates did not survive every scripted effect");
                std::cout<<"PASS: native_scripted_effects_preserved; modes=3,4,255; "
                    <<"production_camera_update_continued\n";
            }
            if (f==10) {
                flat_texture.reset();
                offscreen_atlas.reset();
                upload_source.reset();flat_readback.reset();
                gpu_check(integrated_device->SetTexture(13,nullptr),"Unbind the owned default-pool fixture before reset");
                stale_atlas.reset();
                if (fault_reset_thread) {
                    std::thread resetter([&]{integrated_device->Reset(&p);});resetter.join();
                    throw std::runtime_error("Wrong-thread reset was not rejected");
                }
                p.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
                if (reset_retry) {
                    auto invalid=p;invalid.BackBufferCount=UINT_MAX;
                    require(integrated_device->Reset(&invalid)==D3DERR_INVALIDCALL,
                        "Invalid reset parameters did not preserve the native failure");
                    require(integrated_device->TestCooperativeLevel()==D3DERR_DEVICENOTRESET,
                        "A failed native reset did not preserve recoverable device status");
                }
                gpu_check(integrated_device->Reset(&p),"Reset through native atlas and XR recovery hooks");
                check_desktop_pacing();
                gpu_check(integrated_device->SetRenderState(D3DRS_ZENABLE,FALSE),"Restore fixture depth after reset");
                gpu_check(integrated_device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Restore fixture culling after reset");
            }
            if ((!mock.ui || ui_owned) && !flat_only) main_stub(&viewport,nullptr,&family);
            auto render_frame=[&]{
                if (mode=="--fault-material-constants") {
                    const float wrong_camera[]{100,0,0,0};
                    gpu_check(integrated_device->SetPixelShaderConstantF(12,wrong_camera,1),
                        "Inject a wrong native material camera into the production fixture");
                }
                if (flat_only) {
                    counter.reset();mock.expected_instances=2;
                } else if (ui_scaleform) {
                    require(ui_stub(renderer_data.data())==0x28,"Native Scaleform command return value changed");
                } else if (mock.ui && !ui_owned) {
                    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
                    require(batched_stub(renderer_data.data(),identity,256,256,0x1357,0x3f800000)==0x12345678abcdef00ull,
                        "Native Canvas hook changed the original return value");
                } else dispatch_stub(renderer_data.data());
                if (mock.flat_frame) {
                    if (!flat_upload && !flat_texture.get()) {
                        gpu_check(integrated_device->CreateTexture(256,256,1,D3DUSAGE_RENDERTARGET,
                            D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,flat_texture.put(),nullptr),"Create native flat-video source");
                        Com<IDirect3DSurface9> image;
                        gpu_check(flat_texture->GetSurfaceLevel(0,image.put()),"Get native video surface");
                        gpu_check(integrated_device->ColorFill(image.get(),nullptr,0xff000000),"Clear native video frame");
                        const RECT red{40,145,104,209},green{150,50,214,114};
                        gpu_check(integrated_device->ColorFill(image.get(),&red,0xffff0000),"Fill native red video marker");
                        gpu_check(integrated_device->ColorFill(image.get(),&green,0xff00ff00),"Fill native green video marker");
                    }
                    Com<IDirect3DSurface9> image,backbuffer;
                    gpu_check(integrated_device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),
                        "Get native movie backbuffer");
                    const auto video_pixel=[](UINT x,UINT y) {
                        return x>=40 && x<104 && y>=145 && y<209?DWORD(0xffff0000):
                            x>=150 && x<214 && y>=50 && y<114?DWORD(0xff00ff00):DWORD(0xff000000);
                    };
                    if (flat_upload) {
                        if (!upload_source.get()) {
                            gpu_check(integrated_device->CreateOffscreenPlainSurface(256,256,D3DFMT_A8R8G8B8,
                                D3DPOOL_SYSTEMMEM,upload_source.put(),nullptr),"Create native CPU video surface");
                            D3DLOCKED_RECT upload{};
                            gpu_check(upload_source->LockRect(&upload,nullptr,0),"Lock native CPU movie");
                            for (UINT y=0;y<256;++y) for (UINT x=0;x<256;++x)
                                reinterpret_cast<DWORD*>(static_cast<char*>(upload.pBits)+y*upload.Pitch)[x]=video_pixel(x,y);
                            gpu_check(upload_source->UnlockRect(),"Unlock native CPU movie");
                        }
                        gpu_check(integrated_device->UpdateSurface(upload_source.get(),nullptr,backbuffer.get(),nullptr),
                            "Upload native full-screen movie without a renderer");
                        const RECT patch{40,145,104,209};const POINT offset{40,145};
                        gpu_check(integrated_device->ColorFill(backbuffer.get(),&patch,0xffeeeeee),
                            "Replace both movie subrectangles before the native partial upload");
                        gpu_check(integrated_device->UpdateSurface(upload_source.get(),&patch,backbuffer.get(),&offset),
                            "Upload a native movie subrectangle into both atlas halves");
                    } else {
                        gpu_check(flat_texture->GetSurfaceLevel(0,image.put()),"Get current native video frame");
                        gpu_check(integrated_device->StretchRect(image.get(),nullptr,backbuffer.get(),nullptr,D3DTEXF_NONE),
                            "Present a flat-only frame or overwrite the earlier world with a flat video");
                    }
                    if (!flat_readback.get())
                        gpu_check(integrated_device->CreateOffscreenPlainSurface(512,256,D3DFMT_A8R8G8B8,
                            D3DPOOL_SYSTEMMEM,flat_readback.put(),nullptr),"Create complete native movie-atlas readback");
                    gpu_check(integrated_device->GetRenderTargetData(backbuffer.get(),flat_readback.get()),
                        "Read both flat-source atlas halves before spatial projection");
                    D3DLOCKED_RECT copied{};
                    gpu_check(flat_readback->LockRect(&copied,nullptr,D3DLOCK_READONLY),"Lock copied movie atlas");
                    bool exact=true;
                    for (UINT y=0;y<256;++y) for (UINT x=0;x<512;++x)
                        exact &= reinterpret_cast<const DWORD*>(static_cast<const char*>(copied.pBits)+y*copied.Pitch)[x]
                            ==video_pixel(x%256,y);
                    gpu_check(flat_readback->UnlockRect(),"Unlock copied movie atlas");
                    require(exact,"Native movie upload failed to preserve the complete source in both atlas halves");
                }
                if (mock.flat_test && f%5==3) {
                    if (!offscreen_atlas.get()) {
                        gpu_check(integrated_device->CreateTexture(512,256,1,D3DUSAGE_RENDERTARGET,
                            D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,offscreen_atlas.put(),nullptr),"Create isolated offscreen stereo target");
                        tag_atlas_texture(*offscreen_atlas.get(),OutlastTargetRole::scene_color,256,256);
                    }
                    Com<IDirect3DSurface9> image,backbuffer,offscreen;
                    gpu_check(integrated_device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),
                        "Get stereo-world target for partial-copy isolation");
                    if (f==3) {
                        const auto update_source=integrated_device->UpdateSurface(nullptr,nullptr,backbuffer.get(),nullptr);
                        const auto update_dest=integrated_device->UpdateSurface(backbuffer.get(),nullptr,nullptr,nullptr);
                        const auto stretch_source=integrated_device->StretchRect(nullptr,nullptr,backbuffer.get(),nullptr,D3DTEXF_NONE);
                        const auto stretch_dest=integrated_device->StretchRect(backbuffer.get(),nullptr,nullptr,nullptr,D3DTEXF_NONE);
                        require(update_source==D3DERR_INVALIDCALL && update_dest==D3DERR_INVALIDCALL &&
                            stretch_source==D3DERR_INVALIDCALL && stretch_dest==D3DERR_INVALIDCALL,
                            "Native copy hooks did not preserve invalid-argument results");
                    }
                    gpu_check(offscreen_atlas->GetSurfaceLevel(0,offscreen.put()),"Get isolated offscreen atlas");
                    const RECT corner{0,0,1,1};const POINT offset{};
                    if (flat_upload) {
                        gpu_check(integrated_device->UpdateSurface(upload_source.get(),nullptr,offscreen.get(),nullptr),
                            "Keep full offscreen mono uploads out of headset presentation selection");
                        gpu_check(integrated_device->UpdateSurface(upload_source.get(),&corner,backbuffer.get(),&offset),
                            "Keep partial mono uploads from flattening the stereo world");
                    } else {
                        gpu_check(flat_texture->GetSurfaceLevel(0,image.put()),"Get offscreen mono-copy source");
                        gpu_check(integrated_device->StretchRect(image.get(),nullptr,offscreen.get(),nullptr,D3DTEXF_NONE),
                            "Keep full offscreen mono copies out of headset presentation selection");
                        gpu_check(integrated_device->StretchRect(image.get(),&corner,backbuffer.get(),&corner,D3DTEXF_NONE),
                            "Keep partial mono copies from flattening the stereo world");
                    }
                }
            };
            auto present_frame=[&]{
                DWORD wrapping=0;
                if (flat_wrap) {
                    gpu_check(integrated_device->GetRenderState(D3DRS_WRAP0,&wrapping),"Save native frame wrapping");
                    gpu_check(integrated_device->SetRenderState(D3DRS_WRAP0,D3DWRAP_U|D3DWRAP_V),
                        "Set inherited native frame wrapping");
                }
                gpu_check(integrated_device->Present(nullptr,nullptr,nullptr,nullptr),
                    "Present through integrated XR submission");
                if (mock.loading_blackout_frame) {
                    if (!flat_readback.get())
                        gpu_check(integrated_device->CreateOffscreenPlainSurface(512,256,D3DFMT_A8R8G8B8,
                            D3DPOOL_SYSTEMMEM,flat_readback.put(),nullptr),"Create loading-blackout atlas readback");
                    Com<IDirect3DSurface9> image;
                    gpu_check(integrated_device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,image.put()),
                        "Get loading-blackout desktop-mirror source");
                    gpu_check(integrated_device->GetRenderTargetData(image.get(),flat_readback.get()),
                        "Read complete loading-blackout atlas after presentation");
                    D3DLOCKED_RECT copied{};
                    gpu_check(flat_readback->LockRect(&copied,nullptr,D3DLOCK_READONLY),
                        "Lock loading-blackout atlas");
                    bool opaque_black=true;
                    for (UINT y=0;y<256;++y) for (UINT x=0;x<512;++x)
                        opaque_black &= reinterpret_cast<const DWORD*>(
                            static_cast<const char*>(copied.pBits)+y*copied.Pitch)[x]==0xff000000;
                    gpu_check(flat_readback->UnlockRect(),"Unlock loading-blackout atlas");
                    require(opaque_black,"Loading visuals leaked into the native desktop-mirror source");
                    ++loading_mirror_checks;
                }
                if (flat_wrap) {
                    DWORD restored=0;
                    gpu_check(integrated_device->GetRenderState(D3DRS_WRAP0,&restored),"Inspect native frame wrapping");
                    require(restored==(D3DWRAP_U|D3DWRAP_V),"Stereo composition changed native wrapping state");
                    gpu_check(integrated_device->SetRenderState(D3DRS_WRAP0,wrapping),"Restore frame fixture wrapping");
                }
            };
            auto run_thread=[](auto&& action) {
                std::exception_ptr error;
                std::thread worker([&]{try {action();}catch (...) {error=std::current_exception();}});
                worker.join();if (error) std::rethrow_exception(error);
            };
            if (thread_overlap) {
                require(ResetEvent(render_entered) && ResetEvent(present_requested) && ResetEvent(present_finished),
                    "Cannot reset overlap barriers");
                std::exception_ptr render_error,present_error;
                std::thread rendering([&]{try {render_frame();}catch (...) {render_error=std::current_exception();}});
                const auto entered=WaitForSingleObject(render_entered,5000);
                std::thread presenting([&] {
                    SetEvent(present_requested);
                    try {present_frame();}catch (...) {present_error=std::current_exception();}
                    SetEvent(present_finished);
                });
                rendering.join();presenting.join();
                require(entered==WAIT_OBJECT_0,"Renderer did not acquire the overlap barrier");
                if (render_error) std::rethrow_exception(render_error);
                if (present_error) std::rethrow_exception(present_error);
            } else if (thread_handoff) {
                if (f%2) render_frame();else run_thread(render_frame);
                run_thread(present_frame);
            } else {render_frame();present_frame();}
            if (mock.floor_startup_test) {
                RuntimeStatistics floor{};
                require(statistics(&floor)!=FALSE,"Cannot read actual DLL startup-floor state");
                const bool ready=mock.frame>=7;
                require(bool(floor.floor_reference_ready)==ready &&
                    floor.floor_reference_acquisitions==(ready?1u:0u) &&
                    std::abs(floor.standing_reference_m-(ready?1.7f:0.f))<.0001f,
                    "Actual DLL latched cached/zero floor height or recalibrated standing height during crouch");
            }
            if (f==11) native_engine_pointer=recreated_engine.data();
            input_tick_stub(nullptr,.015f);
            native_engine_pointer=native_engine.data();
            const auto poses_before=light_pose_calls,ticks_before=light_ticks;
            light_tick_stub(camera_hero.data(),.015f);
            require(light_ticks==ticks_before+1,"Headset light replayed native battery/light animation");
            if (light_pose_calls!=poses_before && mode.empty() && !head_light_checked) {
                Vec3 local{};
                std::memcpy(&local,native_light_components[0].data()+0x210,sizeof(local));
                require(std::abs(local.x-(mock.frame*.001f-.001f)*100)<.001f &&
                    std::abs(local.y-(mock.frame*.01f-.01f)*100)<.001f && std::abs(local.z)<.001f,
                    "Actual production light did not follow the rendered headset midpoint");
                write(camera_hero.data(),0x80,Vec3{100,50,20});
                write(camera_hero.data(),0x90,INT(16384));
                light_tick_stub(camera_hero.data(),.015f);
                Vec3 rebased{};
                std::memcpy(&rebased,native_light_components[0].data()+0x210,sizeof(rebased));
                require(std::abs(local.x-rebased.x)<.001f && std::abs(local.y-rebased.y)<.001f &&
                    std::abs(local.z-rebased.z)<.001f,"Headset light did not rebase with the live hero root/yaw");
                write(camera_hero.data(),0x80,Vec3{});
                write(camera_hero.data(),0x90,INT(0));
                for (const BYTE effect:{BYTE(0),BYTE(1),BYTE(3),BYTE(4),BYTE(255)}) {
                    native_effects[0x3b8]=effect;
                    const auto calls=light_pose_calls;
                    light_tick_stub(camera_hero.data(),.015f);
                    require(light_pose_calls==calls,"Headset light overrode native/scripted effect ownership");
                }
                native_effects[0x3b8]=2;
                const auto calls=light_pose_calls;
                Sleep(270);
                light_tick_stub(camera_hero.data(),.015f);
                require(light_pose_calls==calls,"Headset light consumed a stale render pose");
                head_light_checked=true;
            }
            XINPUT_VIBRATION vibration{32768,65535};
            require(gamepad_vibration_stub(0,&vibration)==ERROR_SUCCESS,
                "Native rumble did not acknowledge virtual controller zero");
            require(gamepad_vibration_stub(1,&vibration)==ERROR_SUCCESS,
                "Native rumble changed an unrelated physical controller");
            const bool active=(f+1)%5!=0 && (input_chords || (f+1)%3!=0);
            if (!active)
                require(light_pose_calls==poses_before,"Headset light continued after focus or frame tracking loss");
            require(bool(observed_joystick_focus)==active,
                "XR-focused joystick polling remained tied to desktop keyboard focus");
            require(joystick_focus_site(focus_viewport.data())==0,
                "Joystick focus override leaked outside the owned native input tick");
            if (!active)
                require(!motion_mock::fake.vibrating[0] && !motion_mock::fake.vibrating[1],
                    "Skipped/unfocused native frame retained XR vibration");
            for (unsigned hand=0;hand<2;++hand) if (motion_mock::fake.vibrating[hand])
                require(active && motion_mock::fake.last_haptic[hand].amplitude==1 &&
                    motion_mock::fake.last_haptic[hand].duration==100000000 &&
                    motion_mock::fake.last_haptic[hand].frequency==XR_FREQUENCY_UNSPECIFIED,
                    "Native motor output changed amplitude, duration or focus before XR submission");
            const auto& pad=observed_gamepad.Gamepad;
            if (input_recovery) {
                const WORD expected=f==1 || f==7 || f==13?
                    XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_Y|
                    XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_START:0;
                require(pad.wButtons==expected && pad.sThumbLX==0 && pad.sThumbLY==0 &&
                    pad.sThumbRX==0 && pad.sThumbRY==0,
                    "Actual DLL replayed a held digital action or lost a fresh press at frame "+
                    std::to_string(f+1)+"; buttons="+std::to_string(pad.wButtons)+
                    "; expected="+std::to_string(expected));
                continue;
            }
            if (input_chords) {
                const unsigned step=f+1;
                const WORD expected=step==9?XINPUT_GAMEPAD_RIGHT_THUMB:step==12?XINPUT_GAMEPAD_START:
                    step>=17 && step<=19?XINPUT_GAMEPAD_BACK:0;
                const SHORT expected_x=step>=17 && step<=19?0:9830;
                const SHORT expected_y=step>=17 && step<=19?0:19660;
                require(pad.wButtons==expected &&
                    pad.sThumbLX==(active?expected_x:0) && pad.sThumbLY==(active?expected_y:0) &&
                    pad.sThumbRX==(active?-13107:0) && pad.sThumbRY==0,
                    "Actual DLL leaked a consumed chord/gesture or lost a fresh action at frame "+
                    std::to_string(step));
                continue;
            }
            const double snap_yaw=snap_mode && mock.frame>=2?-3.14159265358979323846/12:0;
            const auto move_x=SHORT(std::lround((std::cos(snap_yaw)*.3-std::sin(snap_yaw)*.6)*32767));
            const auto move_y=SHORT(std::lround((std::sin(snap_yaw)*.3+std::cos(snap_yaw)*.6)*32767));
            require(active?(std::abs(pad.sThumbLX-move_x)<=1 && std::abs(pad.sThumbLY-move_y)<=1 &&
                pad.sThumbRX==((comfort_mode || snap_mode)?0:-13107) && pad.sThumbRY==0 &&
                (pad.wButtons&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_RIGHT_THUMB))==
                ((f==1 || f==7 || f==13)?XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_RIGHT_THUMB:0) &&
                !(pad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER)):
                pad.wButtons==0 && pad.sThumbLX==0 && pad.sThumbRX==0,
                "Native motion action/focus/skipped-frame or unavailable-body use isolation failed");
            const bool crouch_edge=(pad.wButtons&XINPUT_GAMEPAD_B)!=0;
            require(crouch_edge==(active && (f==7 || f==13)),
                "Interrupted held crouch replayed or a fresh release/press lost its native edge");
            crouch_edges+=crouch_edge;
        }
        require(motion_polls==20,"Native motion polling proof truncated");
        require(input_chords || input_recovery || crouch_edges==2,
            "Fresh manual crouch edges were not exercised through the actual DLL");
        if (input_chords)
            std::cout<<"PASS: actual_dll_input_chords; both_stick_release_orders; pause_release_tail; "
                "held_pad_sector; fresh_actions_recovered; native_polls=20\n";
        if (input_recovery)
            std::cout<<"PASS: actual_dll_digital_recovery; held_primary_secondary_and_menu_suppressed; "
                "fresh_press_after_release; focus_and_render_suspension; native_polls=20\n";
        if (snap_recovery)
            std::cout<<"PASS: actual_dll_snap_action_recovery; inactive_axis_does_not_rearm; "
                "held_deflection_suppressed; independent_move_and_buttons_preserved\n";
        if (reset_retry)
            std::cout<<"PASS: rejected_reset_preserves_native_error; native_device_not_reset; "
                "successful_retry_rebuilds_stereo\n";
        if (flat_wrap)
            std::cout<<"PASS: inherited_texture_wrapping; native_state_preserved; stereo_flat_pixels_correct\n";
        if (mock.floor_startup_test)
            std::cout<<"PASS: floor_startup_frames=20; zero_height_deferred; cached_positive_pose_deferred; "
                <<"tracked_zero_deferred; measured_height_acquired_once; crouched_height_preserved\n";
        std::cout<<"PASS: joystick_focus_checks=20; XR_focused_desktop_unfocused; "
            <<"exact_native_callsite_only; input_tick_scope_only; no_Windows_focus_mutation\n";
        require(motion_mock::fake.hand_haptics[0]>0 && motion_mock::fake.hand_haptics[1]>0,
            "Native rumble never reached both actual attached XR action paths");
        std::cout<<"PASS: native_rumble_to_xr; both_tracked_hands; bounded_100ms; "
            <<"native_motor_amplitude; focus_and_skipped_stop; unrelated_pad_preserved\n";
        std::cout<<"PASS: native_motion_polls=20; XR_actions_to_game_input; cached_presence_seeded; "
            <<"focused_axes_buttons; unavailable_body_use_suppressed; focus_and_skipped_frames_neutral; unrelated_pad_preserved\n";
        RuntimeStatistics counts{};require(statistics(&counts)!=FALSE,"Cannot read integrated mod counters");
        if (loading_blackout)
            require(native_movie_queries==19 && mock.loading_blackout_pixels==786432 && loading_mirror_checks==8,
                "Loading blackout missed native named-movie queries, opaque pixels or automatic recovery");
        // Regular/replay masks, additionally split by menu clipping, each rebuilt after reset.
        const unsigned expected_state_allocations=mock.ui?8u:4u;
        const unsigned native_scenes=mock.flat_test?16u:20u;
        const unsigned tracked_scenes=mock.flat_test?12u:16u;
        require(counts.state_block_allocations==expected_state_allocations && counts.state_block_captures==native_scenes,
            "Caller-state blocks were not reused across draws and recreated after native reset; allocations="+
            std::to_string(counts.state_block_allocations)+"; captures="+std::to_string(counts.state_block_captures));
        std::cout<<"PASS: caller_state_allocations="<<expected_state_allocations<<"; captures="<<native_scenes<<"; reset_releases_cached_state; "
            <<"per_draw_recapture_and_restoration\n";
        require(counts.direct_xr_submissions==(mock.flat_test?8u:16u),
            "Gameplay or flat-only frame used the wrong XR presentation path");
        require(mock.frame==20 && mock.submitted==16 && mock.blank==4 && mock.pixels==2097152 &&
            !mock.failed && counts.frames==20 && counts.submitted==16 &&
            counts.geometry_draws==(loading_blackout?20u:mock.flat_test?24u:20u) &&
            counts.world_panel_draws==(loading_blackout?4u:mock.flat_test?8u:0u) &&
            counts.tracked_views==((mock.ui && !ui_owned)?0u:tracked_scenes) &&
            counts.owned_culls==(mock.ui?0u:tracked_scenes*6) && cull_checks==(mock.ui?0u:tracked_scenes*8) &&
            counts.crosshair_updates==native_scenes && crosshair_updates==native_scenes &&
            counts.sphere_inside_culls==(mock.ui?0u:tracked_scenes*2) &&
            nonfinite_spheres==(mock.ui?0u:tracked_scenes) &&
            counts.consumed_replay_calls>0 && counts.reset_count==1,
            "Integrated DLL proof was truncated or changed native ownership/submission/draw contracts");
        if (thread_handoff || thread_overlap)
            require(counts.thread_handoffs>=20,"Native render/Present ownership did not move between threads");
        if (mock.ui) require(counts.ui_draws==20 && counts.ui_plane_draws==16 && mock.ui_checked>1000000,
            "Menu draws did not use the finite-depth single-pass UI plane");
        const unsigned expected_camera_matches=camera_handoff?12u:16u;
        if (camera_bob) require(counts.camera_pose_matches==expected_camera_matches &&
            counts.camera_stabilized_views==expected_camera_matches,
            "Native translation stabilization count differs; linked="+std::to_string(counts.camera_pose_matches)+
            "; stabilized="+std::to_string(counts.camera_stabilized_views)+
            "; expected="+std::to_string(expected_camera_matches));
        const float center[]{0,0,200,0},radius=1;
        require(sphere_stub(view_data.data()+0x3b0,center,&radius)==0,"Owned native culling scope leaked after dispatch");
        RECT outside_bounds{};
        const RECT expected_bounds=scene_number%2?RECT{}:RECT{96,96,128,128};
        require(translucency_bounds_stub(renderer_data.data(),0,&outside_bounds)==(scene_number%2?0:1) &&
            std::memcmp(&outside_bounds,&expected_bounds,sizeof(outside_bounds))==0,
            "Tracked translucency bounds leaked outside native dispatch");
        int full=1;
        require(sphere_inside_stub(view_data.data()+0x3b0,center,&radius,&full)==0 && full==0,
            "Owned native sphere containment scope leaked after dispatch");
        const auto draws=counts.geometry_draws,replays=counts.consumed_replay_calls;
        flat_readback.reset();upload_source.reset();flat_texture.reset();offscreen_atlas.reset();
        geometry_indices.reset();geometry_vertices.reset();object_records.reset();
        geometry_declaration.reset();replay_declaration.reset();original_vertex.reset();original_pixel.reset();
        integrated_device.reset();factory.reset();
        require(shutdown(nullptr)==0,"Integrated runtime teardown failed");
        require(mock.destroyed==4,"Integrated XR handles were not released in order");
        require(FreeLibrary(dll)!=0,"Cannot unload integrated DLL");dll=nullptr;
        for (HANDLE event:{render_entered,present_requested,present_finished})
            if (event) require(CloseHandle(event)!=0,"Cannot release an overlap barrier");
        if (mode.empty() || mode=="--fault-headset-light")
            require(head_light_checked,"Production headset light correction was never exercised");
        if (head_light_checked)
            std::cout<<"PASS: headset_night_light_midpoint; live_root_yaw_rebased; original_tick_once; "
                <<"scripted_modes_and_stale_pose_preserved; focus_tracking_loss_deferred\n";
        std::cout<<"PASS: native_smoothing_disabled; unrelated_flags_preserved; absent_and_recreated_engine_checked\n";
        std::cout<<"PASS: integrated production DLL; native_frames=20; XR_projections=16; checked_eye_pixels=2097152; "
            <<"geometry_draws="<<draws<<"; all_four_draw_families; real_retail_replay_consumed="<<replays
            <<"; native_owned_culls="<<counts.owned_culls<<"; max_disparity_error_px="<<mock.maximum_disparity_error
            <<"; exact_eye_layers; scoped_ownership; native_reset=1; unused_atlas_sampler_preserved; XR_cleanup_handles=4";
        std::cout<<"; native_nonfinite_spheres="<<nonfinite_spheres;
        std::cout<<"; xr_paced_desktop=2";
        std::cout<<"; direct_xr_atlas="<<counts.direct_xr_submissions;
        std::cout<<"; native_sphere_inside_culls="<<counts.sphere_inside_culls;
        std::cout<<"; crosshair_updates_preserved="<<crosshair_updates<<"; crosshair_alpha=0; other_HUD_bytes_preserved";
        std::cout<<"; translucency_bounds_checks="<<translucency_checks
            <<"; empty_partial_mono_bounds_replaced; native_subviewport_and_foreign_renderers_preserved";
        if (thread_handoff || thread_overlap) std::cout<<"; serialized_thread_handoffs="<<counts.thread_handoffs;
        if (thread_overlap) std::cout<<"; simultaneous_present_requests_blocked=20";
        if (mock.ui) std::cout<<"; ui_classified_draws="<<counts.ui_draws<<"; ui_plane_draws="<<counts.ui_plane_draws
            <<"; ui_ray_checked_pixels="<<mock.ui_checked
            <<"; asymmetric_canted_FOV; yaw_pitch_roll; native_canvas_scissor_and_subviewport";
        if (mock.flat_test) std::cout<<"; flat_stereo_panels="<<(loading_blackout?4:8)<<"; world_flat_world_cycles=4; "
            <<"flat_only_and_late_fullscreen_mono_copy; independent_per_eye_flat_pixel_oracle";
        if (loading_blackout) std::cout<<"; loading_blackout_frames=6; opaque_black_eye_pixels=786432; "
            <<"native_loading_name_only; other_movies_visible; world_recovered_after_reset; "
            <<"native_loading_presents=8; direct_and_panel_paths; skipped_frame_mirror_black";
        if (flat_upload) std::cout<<"; UpdateSurface_full_and_partial_two_half_reference";
        if (material_constants) std::cout<<"; native_material_pixel_camera_matches_independent_eye_camera; reset_recovered";
        if (mock.dynamic_ipd) std::cout<<"; runtime_ipd_changes_mm=64,78,50,72; current_eye_layers_and_disparity";
        if (ui_scaleform) std::cout<<"; native_scaleform_scope";
        if (camera_tilt) std::cout<<"; native_body_pitch_roll=0.35,0.18; absolute_eye_centroids=64; headset_gravity_upright";
        if (camera_bob) {
            std::cout<<"; animated_native_translation_removed; absolute_eye_centroids=64; completed_camera_pose_matches="
                <<expected_camera_matches;
        }
        if (camera_handoff) std::cout<<"; native_camera_handoffs=2; unlinked_views=4; "
            <<"native_origins_preserved; gameplay_stabilization_resumed";
        std::cout<<'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        geometry_indices.reset();geometry_vertices.reset();object_records.reset();
        geometry_declaration.reset();replay_declaration.reset();original_vertex.reset();original_pixel.reset();
        integrated_device.reset();
        factory.reset();
        if (dll && shutdown) shutdown(nullptr);
        native.reset();
        if (dll) FreeLibrary(dll);
        return 1;
    }
}
