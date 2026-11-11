#define OLVR_OPENXR_MOCK_ONLY
#include "test_openxr_session.cpp"
#include "outlast_runtime_api.h"
#include "native_probe_progress.h"
#include "native_probe_input.h"
#include "headlocked_ui.h"
#include "native_hook_set.h"
#include "outlast_native_image.h"
#include "native_collectible_probe.h"
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <optional>

namespace {
std::filesystem::path evidence;
std::ofstream trace;
std::unique_ptr<IsolatedD3D12DrawCounter> driver;
RuntimeStatistics previous{};
constexpr UINT probe_frames=300;
bool continue_probe=false;
bool head_turn_probe=false;
int head_turn_pitch_degrees=0;
bool window_sweep_probe=false;
bool particle_emitter_facing_probe=false;
bool material_body_camera_probe=false;
bool window_position_probe=false;
bool occlusion_probe=false;
using OcclusionEntry=UINT64(WINAPI*)(void*,void*,void*,float,UINT64,INT*);
OcclusionEntry original_occlusion=nullptr;
std::unique_ptr<NativeHookSet> occlusion_hooks;
std::ofstream occlusion_trace;
std::mutex occlusion_mutex;
std::unordered_set<UINT64> occlusion_components;
std::atomic<UINT64> occlusion_pose{0};
template<class T> T occlusion_field(const void* object,std::size_t offset) {
    if (!object) throw std::invalid_argument("Null native occlusion object");
    T result{};std::memcpy(&result,static_cast<const BYTE*>(object)+offset,sizeof(result));return result;
}
UINT64 WINAPI observe_occlusion(void* state,void* bounds,void* view,float elapsed,UINT64 opaque,INT* definite) {
    const auto result=original_occlusion(state,bounds,view,elapsed,opaque,definite);
    const auto pose=occlusion_pose.load(std::memory_order_acquire),world=pose>>32;
    if (world<320 || world>420) return result;
    try {
        const auto primitive=occlusion_field<const void*>(bounds,0);
        const auto component=occlusion_field<UINT64>(primitive,0x48);
        const auto center=occlusion_field<std::array<float,3>>(bounds,8);
        const auto extent=occlusion_field<std::array<float,3>>(bounds,0x14);
        const auto radius=occlusion_field<float>(bounds,0x20);
        const auto origin=occlusion_field<std::array<float,3>>(view,0x3a0);
        if (result>1 || !std::isfinite(elapsed) || !std::isfinite(radius) || radius<0 ||
            !std::all_of(center.begin(),center.end(),[](float value){return std::isfinite(value);}) ||
            !std::all_of(extent.begin(),extent.end(),[](float value){return std::isfinite(value) && value>=0;}) ||
            !std::all_of(origin.begin(),origin.end(),[](float value){return std::isfinite(value);}))
            throw std::runtime_error("Native occlusion observer encountered an invalid recovered ABI/layout");
        LARGE_INTEGER clock{};
        if (!QueryPerformanceCounter(&clock)) throw std::runtime_error("Cannot timestamp native occlusion");
        std::lock_guard<std::mutex> lock(occlusion_mutex);
        if (occlusion_components.insert(component).second) {
            MEMORY_BASIC_INFORMATION region{};
            constexpr DWORD readable=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|
                PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
            if (!component || VirtualQuery(reinterpret_cast<const void*>(component),&region,sizeof(region))!=sizeof(region) ||
                region.State!=MEM_COMMIT || (region.Protect&PAGE_GUARD) || !(region.Protect&readable) ||
                component<reinterpret_cast<UINT64>(region.BaseAddress) ||
                region.RegionSize<0x100 || component-reinterpret_cast<UINT64>(region.BaseAddress)>region.RegionSize-0x100)
                throw std::runtime_error("Native primitive component snapshot is not a bounded readable allocation");
            const auto words=occlusion_field<std::array<UINT,16>>(reinterpret_cast<const void*>(component),0xc0);
            occlusion_trace<<"{\"kind\":\"component\",\"primitive_key\":"<<component
                <<",\"first_world_frame\":"<<world<<",\"world_offset\":192,\"world_words\":[";
            for (unsigned index=0;index<words.size();++index) occlusion_trace<<(index?",":"")<<words[index];
            occlusion_trace<<"]}\n";
        }
        occlusion_trace<<"{\"kind\":\"primitive\",\"world_frame\":"<<world<<",\"xr_frame\":"<<UINT(pose)
            <<",\"clock_ticks\":"<<clock.QuadPart<<",\"view\":"<<reinterpret_cast<UINT64>(view)
            <<",\"view_state\":"<<reinterpret_cast<UINT64>(state)
            <<",\"view_query_frame\":"<<occlusion_field<UINT>(view,0x7c)
            <<",\"primitive_info\":"<<reinterpret_cast<UINT64>(primitive)
            <<",\"primitive_key\":"<<component
            <<",\"primitive_flags\":"<<unsigned(occlusion_field<BYTE>(primitive,0x60))
            <<",\"center\":["<<center[0]<<','<<center[1]<<','<<center[2]
            <<"],\"extent\":["<<extent[0]<<','<<extent[1]<<','<<extent[2]<<"],\"radius\":"<<radius
            <<",\"native_origin\":["<<origin[0]<<','<<origin[1]<<','<<origin[2]
            <<"],\"view_query_flags\":"<<occlusion_field<UINT>(view,0x1138)
            <<",\"occluded\":"<<(result?"true":"false")<<",\"definite\":";
        if (result) occlusion_trace<<"null";
        else {
            if (!definite || (*definite!=0 && *definite!=1))
                throw std::runtime_error("Invalid native occlusion output flag");
            occlusion_trace<<*definite;
        }
        occlusion_trace<<"}\n";
        if (!occlusion_trace) throw std::runtime_error("Cannot retain actual native occlusion evidence");
        return result;
    } catch (const std::exception& error) {
        std::cerr<<"Native occlusion observer: "<<error.what()<<'\n';
        std::ofstream failure(evidence/L"capture-error.txt",std::ios::app);
        failure<<"Native occlusion observer: "<<error.what()<<'\n';failure.close();
        RaiseFailFastException(nullptr,nullptr,0);std::terminate();
    }
}
unsigned menu_pointer_probe=0;
bool pause_menu_probe=false;
bool gait_probe=false;
bool lean_probe=false;
bool body_probe=false;
bool finger_gait_probe=false;
bool floor_probe=false,contacts_probe=false,interaction_probe=false,floor_startup_probe=false;
std::unique_ptr<NativeCollectibleProbe> collectible_probe;
std::ofstream collectible_fixture;
UINT64 collectible_settled_world=0,collectible_press_world=0;
UINT64 collectible_obstacle_world=0;
unsigned collectible_attempts=0;
unsigned collectible_route=0;
std::optional<Vec3> collectible_start;
bool corner_recovery_probe=false;
bool physical_camcorder_probe=false;
bool no_projection_images_probe=false;
bool material_readback_probe=false;
bool asymmetric_fov_probe=false;
UINT64 camcorder_paced_world=0;
std::chrono::steady_clock::time_point camcorder_frame_time{};
INT64 camcorder_frame_clock_ns=0;
bool jump_probe=false;
UINT camera_mount_version=1;
bool contact_draw_probe=false;
bool focus_policy_probe=false;
UINT focus_probe_phase=0;
UINT64 focus_probe_phase_started=0;
bool focus_probe_finished=false;
bool capture_complete=false;
std::atomic<UINT64> startup_pad_state{0};
olvr::NativeProbeProgress continue_progress;
HWND held_enter_window=nullptr;
UINT enter_release_at=0;
struct InputAttempt {
    bool foreground=false,focus=false,active=false;
    std::uintptr_t window_proc_rva=0;
};
class OwnedInputQueue {
    DWORD source_=GetCurrentThreadId(),target_;
public:
    explicit OwnedInputQueue(DWORD target):target_(target) {
        if (source_!=target_ && !AttachThreadInput(source_,target_,TRUE))
            throw std::runtime_error("Cannot attach the probe to its own window input queue");
    }
    ~OwnedInputQueue() {
        if (source_!=target_ && !AttachThreadInput(source_,target_,FALSE)) {
            OutputDebugStringA("FATAL: Cannot detach the owned probe input queue");
            std::terminate();
        }
    }
};

void release_owned_enter(UINT submitted,bool force=false) {
    if (!held_enter_window || (!force && submitted<enter_release_at)) return;
    DWORD process=0;
    GetWindowThreadProcessId(held_enter_window,&process);
    if (process!=GetCurrentProcessId())
        throw std::runtime_error("Held Continue input lost its owned game window");
    const LPARAM key=1|(LPARAM(MapVirtualKeyW(VK_RETURN,MAPVK_VK_TO_VSC))<<16)|
        LPARAM(0xc0000000u)|native_probe_key_marker;
    if (!PostMessageW(held_enter_window,WM_KEYUP,VK_RETURN,key))
        throw std::runtime_error("Continue probe could not release owned menu input");
    held_enter_window=nullptr;
}

HWND owned_window() {
    using Window=HWND(WINAPI*)();
    const auto module=GetModuleHandleW(L"OutlastVRSinglePass.dll");
    const auto address=GetProcAddress(module,"OutlastVR_ProbeWindow");
    Window selected=nullptr;
    static_assert(sizeof(selected)==sizeof(address));
    std::memcpy(&selected,&address,sizeof(selected));
    if (!selected) throw std::runtime_error("Continue probe lacks the native device-window query");
    const auto window=selected();
    DWORD process=0;
    const auto thread=GetWindowThreadProcessId(window,&process);
    if (!thread || process!=GetCurrentProcessId())
        throw std::runtime_error("Continue probe input window is no longer owned");
    return window;
}

void focus_policy_input(UINT world) {
    if (!focus_policy_probe || world<360) return;
    LARGE_INTEGER clock{},frequency{};
    QueryPerformanceCounter(&clock);QueryPerformanceFrequency(&frequency);
    const auto tick=GetTickCount64();
    const auto now=UINT64(clock.QuadPart*1000000/frequency.QuadPart);
    motion_mock::fake.focused=focus_probe_phase!=2;
    if (focus_probe_phase==4) {
        focus_probe_finished=now-focus_probe_phase_started>=1000000;
        return;
    }
    const UINT64 dwell=focus_probe_phase==1?24000000:1500000;
    if (focus_probe_phase && now-focus_probe_phase_started<dwell) return;
    const UINT phase=focus_probe_phase+1;
    motion_mock::fake.focused=phase!=2;
    if (phase==1 || phase==4) {
        const auto window=owned_window();
        if (!PostMessageW(window,WM_ACTIVATEAPP,phase==4,0) ||
            !PostMessageW(window,WM_ACTIVATE,phase==4?WA_ACTIVE:WA_INACTIVE,0))
            throw std::runtime_error("Cannot post activation to the owned native window");
    }
    std::ofstream events(evidence/L"native-focus-events.jsonl",std::ios::app);
    events<<"{\"phase\":"<<phase<<",\"gameplay_frame\":"<<world<<",\"tick_ms\":"<<tick
        <<",\"clock_us\":"<<now
        <<",\"xr_focused\":"<<(motion_mock::fake.focused?"true":"false")
        <<",\"desktop_active\":"<<(phase==4?"true":"false")
        <<",\"posted_activation\":"<<((phase==1 || phase==4)?"true":"false")
        <<",\"activation_message\":"<<WM_ACTIVATE<<"}\n";
    events.close();
    if (!events) throw std::runtime_error("Cannot retain native focus fixture events");
    focus_probe_phase=phase;focus_probe_phase_started=now;
}

InputAttempt press_owned_enter() {
    if (held_enter_window) throw std::runtime_error("Continue probe overlapped held menu inputs");
    const auto window=owned_window();
    const auto thread=GetWindowThreadProcessId(window,nullptr);
    OwnedInputQueue input(thread);
    ShowWindow(window,SW_RESTORE);
    SetActiveWindow(window);
    SetFocus(window);
    SetForegroundWindow(window);
    GUITHREADINFO gui{};gui.cbSize=sizeof(gui);
    if (!GetGUIThreadInfo(thread,&gui))
        throw std::runtime_error("Cannot read the owned window input state");
    InputAttempt result;
    result.foreground=GetForegroundWindow()==window;
    result.focus=gui.hwndFocus==window;
    result.active=gui.hwndActive==window;
    const auto procedure=std::uintptr_t(GetWindowLongPtrW(window,GWLP_WNDPROC));
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE owner=nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(procedure),&owner) && owner==GetModuleHandleW(nullptr))
        result.window_proc_rva=procedure-base;
    const LPARAM key=1|(LPARAM(MapVirtualKeyW(VK_RETURN,MAPVK_VK_TO_VSC))<<16)|native_probe_key_marker;
    if (!PostMessageW(window,WM_KEYDOWN,VK_RETURN,key))
        throw std::runtime_error("Continue probe could not queue its owned menu input");
    held_enter_window=window;
    enter_release_at=mock.submitted+3;
    return result;
}

void gait_input() {
    const auto frame=continue_progress.gameplay_frames();
    const auto action=native_probe_gait_action(frame,lean_probe);
    const auto key=action.key;
    const auto down=action.down;
    if (!key) return;
    const auto window=owned_window();
    const LPARAM bits=1|(LPARAM(MapVirtualKeyW(key,MAPVK_VK_TO_VSC))<<16)|
        (down?0:LPARAM(0xc0000000u))|native_probe_key_marker;
    if (!PostMessageW(window,down?WM_KEYDOWN:WM_KEYUP,key,bits))
        throw std::runtime_error("Gait probe could not queue its owned movement input");
    std::ofstream inputs(evidence/L"gait-input-events.jsonl",std::ios::app);
    inputs<<"{\"gameplay_frame\":"<<frame<<",\"key\":"<<key
        <<",\"down\":"<<(down?"true":"false")<<"}\n";
    inputs.close();
    if (!inputs) throw std::runtime_error("Cannot record owned gait input");
}

void initialize_probe() {
    wchar_t folder[32768]{};
    const auto count=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder,UINT(std::size(folder)));
    if (!count || count>=std::size(folder))
        throw std::runtime_error("Native probe requires an explicit evidence directory");
    evidence=std::wstring(folder,count);
    if (!evidence.is_absolute() || !std::filesystem::is_directory(evidence))
        throw std::runtime_error("Native probe evidence directory must already exist");
    trace.open(evidence/L"frames.jsonl",std::ios::binary|std::ios::trunc);
    if (!trace) throw std::runtime_error("Cannot create native probe trace");
    Com<IDXGIFactory4> inventory;
    gpu_check(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
        reinterpret_cast<void**>(inventory.put())),"Create probe hardware inventory");
    Com<IDXGIAdapter1> adapter;
    gpu_check(inventory->EnumAdapters1(0,adapter.put()),"Select probe hardware");
    DXGI_ADAPTER_DESC1 description{};
    gpu_check(adapter->GetDesc1(&description),"Read probe hardware identity");
    if (description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)
        throw std::runtime_error("Native probe refuses software graphics");
    mock.luid=description.AdapterLuid;
    mock.extent=512;
    mock.native_probe=true;
    motion_mock::fake.neutral_controls=true;
    wchar_t scenario[8]{};
    const auto length=GetEnvironmentVariableW(L"OLVR_PROBE_CONTINUE",scenario,UINT(std::size(scenario)));
    if (length && (length!=1 || scenario[0]!=L'1'))
        throw std::runtime_error("Invalid Continue probe selection");
    continue_probe=length!=0;
    wchar_t pointer[16]{};
    const auto pointer_length=GetEnvironmentVariableW(L"OLVR_PROBE_MENU_POINTER",pointer,16);
    if (pointer_length) {
        const std::wstring selected(pointer,pointer_length<16?pointer_length:0);
        pause_menu_probe=selected==L"PauseInspect" || selected==L"PauseResume" ||
            selected==L"PauseOptions" || selected==L"PauseMiss" ||
            selected==L"PauseUpright" || selected==L"PauseFocus";
        if ((continue_probe!=pause_menu_probe) ||
            (!pause_menu_probe && selected!=L"Continue" && selected!=L"Options" && selected!=L"Miss"))
            throw std::runtime_error("Native menu pointer fixture requires its explicit main-menu or gameplay target");
        menu_pointer_probe=selected==L"PauseInspect"?4:selected==L"PauseResume"?5:
            selected==L"PauseOptions"?6:selected==L"PauseMiss"?7:
            selected==L"PauseUpright"?8:selected==L"PauseFocus"?9:
            selected==L"Continue"?1:selected==L"Options"?2:3;
    }
    wchar_t head_turn[8]{};
    const auto head_length=GetEnvironmentVariableW(L"OLVR_PROBE_HEAD_TURN",head_turn,UINT(std::size(head_turn)));
    if (head_length && (head_length!=1 || head_turn[0]!=L'1' || !continue_probe))
        throw std::runtime_error("Head-turn probe requires Continue and the explicit value 1");
    head_turn_probe=head_length!=0;
    if (head_turn_probe) mock.extent=1024;
    wchar_t pitch[8]{};
    const auto pitch_length=GetEnvironmentVariableW(L"OLVR_PROBE_HEAD_TURN_PITCH",pitch,UINT(std::size(pitch)));
    if (pitch_length) {
        if (!head_turn_probe || pitch_length!=3 || std::wstring(pitch,pitch_length)!=L"-15")
            throw std::runtime_error("Head-turn pitch fixture requires HeadTurn and the explicit value -15");
        head_turn_pitch_degrees=-15;
    }
    wchar_t sweep[8]{};
    const auto sweep_length=GetEnvironmentVariableW(L"OLVR_PROBE_WINDOW_SWEEP",sweep,UINT(std::size(sweep)));
    if (sweep_length && (sweep_length!=1 || sweep[0]!=L'1' || !head_turn_probe ||
        head_turn_pitch_degrees!=-15))
        throw std::runtime_error("Window sweep requires HeadTurn, pitch -15 and the explicit value 1");
    window_sweep_probe=sweep_length!=0;
    wchar_t emitter[2]{},particle_mode[2]{};
    const auto emitter_length=GetEnvironmentVariableW(L"OLVR_PROBE_PARTICLE_EMITTER_FACING",emitter,2);
    const auto particle_length=GetEnvironmentVariableW(L"OLVR_PROBE_PARTICLE_WINDOWS",particle_mode,2);
    if (emitter_length && (emitter_length!=1 || emitter[0]!=L'1' || !window_sweep_probe ||
        particle_length!=1 || particle_mode[0]!=L'1'))
        throw std::runtime_error("Emitter-facing sweep requires the isolated native particle fixture");
    particle_emitter_facing_probe=emitter_length!=0;
    wchar_t position[2]{};
    const auto position_length=GetEnvironmentVariableW(L"OLVR_PROBE_WINDOW_POSITION",position,2);
    if (position_length && (position_length!=1 || position[0]!=L'1' || !window_sweep_probe))
        throw std::runtime_error("Roomscale window fixture requires the explicit native window sweep");
    window_position_probe=position_length!=0;
    wchar_t occlusion[2]{};
    const auto occlusion_length=GetEnvironmentVariableW(L"OLVR_PROBE_OCCLUSION",occlusion,2);
    if (occlusion_length && (occlusion_length!=1 || occlusion[0]!=L'1' || !window_position_probe))
        throw std::runtime_error("Native occlusion observation requires the explicit roomscale window fixture");
    occlusion_probe=occlusion_length!=0;
    wchar_t gait[8]{};
    const auto gait_length=GetEnvironmentVariableW(L"OLVR_PROBE_GAIT",gait,UINT(std::size(gait)));
    if (gait_length && (gait_length!=1 || gait[0]!=L'1' || !continue_probe || head_turn_probe))
        throw std::runtime_error("Gait probe requires Continue without a head-turn test and the explicit value 1");
    gait_probe=gait_length!=0;
    wchar_t lean[8]{};
    const auto lean_length=GetEnvironmentVariableW(L"OLVR_PROBE_LEAN",lean,UINT(std::size(lean)));
    if (lean_length && (lean_length!=1 || lean[0]!=L'1' || !gait_probe))
        throw std::runtime_error("Lean probe requires explicit gait and the value 1");
    lean_probe=lean_length!=0;
    wchar_t combined[2]{};
    const auto combined_length=GetEnvironmentVariableW(L"OLVR_PROBE_MATERIAL_BODY_CAMERA",combined,2);
    if (combined_length && (combined_length!=1 || combined[0]!=L'1'))
        throw std::runtime_error("Invalid explicit combined body/camera material fixture");
    wchar_t body[8]{};
    const auto body_length=GetEnvironmentVariableW(L"OLVR_PROBE_BODY",body,UINT(std::size(body)));
    if (body_length && (body_length!=1 || body[0]!=L'1' ||
        !native_probe_body_fixture(continue_probe,gait_probe,head_turn_probe,combined_length!=0)))
        throw std::runtime_error("Body probe requires Continue without gait and an explicitly combined head-turn fixture");
    body_probe=body_length!=0;
    if (pause_menu_probe && !body_probe)
        throw std::runtime_error("Native pause-menu fixture requires actual gameplay and tracked Body input");
    wchar_t fingers[2]{};
    const auto fingers_length=GetEnvironmentVariableW(L"OLVR_PROBE_FINGER_GAIT",fingers,2);
    if (fingers_length && (fingers_length!=1 || fingers[0]!=L'1' || !body_probe))
        throw std::runtime_error("Native finger gait requires the explicit body fixture");
    finger_gait_probe=fingers_length!=0;
    wchar_t focus_policy[16]{};
    const auto focus_length=GetEnvironmentVariableW(L"OLVR_PROBE_FOCUS_POLICY",focus_policy,16);
    if (focus_length) {
        if (!body_probe || focus_length>=16 ||
            (std::wstring(focus_policy,focus_length)!=L"Tracked" &&
             std::wstring(focus_policy,focus_length)!=L"Control"))
            throw std::runtime_error("Timed native focus fixture requires Body and Tracked or Control policy");
        focus_policy_probe=true;
    }
    wchar_t draw_selection[16]{};
    const auto draw_length=GetEnvironmentVariableW(L"OLVR_PROBE_DRAW_FRAME",draw_selection,UINT(std::size(draw_selection)));
    contact_draw_probe=draw_length==7 && std::wstring(draw_selection,draw_length)==L"contact";
    for (const auto setting: {std::pair<const wchar_t*,bool*>{L"OLVR_PROBE_FLOOR",&floor_probe},
                             {L"OLVR_PROBE_CONTACTS",&contacts_probe},{L"OLVR_PROBE_INTERACTION",&interaction_probe},
                             {L"OLVR_PROBE_FLOOR_STARTUP",&floor_startup_probe},
                             {L"OLVR_PROBE_JUMP",&jump_probe},
                             {L"OLVR_PROBE_CORNER_RECOVERY",&corner_recovery_probe},
                             {L"OLVR_PROBE_PHYSICAL_CAMCORDER",&physical_camcorder_probe}}) {
        wchar_t value[8]{};
        const auto length=GetEnvironmentVariableW(setting.first,value,UINT(std::size(value)));
        if (length && (length!=1 || value[0]!=L'1' || !body_probe))
            throw std::runtime_error("Floor/contact/interaction probes require explicit Body mode and value1");
        *setting.second=length!=0;
    }
    if (jump_probe && (!floor_probe || physical_camcorder_probe || contacts_probe ||
                       interaction_probe || floor_startup_probe))
        throw std::runtime_error("Jump probe requires measured-floor Body mode without other action fixtures");
    if (corner_recovery_probe && (!contacts_probe || !floor_probe || physical_camcorder_probe ||
                                 interaction_probe || focus_policy_probe || jump_probe))
        throw std::runtime_error("Corner recovery requires measured-floor Contacts without other action fixtures");
    wchar_t collectible[2]{};
    const auto collectible_length=GetEnvironmentVariableW(L"OLVR_PROBE_COLLECTIBLE",collectible,2);
    if (collectible_length) {
        if (collectible_length!=1 || collectible[0]!=L'1' || !body_probe || !floor_probe ||
            contacts_probe || interaction_probe || physical_camcorder_probe || jump_probe ||
            finger_gait_probe || pause_menu_probe || focus_policy_probe)
            throw std::runtime_error("Collectible fixture requires isolated measured-floor Body gameplay");
        collectible_probe=std::make_unique<NativeCollectibleProbe>(evidence);
        collectible_fixture.open(evidence/L"native-collectible-fixture.jsonl",std::ios::binary);
        collectible_fixture.precision(9);
        if (!collectible_fixture) throw std::runtime_error("Cannot create native collectible input evidence");
    }
    wchar_t mount[8]{};
    const auto mount_length=GetEnvironmentVariableW(L"OLVR_PROBE_CAMERA_MOUNT_VERSION",mount,UINT(std::size(mount)));
    if (mount_length) {
        if (!physical_camcorder_probe || mount_length!=1 || mount[0]!=L'5')
            throw std::runtime_error("Current camera fixture requires explicit physical-camera mode and mount version 5");
        camera_mount_version=UINT(mount[0]-L'0');
    }
    mock.stage_supported=floor_probe;
    if (floor_startup_probe && !floor_probe)
        throw std::runtime_error("Floor-startup probe requires explicit measured-floor mode");
    wchar_t recommended_size[8]{};
    const auto size_length=GetEnvironmentVariableW(L"OLVR_PROBE_RECOMMENDED_SIZE",recommended_size,UINT(std::size(recommended_size)));
    if (size_length && (size_length!=1 || recommended_size[0]!=L'1'))
        throw std::runtime_error("Invalid recommended-extent native probe policy");
    if (size_length) mock.extent=1024;
    if (combined_length && (combined_length!=1 || combined[0]!=L'1' || !body_probe || !floor_probe ||
        !physical_camcorder_probe || !head_turn_probe || !window_sweep_probe || !window_position_probe ||
        contacts_probe || interaction_probe || jump_probe || floor_startup_probe || collectible_probe))
        throw std::runtime_error("Combined material fixture requires isolated measured-floor Body and physical camera");
    material_body_camera_probe=combined_length!=0;
    wchar_t no_images[2]{};
    const auto no_images_length=GetEnvironmentVariableW(
        L"OLVR_PROBE_NO_PROJECTION_IMAGES",no_images,2);
    if (no_images_length) {
        const auto flag=[](const wchar_t* name) {
            wchar_t value[2]{};
            const auto length=GetEnvironmentVariableW(name,value,2);
            if (length && (length!=1 || value[0]!=L'1'))
                throw std::runtime_error("Invalid material readback fixture flag");
            return length!=0;
        };
        const bool glass=flag(L"OLVR_PROBE_GLASS_REFERENCE");
        const bool temporal=flag(L"OLVR_PROBE_TEMPORAL_WINDOWS");
        const bool reflective=flag(L"OLVR_PROBE_REFLECTIVE_GLASS");
        const bool contribution=flag(L"OLVR_PROBE_GLASS_CONTRIBUTION");
        const bool hdr=flag(L"OLVR_PROBE_DRAW_HDR");
        material_readback_probe=glass && temporal && (reflective || contribution || particle_length) &&
            window_sweep_probe && window_position_probe && !hdr;
        if (no_images_length!=1 || no_images[0]!=L'1' ||
            !native_probe_readback_free_fixture(size_length!=0,physical_camcorder_probe,
                occlusion_probe,material_readback_probe,draw_length!=0,material_body_camera_probe))
            throw std::runtime_error("Readback-free projections require one complete recommended camera, occlusion or original-material fixture");
    }
    no_projection_images_probe=no_images_length!=0;
    if (material_body_camera_probe && !material_readback_probe)
        throw std::runtime_error("Combined body/camera fixture requires complete bounded original-material readbacks");
    wchar_t asymmetric_fov[2]{};
    const auto asymmetric_length=GetEnvironmentVariableW(L"OLVR_PROBE_ASYMMETRIC_FOV",asymmetric_fov,2);
    if (asymmetric_length && (asymmetric_length!=1 || asymmetric_fov[0]!=L'1' || !material_readback_probe))
        throw std::runtime_error("Asymmetric FOV requires the complete readback-bounded original-material fixture");
    asymmetric_fov_probe=asymmetric_length!=0;
    wchar_t eye_size[16]{};
    const auto eye_size_length=GetEnvironmentVariableW(L"OLVR_PROBE_EYE_SIZE",eye_size,UINT(std::size(eye_size)));
    if (eye_size_length) {
        const std::wstring size(eye_size,eye_size_length<std::size(eye_size)?eye_size_length:0);
        if (!size_length || (size!=L"1024" && size!=L"2448" && size!=L"2688x2880"))
            throw std::runtime_error("Unsupported explicit native diagnostic eye size");
        mock.extent=size==L"2688x2880"?2688:size==L"2448"?2448:1024;
        mock.extent_height=size==L"2688x2880"?2880:0;
    }
    if (material_readback_probe && (mock.extent!=2688 || mock.extent_height!=2880))
        throw std::runtime_error("Bounded material readback requires the declared rectangular headset extent");
    if (occlusion_probe) {
        std::array<wchar_t,32768> path{};
        const auto length=GetModuleFileNameW(nullptr,path.data(),UINT(path.size()));
        if (!length || length>=path.size()) throw std::runtime_error("Cannot identify native occlusion image");
        const OutlastNativeImage image(std::wstring(path.data(),length));
        const auto body=image.body(0x8bb6f0,16);
        const auto address=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x8bb6f0;
        if (std::memcmp(reinterpret_cast<const void*>(address),body.data(),body.size())!=0)
            throw std::runtime_error("Native occlusion entry is already modified; observer refused");
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0)
            throw std::runtime_error("Cannot identify native occlusion clock");
        occlusion_trace.open(evidence/L"native-occlusion.jsonl",std::ios::binary);
        occlusion_trace.precision(9);
        occlusion_trace<<"{\"kind\":\"observer\",\"fixture_version\":2,\"clock_frequency\":"
            <<frequency.QuadPart<<",\"retail_sha256\":\""<<outlast_retail_sha256
            <<"\",\"entry_rva\":9156336,\"world_frame_first\":320,\"world_frame_last\":420,"
              "\"scope\":\"all_native_occlusion_calls_in_roomscale_pose_interval\","
              "\"argument5\":\"opaque_forwarded_stack_slot\","
              "\"native_result_and_output_preserved\":true}\n";
        occlusion_trace.flush();
        if (!occlusion_trace) throw std::runtime_error("Cannot create native occlusion evidence");
        occlusion_hooks=std::make_unique<NativeHookSet>();
        occlusion_hooks->add(reinterpret_cast<OcclusionEntry>(address),&observe_occlusion,original_occlusion);
        occlusion_hooks->enable_pending();
    }
}

XrResult XRAPI_CALL probe_pose_state(XrSession session,const XrActionStateGetInfo* info,XrActionStatePose* output) {
    const auto result=motion_mock::pose_state(session,info,output);
    if (result==XR_SUCCESS && continue_probe && !continue_progress.gameplay_frames() &&
        motion_mock::fake.actions.at(info->action).first=="aim_pose")
        output->isActive=XR_FALSE;
    return result;
}
XrResult XRAPI_CALL probe_configuration(XrInstance instance,XrSystemId system,XrViewConfigurationType type,
    uint32_t capacity,uint32_t* count,XrViewConfigurationView* output) {
    const auto result=configuration(instance,system,type,capacity,count,output);
    if (result==XR_SUCCESS && capacity)
        for (uint32_t eye=0;eye<std::min(capacity,*count);++eye) {
            output[eye].maxImageRectWidth=std::max(1024u,mock.extent);
            output[eye].maxImageRectHeight=std::max(1024u,mock.height());
        }
    return result;
}

XrResult XRAPI_CALL probe_locate(XrSession session,const XrViewLocateInfo* info,XrViewState* state,
    uint32_t capacity,uint32_t* count,XrView* output) {
    const auto result=locate(session,info,state,capacity,count,output);
    if (result==XR_SUCCESS && asymmetric_fov_probe && output && capacity>=2 && *count==2) {
        for (unsigned eye=0;eye<2;++eye) {
            const auto fov=native_probe_asymmetric_fov(eye);
            output[eye].fov={fov[0],fov[1],fov[2],fov[3]};
            mock.views[eye].fov=output[eye].fov;
        }
    }
    const auto world=continue_progress.gameplay_frames();
    if (occlusion_probe) occlusion_pose.store((UINT64(world)<<32)|mock.frame,std::memory_order_release);
    if (result==XR_SUCCESS && pause_menu_probe) {
        motion_mock::fake.fixture_head.y=floor_probe?1.7f:0.f;
        motion_mock::fake.focused=menu_pointer_probe!=9 || world<170 || world>=180;
        for (unsigned eye=0;eye<2;++eye) {
            if (menu_pointer_probe==8 && world>=112 && world<145) {
                const auto orientation=oracle_multiply(
                    {std::sin(.725f),0,0,std::cos(.725f)},
                    {0,0,std::sin(.2f),std::cos(.2f)});
                const auto offset=oracle_rotate(orientation,{eye?.032:-.032,0,0});
                output[eye].pose={{orientation.x,orientation.y,orientation.z,orientation.w},
                    {float(offset.x),float(offset.y),float(offset.z)}};
            }
            output[eye].pose.position.y+=motion_mock::fake.fixture_head.y;
            mock.views[eye]=output[eye];
        }
    }
    if (result==XR_SUCCESS && menu_pointer_probe) {
        std::array<EyePose,2> eyes{};
        std::array<EyeFov,2> fovs{};
        for (unsigned eye=0;eye<2;++eye) {
            const auto& pose=output[eye].pose;
            eyes[eye]={{pose.position.x,pose.position.y,pose.position.z},
                {pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w}};
            const auto& fov=output[eye].fov;
            fovs[eye]={fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown};
        }
        const float aspect=previous.source_height?float(previous.source_width)/previous.source_height:16.f/9;
        auto placement=eyes;
        if (pause_menu_probe) {
            const auto center=scale(add(eyes[0].position,eyes[1].position),.5f);
            const auto forward=rotate(eyes[0].orientation,{0,0,-1});
            const float heading_angle=std::atan2(-forward.x,-forward.z);
            const Quaternion heading{0,std::sin(heading_angle/2),0,std::cos(heading_angle/2)};
            for (unsigned eye=0;eye<2;++eye)
                placement[eye]={add(center,rotate(heading,{eye?.032f:-.032f,0,0})),heading};
        }
        const auto panel=headlocked_ui_constants(placement,fovs,aspect);
        const TrackingFrame tracking{{0,0,0},{1,0,0},{0,1,0},{0,0,1},1,{0,0,0}};
        const auto view=eye_constants(tracking,placement[0],fovs[0],.01f,1000);
        Matrix4 projection{},projected{};
        std::copy_n(view.begin(),16,projection.begin());
        std::copy_n(panel[0].begin(),16,projected.begin());
        auto plane=matrix_product(matrix_inverse(projection),projected);
        static std::optional<Matrix4> pause_aim_plane;
        if (pause_menu_probe && world>=123) {
            if (!pause_aim_plane) pause_aim_plane=plane;
            plane=*pause_aim_plane;
        }
        const float u=(menu_pointer_probe==3 || menu_pointer_probe==7 ||
            (menu_pointer_probe==9 && world>=160 && world<180))?1.3f:.5f;
        const float v=menu_pointer_probe==2?.79f:pause_menu_probe?(menu_pointer_probe==6?.5f:5.f/12):.55f;
        const Vec3 origin{.3f,motion_mock::fake.fixture_head.y-.45f,-.4f};
        const Vec3 target{plane[0]*(2*u-1)+plane[1]*(1-2*v)+plane[3],
            plane[4]*(2*u-1)+plane[5]*(1-2*v)+plane[7],
            plane[8]*(2*u-1)+plane[9]*(1-2*v)+plane[11]};
        const auto delta=add(target,scale(origin,-1));
        const auto direction=scale(delta,1/std::sqrt(dot(delta,delta)));
        const float yaw=std::atan2(-direction.x,-direction.z),pitch=std::asin(direction.y);
        motion_mock::fake.fixture_independent_aim=true;
        motion_mock::fake.fixture_aim_rotations[1]=oracle_multiply(
            {0,std::sin(yaw/2),0,std::cos(yaw/2)},{std::sin(pitch/2),0,0,std::cos(pitch/2)});
        motion_mock::fake.lost_left=true;
        motion_mock::fake.fixture_use=pause_menu_probe?
            ((menu_pointer_probe!=4 && ((world>=118 && world<140) || (world>=200 && world<206) ||
                (menu_pointer_probe==9 && world>=168 && world<190)))?1.f:0.f):
            (mock.submitted>=130 && mock.submitted<136?1.f:0.f);
    }
    motion_mock::fake.fixture_jump=body_probe && continue_probe && !world && held_enter_window!=nullptr;
    if (result==XR_SUCCESS && physical_camcorder_probe && world && world!=camcorder_paced_world) {
        if (camcorder_paced_world)
            std::this_thread::sleep_until(camcorder_frame_time+std::chrono::nanoseconds(11111111));
        camcorder_frame_time=std::chrono::steady_clock::now();
        camcorder_frame_clock_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
            camcorder_frame_time.time_since_epoch()).count();
        camcorder_paced_world=world;
    }
    if (result==XR_SUCCESS && pause_menu_probe && world) {
        motion_mock::fake.fixture_menu=false;
        motion_mock::fake.fixture_move={};
        motion_mock::fake.fixture_jump=false;
        motion_mock::fake.fixture_crouch=false;
        motion_mock::fake.fixture_sprint=false;
    }
    if (result==XR_SUCCESS && body_probe && !pause_menu_probe) {
        if (material_body_camera_probe) motion_mock::fake.fixture_head={0,1.7f,0};
        float height=0;
        auto squat=[&](UINT start,UINT hold_end) {
            if (world>=start && world<start+60) height=-.6f*float(world-start)/60;
            else if (world>=start+60 && world<hold_end) height=-.6f;
            else if (world>=hold_end && world<hold_end+60) height=-.6f*float(hold_end+60-world)/60;
        };
        if (!physical_camcorder_probe && !jump_probe) {
            squat(150,330);
            if (!corner_recovery_probe) {squat(450,570);squat(750,840);}
        }
        for (unsigned eye=0;eye<2;++eye) {
            output[eye].pose.position.y+=height;
            if (world>=100 && !physical_camcorder_probe && !jump_probe) {
                const float pitch=-35.f*.017453292519943295f;
                output[eye].pose.orientation={std::sin(pitch/2),0,0,std::cos(pitch/2)};
                const auto offset=oracle_rotate(output[eye].pose.orientation,{eye?.032:-.032,0,0});
                output[eye].pose.position={float(offset.x),float(offset.y)+height,float(offset.z)};
            }
            mock.views[eye]=output[eye];
            const float angle=world>=100?.3f*std::sin(float(world)*.02f+(eye?1.f:0.f)):0;
            motion_mock::fake.fixture_offsets[eye]=world>=100?XrVector3f{
                .1f*std::sin(float(world)*.02f+(eye?1.f:0.f)),.08f*std::sin(float(world)*.015f),
                .08f*std::cos(float(world)*.02f)}:XrVector3f{};
            motion_mock::fake.fixture_rotations[eye]={0,std::sin(angle/2),0,std::cos(angle/2)};
            if (floor_probe) output[eye].pose.position.y+=1.7f;
            mock.views[eye]=output[eye];
        }
        if (floor_startup_probe && world<7) {
            const float startup_height=world>=3 && world<=4?1.7f:0.f;
            for (unsigned eye=0;eye<2;++eye) {
                output[eye].pose.position.y=startup_height;
                mock.views[eye]=output[eye];
            }
            if (world<=4)
                state->viewStateFlags&=~(XR_VIEW_STATE_POSITION_TRACKED_BIT|XR_VIEW_STATE_ORIENTATION_TRACKED_BIT);
        }
        motion_mock::fake.fixture_head.y=floor_probe?1.7f:0.f;
        motion_mock::fake.lost_left=world>=650 && world<680;
        motion_mock::fake.focused=!(world>=700 && world<730);
        if (focus_policy_probe) {
            motion_mock::fake.focused=true;
            focus_policy_input(world);
        }
        motion_mock::fake.fixture_move=((world>=120 && world<150) || (world>=480 && world<510))?
            XrVector2f{0,.7f}:XrVector2f{};
        if (finger_gait_probe) {
            const bool curled=world>=100 && world<600;
            motion_mock::fake.fixture_grab={curled?1.f:0.f,curled?1.f:0.f};
            motion_mock::fake.fixture_use=curled?1.f:0.f;
        }
        if (contacts_probe && world>=360 && world<640)
            motion_mock::fake.fixture_move={.7f,0};
        if (corner_recovery_probe) {
            motion_mock::fake.focused=true;motion_mock::fake.lost_left=false;
            if (world>=640 && world<700) motion_mock::fake.fixture_move={0,-.7f};
            else if (world>=700 && world<800) motion_mock::fake.fixture_move={.7f,0};
            else if (world>=800 && world<870) motion_mock::fake.fixture_move={0,.7f};
        }
        if (interaction_probe) {
            motion_mock::fake.fixture_menu=world==60 || world==100;
            motion_mock::fake.fixture_notes=world==160 || world==200;
            motion_mock::fake.fixture_use=(world==260 || (world>=300 && world<320) ||
                world==340 || (world>=380 && world<420))?1.f:0.f;
        }
        if (jump_probe) {
            const bool forward=(world>=60 && world<90) || (world>=340 && world<370) ||
                (world>=780 && world<810);
            const bool backward=(world>=110 && world<140) || (world>=400 && world<440) ||
                (world>=660 && world<690);
            motion_mock::fake.fixture_move={0,forward?.9f:backward?-.9f:0};
            motion_mock::fake.fixture_sprint=forward;
            motion_mock::fake.fixture_jump=native_probe_jump_pressed(
                world,motion_mock::fake.fixture_jump);
        }
        if (physical_camcorder_probe) {
            motion_mock::fake.fixture_move={};
            motion_mock::fake.fixture_grab={0,0};
            motion_mock::fake.fixture_nv=(world>=180 && world<185) || (world>=500 && world<505) ||
                (world>=840 && world<845);
            motion_mock::fake.fixture_reload=world==260;
            const bool right=(world>=80 && world<350) || (world>=760 && world<870);
            const bool left=world>=400 && world<690;
            if (world>=60 && world<70) motion_mock::fake.fixture_grab[1]=1;
            if (right) motion_mock::fake.fixture_grab[1]=1;
            if (left) motion_mock::fake.fixture_grab[0]=1;
            const auto regrip=material_body_camera_probe?native_probe_material_regrip(world):NativeProbeCameraRegrip{};
            if (regrip.hand>=0) motion_mock::fake.fixture_grab[regrip.hand]=regrip.squeeze;
            for (unsigned hand=0;hand<2;++hand) {
                XrVector3f position{hand? .3f:-.3f,-.45f,-.4f};
                if ((regrip.approach && regrip.hand==int(hand)) ||
                    (hand && world>=70 && world<100) || (!hand && world>=390 && world<420) ||
                    (hand && world>=740 && world<780)) position={.11f,-.32f,-.16f};
                else if ((hand && right) || (!hand && left))
                    position={hand?.2f:-.1f,-.1f,-.4f};
                motion_mock::fake.fixture_offsets[hand]={
                    position.x-(hand?.3f:-.3f),position.y+.45f,position.z+.4f};
                const float angle=native_probe_camera_yaw(material_body_camera_probe,world,
                    (hand && right) || (!hand && left));
                const auto grip=native_probe_camera_grip(material_body_camera_probe,angle);
                motion_mock::fake.fixture_rotations[hand]={grip[0],grip[1],grip[2],grip[3]};
                motion_mock::fake.fixture_independent_aim=true;
                motion_mock::fake.fixture_aim_rotations[hand]=
                    {0,std::sin(angle/2),0,std::cos(angle/2)};
            }
        }
        if (collectible_probe) {
            height=0;
            motion_mock::fake.fixture_offsets={};
            motion_mock::fake.fixture_rotations={{{0,0,0,1},{0,0,0,1}}};
            motion_mock::fake.fixture_independent_aim=true;
            motion_mock::fake.fixture_aim_rotations=motion_mock::fake.fixture_rotations;
            motion_mock::fake.fixture_head={0,1.7f,0};
            motion_mock::fake.focused=true;motion_mock::fake.lost_left=false;
            motion_mock::fake.fixture_move={};
            motion_mock::fake.fixture_sprint=false;
            motion_mock::fake.fixture_use=0;
            motion_mock::fake.fixture_menu=false;motion_mock::fake.fixture_notes=false;
            for (unsigned eye=0;eye<2;++eye) {
                output[eye].pose={{0,0,0,1},{eye?.032f:-.032f,1.7f,0}};
                mock.views[eye]=output[eye];
            }
            const auto observed=collectible_probe->sample(world);
            float distance=0;
            if (world>=100 && observed.hero && observed.item) {
                if (!observed.steam_disabled)
                    throw std::runtime_error("Native collectible test refused online Steam state");
                const auto delta=add(observed.target,scale(observed.root,-1));
                distance=std::sqrt(delta.x*delta.x+delta.y*delta.y);
                auto forward=observed.forward;forward.z=0;
                const auto length=std::sqrt(dot(forward,forward));
                if (!std::isfinite(distance) || !std::isfinite(length) || length<.1f)
                    throw std::runtime_error("Invalid native collectible fixture basis");
                forward=scale(forward,1/length);
                const Vec3 right{-forward.y,forward.x,0};
                if (!collectible_start) collectible_start=observed.root;
                auto travel_to_waypoint=[&] {
                    Vec3 waypoint=observed.target;
                    if (collectible_route==0) waypoint=add(*collectible_start,
                        scale(right,dot(add(observed.target,scale(*collectible_start,-1)),right)-550));
                    else if (collectible_route<=2) {
                        waypoint=add(waypoint,scale(forward,200));
                        if (collectible_route==1) waypoint=add(waypoint,scale(right,-550));
                    }
                    auto travel=add(waypoint,scale(observed.root,-1));travel.z=0;return travel;
                };
                auto travel=travel_to_waypoint();
                float travel_length=std::sqrt(dot(travel,travel));
                if (collectible_route<3 && travel_length<50) {
                    ++collectible_route;travel=travel_to_waypoint();
                    travel_length=std::sqrt(dot(travel,travel));
                }
                const auto origin=add(observed.view,add(scale(forward,40),add(scale(right,30),Vec3{0,0,-45})));
                const auto aim_delta=add(observed.target,scale(origin,-1));
                const auto aim_length=std::sqrt(dot(aim_delta,aim_delta));
                if (!std::isfinite(aim_length) || aim_length<.01f)
                    throw std::runtime_error("Invalid native collectible fixture aim");
                const Vec3 direction{dot(aim_delta,right)/aim_length,aim_delta.z/aim_length,
                    -dot(aim_delta,forward)/aim_length};
                const float yaw=std::atan2(-direction.x,-direction.z),pitch=std::asin(std::clamp(direction.y,-1.f,1.f));
                motion_mock::fake.fixture_aim_rotations[1]=oracle_multiply(
                    {0,std::sin(yaw/2),0,std::cos(yaw/2)},{std::sin(pitch/2),0,0,std::cos(pitch/2)});
                if (!(observed.flags&1) && !collectible_probe->collected() &&
                    ((observed.basic<=1 && observed.special==0) || (observed.basic==2 && observed.special==19))) {
                    if (observed.basic!=0 || observed.special!=0) {
                        motion_mock::fake.fixture_move={0,1};
                        if (observed.basic==2 && observed.special==19) {
                            if (!collectible_obstacle_world) collectible_obstacle_world=world;
                            const auto phase=world-collectible_obstacle_world;
                            motion_mock::fake.fixture_jump=phase>=10 && phase<16;
                        }
                        collectible_settled_world=0;
                    } else if (distance>88 || collectible_route<3) {
                        motion_mock::fake.fixture_move={.95f*dot(travel,right)/travel_length,.95f*dot(travel,forward)/travel_length};
                        motion_mock::fake.fixture_sprint=collectible_route<3 &&
                            motion_mock::fake.fixture_move.y>.2f;
                        collectible_settled_world=0;
                    } else {
                        if (!collectible_settled_world) collectible_settled_world=world;
                        if (observed.basic==0 && world>=collectible_settled_world+30 && collectible_attempts<3 &&
                            (!collectible_press_world || world>=collectible_press_world+100)) {
                            collectible_press_world=world;++collectible_attempts;
                        }
                        motion_mock::fake.fixture_use=collectible_press_world &&
                            world<collectible_press_world+8?1.f:0.f;
                    }
                }
                const auto notified=collectible_probe->notified_at();
                if (notified && world>=notified+100 && world<notified+108)
                    motion_mock::fake.fixture_use=1;
            }
            const auto& aim=motion_mock::fake.fixture_aim_rotations[1];
            collectible_fixture<<"{\"world\":"<<world<<",\"hero\":"<<observed.hero
                <<",\"item\":"<<observed.item<<",\"horizontal_distance\":"<<distance
                <<",\"move\":["<<motion_mock::fake.fixture_move.x<<','<<motion_mock::fake.fixture_move.y
                <<"],\"aim\":["<<aim.x<<','<<aim.y<<','<<aim.z<<','<<aim.w
                <<"],\"trigger\":"<<motion_mock::fake.fixture_use<<",\"attempts\":"<<collectible_attempts
                <<",\"route_version\":5,\"route_stage\":"<<collectible_route<<"}\n";
            collectible_fixture.flush();
            if (!collectible_fixture) throw std::runtime_error("Cannot retain native collectible input evidence");
            if (world>=300 && !observed.item && !collectible_probe->collected())
                throw std::runtime_error("No native document was available in the owned collectible scene");
        }
        if (!capture_complete && world && (jump_probe || world%30==0)) {
            std::ofstream output_log(evidence/L"body-tracking-events.jsonl",std::ios::app);
            output_log<<"{\"gameplay_frame\":"<<world<<",\"height\":"<<height
                <<",\"left_tracked\":"<<(motion_mock::fake.lost_left?"false":"true")
                <<",\"focused\":"<<(motion_mock::fake.focused?"true":"false")
                <<",\"move\":["<<motion_mock::fake.fixture_move.x<<','<<motion_mock::fake.fixture_move.y<<']'
                <<",\"jump\":"<<(motion_mock::fake.fixture_jump?"true":"false")
                <<",\"sprint\":"<<(motion_mock::fake.fixture_sprint?"true":"false")<<"}\n";
            output_log.close();
            if (!output_log) throw std::runtime_error("Cannot retain native body tracking schedule");
        }
    }
    if (result!=XR_SUCCESS || !head_turn_probe || world<300 || world>=750) return result;
    if (window_sweep_probe && world>=450) return result;
    float yaw=world<450?1.57079632679f:world<600?-1.57079632679f:3.14159265359f;
    float pitch=float(head_turn_pitch_degrees)*.017453292519943295f;
    if (window_sweep_probe && world<450) {
        const auto phase=float(world-300);
        if (particle_emitter_facing_probe) yaw=0;
        yaw+=.13962634016f*std::sin(phase*.05f);
        pitch+=.05235987756f*std::sin(phase*.07f);
    }
    const auto head=oracle_multiply({0,std::sin(yaw/2),0,std::cos(yaw/2)},
        {std::sin(pitch/2),0,0,std::cos(pitch/2)});
    for (unsigned eye=0;eye<2;++eye) {
        const auto offset=oracle_rotate(head,{eye?.032:-.032,0,0});
        output[eye].pose.orientation=head;
        output[eye].pose.position={float(offset.x),float(offset.y),float(offset.z)};
        if (window_position_probe) {
            const auto translation=native_probe_window_translation(world,material_body_camera_probe);
            output[eye].pose.position.x+=translation[0];
            output[eye].pose.position.y+=translation[1];
            output[eye].pose.position.z+=translation[2];
            if (material_body_camera_probe)
                motion_mock::fake.fixture_head={translation[0],translation[1],translation[2]};
        }
        mock.views[eye]=output[eye];
    }
    return result;
}

XrResult XRAPI_CALL probe_properties(XrInstance instance,XrInstanceProperties* output) {
    if (instance!=mock.instance || !output || output->type!=XR_TYPE_INSTANCE_PROPERTIES)
        return XR_ERROR_VALIDATION_FAILURE;
    std::strcpy(output->runtimeName,"Outlast native headless probe");
    output->runtimeVersion=XR_MAKE_VERSION(1,0,0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL probe_session(XrInstance instance,const XrSessionCreateInfo* info,XrSession* output) {
    const auto result=create_session(instance,info,output);
    if (XR_SUCCEEDED(result)) {
        driver=std::make_unique<IsolatedD3D12DrawCounter>(*mock.device,true,mock.queue);
        mock.counter=driver.get();
    }
    return result;
}

void capture_images() {
    auto* texture=mock.images[mock.image].get();
    const auto description=texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
    UINT64 bytes=0;
    mock.device->GetCopyableFootprints(&description,0,2,0,footprints,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=bytes;buffer.Height=1;
    buffer.DepthOrArraySize=buffer.MipLevels=1;
    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> readback;
    gpu_check(mock.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),
        reinterpret_cast<void**>(readback.put())),"Create native probe readback");
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> commands;
    Com<ID3D12Fence> fence;
    gpu_check(mock.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(allocator.put())),"Create probe allocator");
    gpu_check(mock.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.get(),nullptr,
        __uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(commands.put())),"Create probe commands");
    for (UINT eye=0;eye<2;++eye) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={texture,eye,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION source{},target{};
        source.pResource=texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=eye;
        target.pResource=readback.get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint=footprints[eye];
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        commands->ResourceBarrier(1,&barrier);
    }
    gpu_check(commands->Close(),"Close native probe readback");
    ID3D12CommandList* lists[]{commands.get()};
    mock.queue->ExecuteCommandLists(1,lists);
    gpu_check(mock.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),
        reinterpret_cast<void**>(fence.put())),"Create probe completion fence");
    gpu_check(mock.queue->Signal(fence.get(),1),"Signal probe readback completion");
    const auto deadline=GetTickCount64()+10000;
    while (fence->GetCompletedValue()<1) {
        if (GetTickCount64()>=deadline) throw std::runtime_error("Native probe readback timed out");
        Sleep(1);
    }
    if (fence->GetCompletedValue()==UINT64_MAX)
        throw std::runtime_error("Native probe GPU was removed");
    void* pointer=nullptr;
    D3D12_RANGE range{0,SIZE_T(bytes)};
    gpu_check(readback->Map(0,&range,&pointer),"Map completed native eye images");
    try {
        for (UINT eye=0;eye<2;++eye) {
            const auto file=evidence/(L"frame_"+std::to_wstring(mock.frame)+L"_eye_"+std::to_wstring(eye)+L".ppm");
            std::ofstream image(file,std::ios::binary);
            image<<"P6\n"<<mock.extent<<' '<<mock.height()<<"\n255\n";
            std::vector<char> row(mock.extent*3);
            for (UINT y=0;y<mock.height();++y) {
                const auto* src=static_cast<const unsigned char*>(pointer)+footprints[eye].Offset+
                    std::size_t(y)*footprints[eye].Footprint.RowPitch;
                for (UINT x=0;x<mock.extent;++x) {
                    row[x*3]=char(src[x*4+2]);row[x*3+1]=char(src[x*4+1]);row[x*3+2]=char(src[x*4]);
                }
                image.write(row.data(),std::streamsize(row.size()));
            }
            image.close();
            if (!image) throw std::runtime_error("Native probe eye image write failed");
        }
    } catch (...) {
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);throw;
    }
    D3D12_RANGE written{0,0};readback->Unmap(0,&written);
}

XrResult XRAPI_CALL probe_end_frame(XrSession session,const XrFrameEndInfo* info) {
    if (capture_complete) {
        if (session!=mock.session || !info || !mock.begun || mock.acquired)
            return XR_ERROR_VALIDATION_FAILURE;
        mock.begun=false;
        return XR_SUCCESS;
    }
    if (session==mock.session && info && mock.begun && !info->layerCount) {
        release_owned_enter(mock.submitted,true);
        trace<<"{\"frame\":"<<mock.frame<<",\"canceled\":true}\n";trace.flush();
        mock.begun=false;
        return XR_SUCCESS;
    }
    if (session!=mock.session || !info || !mock.begun || mock.acquired ||
        info->displayTime!=mock.frame*11111111LL || info->layerCount!=1 ||
        info->environmentBlendMode!=XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
        return XR_ERROR_VALIDATION_FAILURE;
    release_owned_enter(mock.submitted+1);
    const auto* layer=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
    if (!layer || layer->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION ||
        layer->viewCount!=2 || layer->space!=mock.space) return XR_ERROR_VALIDATION_FAILURE;
    for (UINT eye=0;eye<2;++eye) {
        const auto& view=layer->views[eye];
        if (view.subImage.swapchain!=mock.swap || view.subImage.imageArrayIndex!=eye ||
            view.subImage.imageRect.extent.width!=int(mock.extent) ||
            view.subImage.imageRect.extent.height!=int(mock.height()) ||
            view.subImage.imageRect.offset.x || view.subImage.imageRect.offset.y ||
            std::memcmp(&view.pose,&mock.views[eye].pose,sizeof(XrPosef)) ||
            std::memcmp(&view.fov,&mock.views[eye].fov,sizeof(XrFovf)))
            return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto capture_serial=mock.submitted+1;
    static unsigned captured_lens_samples=0;
    int lens_sample=-1;
    if (physical_camcorder_probe && captured_lens_samples<3 &&
        std::filesystem::exists(evidence/(L"native-lens-"+std::to_wstring(captured_lens_samples)+L".ppm"))) {
        lens_sample=int(captured_lens_samples++);
    }
    const bool captured=!no_projection_images_probe &&
        (capture_serial<=5 || capture_serial%30==0 || lens_sample>=0);
    if (captured) capture_images();
    using Snapshot=BOOL(WINAPI*)(RuntimeStatistics*);
    const auto mod=GetModuleHandleW(L"OutlastVRSinglePass.dll");
    const auto address=GetProcAddress(mod,"OutlastVR_Statistics");
    Snapshot snapshot=nullptr;
    static_assert(sizeof(snapshot)==sizeof(address));
    std::memcpy(&snapshot,&address,sizeof(snapshot));
    RuntimeStatistics stats{};
    if (!snapshot || !snapshot(&stats)) throw std::runtime_error("Cannot read actual native runtime statistics");
    const bool world_panel=stats.world_panel_draws>previous.world_panel_draws;
    if (pause_menu_probe && continue_progress.gameplay_frames()) {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto engine=occlusion_field<const void*>(reinterpret_cast<const void*>(base),0x201b8e0);
        std::ofstream movies(evidence/L"native-pause-movies.jsonl",std::ios::app);
        movies<<"{\"gameplay_frame\":"<<continue_progress.gameplay_frames()
            <<",\"native_frame\":"<<stats.frames<<",\"world_panel\":"<<(world_panel?"true":"false")
            <<",\"engine\":"<<reinterpret_cast<UINT64>(engine)<<",\"movies\":[";
        bool comma=false;
        for (const std::size_t offset:{std::size_t(0x1c),std::size_t(0xe4)}) {
            const auto count=engine?occlusion_field<INT>(engine,offset+8):0;
            const auto records=engine?occlusion_field<const void* const*>(engine,offset):nullptr;
            if (count<0 || count>128 || (count && !records))
                throw std::runtime_error("Invalid native pause movie inventory");
            for (INT index=0;index<count;++index) {
                const auto record=records[index];
                if (!record) throw std::runtime_error("Null native pause movie record");
                const auto owner=occlusion_field<const void*>(record,0x70);
                movies<<(comma?",":"")<<"{\"list\":"<<offset<<",\"record\":"<<reinterpret_cast<UINT64>(record)
                    <<",\"display\":"<<occlusion_field<UINT64>(record,0x3c)
                    <<",\"field58\":"<<occlusion_field<INT>(record,0x58)
                    <<",\"field68\":"<<occlusion_field<INT>(record,0x68)
                    <<",\"owner\":"<<reinterpret_cast<UINT64>(owner)
                    <<",\"owner_name\":"<<(owner?occlusion_field<UINT64>(owner,0x48):0)
                    <<",\"owner_flags\":"<<(owner?occlusion_field<UINT>(owner,0x114):0)<<'}';
                comma=true;
            }
        }
        movies<<"]}\n";movies.close();
        if (!movies) throw std::runtime_error("Cannot retain native pause movie evidence");
    }
    const auto window=owned_window();
    GUITHREADINFO gui{};gui.cbSize=sizeof(gui);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(window,nullptr),&gui))
        throw std::runtime_error("Cannot observe native focus during the capture");
    trace<<std::setprecision(9)<<"{\"frame\":"<<mock.frame<<",\"gameplay_frame\":"<<continue_progress.gameplay_frames()
        <<",\"captured\":"<<(captured?"true":"false")
        <<",\"lens_sample\":"<<lens_sample
        <<",\"capture_serial\":"<<capture_serial
        <<",\"world_panel\":"<<(world_panel?"true":"false")
        <<",\"native_frame\":"<<stats.frames
        <<",\"eye_width\":"<<mock.extent<<",\"eye_height\":"<<mock.height()
        <<",\"source_width\":"<<stats.source_width<<",\"source_height\":"<<stats.source_height
        <<",\"direct_xr_submissions\":"<<stats.direct_xr_submissions
        <<",\"floor_reference_ready\":"<<(stats.floor_reference_ready?"true":"false")
        <<",\"floor_reference_acquisitions\":"<<stats.floor_reference_acquisitions
        <<",\"standing_reference_m\":"<<stats.standing_reference_m
        <<",\"state_block_allocations\":"<<stats.state_block_allocations
        <<",\"state_block_captures\":"<<stats.state_block_captures
        <<",\"logical_geometry_draws\":"<<stats.geometry_draws-previous.geometry_draws
        <<",\"ui_plane_draws\":"<<stats.ui_plane_draws-previous.ui_plane_draws
        <<",\"tracked_views\":"<<stats.tracked_views-previous.tracked_views
        <<",\"owned_culls\":"<<stats.owned_culls-previous.owned_culls
        <<",\"sphere_inside_culls\":"<<stats.sphere_inside_culls-previous.sphere_inside_culls
        <<",\"crosshair_updates\":"<<stats.crosshair_updates-previous.crosshair_updates
        <<",\"head_turn_probe\":"<<(head_turn_probe?"true":"false")
        <<",\"window_sweep_fixture_version\":"<<(window_sweep_probe?(particle_emitter_facing_probe?3:2):0)
        <<",\"window_position_fixture_version\":"<<(window_position_probe?1:0)
        <<",\"occlusion_observer_fixture_version\":"<<(occlusion_probe?2:0)
        <<",\"menu_pointer_fixture_version\":"<<(menu_pointer_probe?1:0)
        <<",\"menu_pointer_target\":"<<menu_pointer_probe
        <<",\"pause_menu_fixture_version\":"<<(pause_menu_probe?3:0)
        <<",\"startup_aim_isolation_fixture_version\":"<<(continue_probe?1:0)
        <<",\"finger_gait_fixture_version\":"<<(finger_gait_probe?1:0)
        <<",\"head_turn_pitch_degrees\":"<<head_turn_pitch_degrees
        <<",\"gait_probe\":"<<(gait_probe?"true":"false")
        <<",\"gait_schedule_version\":"<<native_probe_gait_version
        <<",\"lean_probe\":"<<(lean_probe?"true":"false")
        <<",\"body_schedule_version\":"<<(corner_recovery_probe?4:3)
        <<",\"jump_probe\":"<<(jump_probe?"true":"false")
        <<",\"jump_schedule_version\":"<<(jump_probe?2:0)
        <<",\"camera_mount_version\":"<<camera_mount_version
        <<",\"no_projection_images_fixture_version\":"<<(material_readback_probe?2:no_projection_images_probe?1:0)
        <<",\"asymmetric_fov_fixture_version\":"<<(asymmetric_fov_probe?1:0)
        <<",\"physical_camcorder_fixture_version\":"<<(physical_camcorder_probe?(material_body_camera_probe?4:7):0)
        <<",\"material_body_camera_fixture_version\":"<<(material_body_camera_probe?3:0)
        <<",\"material_readback_stride\":"<<(material_body_camera_probe?20:1)
        <<",\"camcorder_frame_clock_ns\":"<<camcorder_frame_clock_ns
        <<",\"contact_fixture_version\":"<<(corner_recovery_probe?6:contact_draw_probe?3:0)
        <<",\"focus_fixture_version\":"<<(focus_policy_probe?3:0)
        <<",\"focus_fixture_phase\":"<<focus_probe_phase
        <<",\"owned_foreground\":"<<(GetForegroundWindow()==window?"true":"false")
        <<",\"owned_keyboard_focus\":"<<(gui.hwndFocus==window?"true":"false")
        <<",\"source_width\":"<<stats.source_width<<",\"source_height\":"<<stats.source_height
        <<",\"shader_variants\":"<<stats.shader_variants
        <<",\"xr_wait_us\":"<<stats.xr_wait_us<<",\"cpu_render_us\":"<<stats.cpu_render_us
        <<",\"gpu_timing_status\":"<<stats.gpu_timing_status
        <<",\"gpu_timing_frame\":"<<stats.gpu_timing_frame<<",\"gpu_work_us\":"<<stats.gpu_work_us
        <<",\"consumed_replays\":"<<stats.consumed_replay_calls-previous.consumed_replay_calls
        <<",\"driver_indexed\":"<<driver->indexed_calls<<",\"driver_plain\":"<<driver->plain_calls
        <<",\"driver_draws\":[";
    bool separator=false;
    for (const auto& draw:driver->events()) {
        if (separator) trace<<',';
        trace<<"{\"indexed\":"<<(draw.indexed?"true":"false")<<",\"elements\":"<<draw.elements
            <<",\"instances\":"<<draw.instances<<'}';separator=true;
    }
    trace<<"],\"views\":[";
    for (UINT eye=0;eye<2;++eye) {
        if (eye) trace<<',';
        const auto& v=mock.views[eye];
        trace<<"{\"position\":["<<v.pose.position.x<<','<<v.pose.position.y<<','<<v.pose.position.z
            <<"],\"orientation\":["<<v.pose.orientation.x<<','<<v.pose.orientation.y<<','
            <<v.pose.orientation.z<<','<<v.pose.orientation.w<<"],\"fov\":["<<v.fov.angleLeft
            <<','<<v.fov.angleRight<<','<<v.fov.angleUp<<','<<v.fov.angleDown<<"]}";
    }
    trace<<"]}\n";trace.flush();
    if (!trace) throw std::runtime_error("Native probe trace write failed");
    if (occlusion_probe) {
        std::lock_guard<std::mutex> lock(occlusion_mutex);
        occlusion_trace.flush();
        if (!occlusion_trace) throw std::runtime_error("Cannot flush native occlusion evidence");
    }
    previous=stats;driver->reset();
    mock.begun=false;++mock.submitted;
    if (continue_probe) {
        if (continue_progress.observe(mock.submitted,world_panel)) {
            const auto input=press_owned_enter();
            std::ofstream inputs(evidence/L"menu-input-attempts.jsonl",std::ios::app);
            inputs<<"{\"submitted\":"<<mock.submitted<<",\"continue_attempt\":"
                <<continue_progress.continue_attempts()<<",\"gameplay_confirmed\":false,\"owned_foreground\":"
                <<(input.foreground?"true":"false")<<",\"owned_keyboard_focus\":"<<(input.focus?"true":"false")
                <<",\"owned_active_window\":"<<(input.active?"true":"false")
                <<",\"window_proc_rva\":"<<input.window_proc_rva
                <<",\"input_method\":\"native_device_window_messages\""
                <<",\"scheduled_openxr_primary\":"<<(body_probe?"true":"false")
                <<",\"key_hold_projections\":3}\n";
            inputs.close();
            if (!inputs) throw std::runtime_error("Cannot record owned menu input attempt");
        }
        if (continue_progress.expired(mock.submitted))
            throw std::runtime_error("Continue input did not produce 900 gameplay frames");
        startup_pad_state.store(continue_progress.startup_only()?
            (UINT64(mock.submitted)<<2)|2|(held_enter_window?1:0):0,std::memory_order_release);
        if (gait_probe && continue_progress.gameplay_frames()) gait_input();
        const auto world=continue_progress.gameplay_frames();
        if (pause_menu_probe && (world==120 || world==126)) {
            const bool down=world==120;
            const LPARAM bits=1|(LPARAM(MapVirtualKeyW(VK_ESCAPE,MAPVK_VK_TO_VSC))<<16)|
                (down?0:LPARAM(0xc0000000u))|native_probe_key_marker;
            if (!PostMessageW(owned_window(),down?WM_KEYDOWN:WM_KEYUP,VK_ESCAPE,bits))
                throw std::runtime_error("Cannot seed the owned native pause inspection");
            std::ofstream input(evidence/L"native-pause-seed.jsonl",std::ios::app);
            input<<"{\"gameplay_frame\":"<<world<<",\"key\":"<<VK_ESCAPE
                <<",\"down\":"<<(down?"true":"false")<<",\"native_owned_keyboard_seed\":true}\n";
            input.close();
            if (!input) throw std::runtime_error("Cannot retain the native pause seed");
        }
    }
    if (mock.failed) return XR_ERROR_VALIDATION_FAILURE;
    if ((!continue_probe && mock.submitted==probe_frames) ||
        (continue_probe && (pause_menu_probe?continue_progress.gameplay_frames()>=300:continue_progress.complete()) &&
            (!focus_policy_probe || focus_probe_finished))) {
        if (collectible_probe && !collectible_probe->collected())
            throw std::runtime_error("Native document collection did not complete; frame count is not pickup acceptance");
        using Complete=void(WINAPI*)();
        const auto address=GetProcAddress(GetModuleHandleW(L"OutlastVRSinglePass.dll"),"OutlastVR_CompleteProbe");
        Complete seal=nullptr;
        static_assert(sizeof(seal)==sizeof(address));
        std::memcpy(&seal,&address,sizeof(seal));
        if (!seal) throw std::runtime_error("Native probe lacks synchronized diagnostic completion");
        seal();
        if (occlusion_probe) {
            std::lock_guard<std::mutex> lock(occlusion_mutex);
            occlusion_trace.close();
            if (!occlusion_trace) throw std::runtime_error("Cannot seal completed native occlusion evidence");
        }
        trace.close();
        if (!trace) throw std::runtime_error("Cannot seal completed native driver trace");
        capture_complete=true;
        std::ofstream complete(evidence/L"capture-complete.txt");
        complete<<"Captured "<<mock.submitted<<" projected Outlast frames; native diagnostic streams sealed; this marker is not a stereo verdict.\n";
        complete.close();
        if (!complete) throw std::runtime_error("Cannot finalize native probe capture");
    }
    return XR_SUCCESS;
}
}

extern "C" __declspec(dllexport) BOOL WINAPI OutlastVRProbe_GlassWorldFrameV2(UINT64* frame) {
    if (!frame || !window_sweep_probe || !head_turn_probe || !continue_probe) return FALSE;
    *frame=continue_progress.gameplay_frames();
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI OutlastVRProbe_StartupPadV1(DWORD* packet,BOOL* pressed) {
    if (!packet || !pressed) return FALSE;
    const auto state=startup_pad_state.load(std::memory_order_acquire);
    *packet=DWORD(state>>2);*pressed=(state&1)?TRUE:FALSE;
    return (state&2)?TRUE:FALSE;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance instance,const char* name,PFN_xrVoidFunction* output) {
    try {
        static std::once_flag initialization;
        std::call_once(initialization,initialize_probe);
        if (!name || !output) return XR_ERROR_VALIDATION_FAILURE;
        if (std::strcmp(name,"xrGetInstanceProperties")==0) export_function(output,&probe_properties);
        else if (std::strcmp(name,"xrCreateSession")==0) export_function(output,&probe_session);
        else if (std::strcmp(name,"xrEndFrame")==0) export_function(output,&probe_end_frame);
        else if (std::strcmp(name,"xrLocateViews")==0) export_function(output,&probe_locate);
        else if (std::strcmp(name,"xrGetActionStatePose")==0) export_function(output,&probe_pose_state);
        else if (std::strcmp(name,"xrEnumerateViewConfigurationViews")==0) export_function(output,&probe_configuration);
        else return resolve(instance,name,output);
        return XR_SUCCESS;
    } catch (const std::exception& error) {
        std::ofstream failure(evidence/L"capture-error.txt");
        failure<<error.what()<<'\n';
        return XR_ERROR_RUNTIME_FAILURE;
    }
}
