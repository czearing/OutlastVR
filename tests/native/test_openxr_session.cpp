#include "isolated_d3d9.h"
#include "openxr_loader.h"
#include "d3d9_stereo_pipeline.h"
#include "d3d9_shader_variants.h"
#include "isolated_d3d12_counter.h"
#include "xr_view_constants.h"
#define OLVR_MOTION_MOCK_ONLY
#include "test_openxr_motion_input.cpp"
#undef OLVR_MOTION_MOCK_ONLY
#include <atomic>
#include <future>
#include <iostream>
#include <optional>

using namespace olvr;

namespace {
std::atomic<bool> release_observed{false},gate_opened{false};
struct OracleVector {double x,y,z;};
OracleVector oracle_add(OracleVector a,OracleVector b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
OracleVector oracle_scale(OracleVector a,double b){return {a.x*b,a.y*b,a.z*b};}
double oracle_dot(OracleVector a,OracleVector b){return a.x*b.x+a.y*b.y+a.z*b.z;}
OracleVector oracle_rotate(XrQuaternionf q,OracleVector v) {
    const double x=q.x,y=q.y,z=q.z,w=q.w;
    return {(1-2*y*y-2*z*z)*v.x+(2*x*y-2*z*w)*v.y+(2*x*z+2*y*w)*v.z,
        (2*x*y+2*z*w)*v.x+(1-2*x*x-2*z*z)*v.y+(2*y*z-2*x*w)*v.z,
        (2*x*z-2*y*w)*v.x+(2*y*z+2*x*w)*v.y+(1-2*x*x-2*y*y)*v.z};
}
XrQuaternionf oracle_multiply(XrQuaternionf a,XrQuaternionf b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
struct Mock {
    bool native_probe = false;
    bool motion_focus_test=false,motion_focus_fault=false;
    UINT extent = 256;
    UINT extent_height = 0;
    UINT height() const {return extent_height?extent_height:extent;}
    LUID luid{};
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    Com<ID3D12Resource> images[2];
    XrInstance instance = reinterpret_cast<XrInstance>(1);
    XrSession session = reinterpret_cast<XrSession>(2);
    XrSpace space = reinterpret_cast<XrSpace>(3);
    XrSwapchain swap = reinterpret_cast<XrSwapchain>(4);
    UINT frame = 0, submitted = 0, blank = 0, pixels = 0, destroyed = 0;
    bool ready = false, running = false, begun = false, acquired = false, waited = false;
    bool requirements = false, failed = false, stopping = false, stopped = false;
    bool invalid_pose = false;
    bool tracking_resume=false,asymmetric_extents=false,dynamic_ipd=false;
    UINT tracking_gaps=0,session_begins=0,session_ends=0;
    std::optional<XrSessionState> queued_state;
    std::string startup_error;
    bool created_instance=false,created_session=false,created_space=false,created_swap=false;
    float ipd() const {return !dynamic_ipd || frame<7?.064f:frame<12?.078f:frame<17?.050f:.072f;}
    bool reference_test=false,stage_supported=false,origins_queued=false;
    bool floor_startup_test=false;
    std::vector<XrEventDataReferenceSpaceChangePending> origin_events;
    bool gated_release_test=false,fault_serial_wait=false;
    UINT gated_releases=0;
    bool geometry = false;
    bool recentered = false;
    float comfort_world_scale=1;
    bool comfort_seated=false;
    bool snap_test=false;
    bool flat_test=false,flat_frame=false,flat_after_world=false;
    bool ui=false,ui_scissor=false;
    bool upright_camera=false;
    XrVector3f native_camera_offset_m{};
    RECT ui_viewport{0,0,256,256};
    XrQuaternionf ui_head{0,0,0,1};
    UINT ui_checked=0;
    UINT expected_instances = 2;
    IsolatedD3D12DrawCounter* counter = nullptr;
    double maximum_disparity_error = 0;
    UINT invalid_poses = 0;
    UINT image = 0;
    std::array<XrView, 2> views{};
    DWORD color(UINT eye) const {
        return eye ? 0xff005500 | ((frame * 7) << 16) | frame
                   : 0xff000077 | (frame << 16) | ((frame * 11) << 8);
    }
    void expect(bool value,unsigned line=__builtin_LINE()) {
        if (!value) {failed=true;std::cerr<<"OpenXR mock contract failed at line "<<line<<'\n';}
    }
} mock;
class UIOracle {
    OracleVector center_,right_,up_,forward_,eyes_[2],eye_right_[2],eye_up_[2],eye_forward_[2];
    double extent_=1e9;
public:
    UIOracle() {
        right_=oracle_rotate(mock.ui_head,{1,0,0});up_=oracle_rotate(mock.ui_head,{0,1,0});
        forward_=oracle_rotate(mock.ui_head,{0,0,-1});
        OracleVector midpoint{};
        for (unsigned eye=0;eye<2;++eye) {
            const auto& p=mock.views[eye].pose;
            eyes_[eye]={p.position.x,p.position.y,p.position.z};
            midpoint=oracle_add(midpoint,oracle_scale(eyes_[eye],.5));
            eye_right_[eye]=oracle_rotate(p.orientation,{1,0,0});
            eye_up_[eye]=oracle_rotate(p.orientation,{0,1,0});
            eye_forward_[eye]=oracle_rotate(p.orientation,{0,0,-1});
        }
        center_=oracle_add(midpoint,oracle_scale(forward_,2));
        for (unsigned eye=0;eye<2;++eye) {
            const auto& f=mock.views[eye].fov;
            const OracleVector normals[]{
                oracle_add(eye_right_[eye],oracle_scale(eye_forward_[eye],-std::tan(double(f.angleLeft)))),
                oracle_add(oracle_scale(eye_right_[eye],-1),oracle_scale(eye_forward_[eye],std::tan(double(f.angleRight)))),
                oracle_add(eye_up_[eye],oracle_scale(eye_forward_[eye],-std::tan(double(f.angleDown)))),
                oracle_add(oracle_scale(eye_up_[eye],-1),oracle_scale(eye_forward_[eye],std::tan(double(f.angleUp))))};
            for (auto normal:normals) {
                const auto available=oracle_dot(normal,oracle_add(center_,oracle_scale(eyes_[eye],-1)));
                const auto extent=std::abs(oracle_dot(normal,right_))+std::abs(oracle_dot(normal,up_));
                if (available<=0 || extent<=0) throw std::runtime_error("Independent menu plane does not fit the eye frustum");
                extent_=std::min(extent_,available/extent);
            }
        }
        // These DLL fixtures have no interactive movie owner: their canvas is passive HUD.
        constexpr double radians=3.14159265358979323846/180;
        extent_=std::min({extent_,2*std::tan(25*radians),2*std::tan(35*radians)});
    }
    int label(unsigned eye,unsigned x,unsigned y,double& margin) const {
        const auto& f=mock.views[eye].fov;
        // D3D9On12 retains D3D9's integer pixel-center convention.
        const auto ray_x=std::tan(double(f.angleLeft))+
            (std::tan(double(f.angleRight))-std::tan(double(f.angleLeft)))*x/256;
        const auto ray_y=std::tan(double(f.angleUp))+
            (std::tan(double(f.angleDown))-std::tan(double(f.angleUp)))*y/256;
        const auto ray=oracle_add(eye_forward_[eye],oracle_add(
            oracle_scale(eye_right_[eye],ray_x),oracle_scale(eye_up_[eye],ray_y)));
        const auto t=oracle_dot(oracle_add(center_,oracle_scale(eyes_[eye],-1)),forward_)/oracle_dot(ray,forward_);
        const auto point=oracle_add(oracle_add(eyes_[eye],oracle_scale(ray,t)),oracle_scale(center_,-1));
        const auto canvas_x=oracle_dot(point,right_)/extent_,canvas_y=oracle_dot(point,up_)/extent_;
        const auto pixel_x=128*(canvas_x+1),pixel_y=128*(1-canvas_y);
        const auto& viewport=mock.ui_viewport;
        const auto u=2*(pixel_x-viewport.left)/(viewport.right-viewport.left)-1;
        const auto v=1-2*(pixel_y-viewport.top)/(viewport.bottom-viewport.top);
        margin=1e9;
        if (mock.ui_scissor) {
            margin=std::min({margin,std::abs(pixel_x-30)/128,std::abs(pixel_x-195.5)/128,
                std::abs(pixel_y-32)/128,std::abs(pixel_y-223.5)/128});
            if (pixel_x<30 || pixel_x>195.5 || pixel_y<32 || pixel_y>223.5) return -1;
        }
        int result=-1;
        for (unsigned group=0;group<2;++group) {
            const double dx=std::abs(u-(group==0?-.4:.45)),dy=std::abs(v-(group==0?-.25:.35));
            margin=std::min({margin,std::abs(dx-.13),std::abs(dy-.13)});
            if (dx<.13 && dy<.13) result=int(group);
        }
        return result;
    }
};
class SnapOracle {
public:
    int label(unsigned eye,unsigned x,unsigned y,double& margin) const {
        const auto& view=mock.views[eye];
        const auto& f=view.fov;
        const double yaw=mock.frame>=2?-3.14159265358979323846/12:0;
        const double c=std::cos(yaw),s=std::sin(yaw);
        const double pivot=mock.frame>=2?.001:0;
        const double dx=view.pose.position.x-.001-pivot,dz=view.pose.position.z+.02;
        const OracleVector origin{pivot+c*dx+s*dz,view.pose.position.y-.01,-s*dx+c*dz};
        const double rx=std::tan(double(f.angleLeft))+
            (std::tan(double(f.angleRight))-std::tan(double(f.angleLeft)))*x/256;
        const double ry=std::tan(double(f.angleUp))+
            (std::tan(double(f.angleDown))-std::tan(double(f.angleUp)))*y/256;
        const OracleVector ray{c*rx-s,ry,-s*rx-c};
        int result=-1;margin=1e9;
        for (unsigned group=0;group<2;++group) {
            const double depth=group==0?2.:6.,radius=depth*.1;
            const double t=(-depth-origin.z)/ray.z;
            if (t<=0) continue;
            const double px=origin.x+t*ray.x,py=origin.y+t*ray.y;
            const double distance_x=std::abs(px-(group==0?-.6:.9));
            const double distance_y=std::abs(py-(group==0?-.35:.6));
            margin=std::min({margin,std::abs(distance_x-radius)/depth,std::abs(distance_y-radius)/depth});
            if (distance_x<radius && distance_y<radius) result=int(group);
        }
        return result;
    }
};
class FlatOracle {
    OracleVector center_{};
    double extent_=1e9;
public:
    FlatOracle() {
        const unsigned start=mock.frame-(mock.frame-2)%5;
        center_={start*.001,start*.01,-2.02};
        for (unsigned eye=0;eye<2;++eye) {
            const auto& f=mock.views[eye].fov;
            const double eye_x=start*.001+(eye?.032:-.032);
            extent_=std::min({extent_,
                2*std::tan(-double(f.angleLeft))+center_.x-eye_x,
                2*std::tan(double(f.angleRight))-center_.x+eye_x,
                2*std::tan(double(f.angleUp)),2*std::tan(-double(f.angleDown))});
        }
    }
    int label(unsigned eye,unsigned x,unsigned y,double& margin) const {
        const auto& view=mock.views[eye];
        const auto& f=view.fov;
        const double rx=std::tan(double(f.angleLeft))+
            (std::tan(double(f.angleRight))-std::tan(double(f.angleLeft)))*x/256;
        const double ry=std::tan(double(f.angleUp))+
            (std::tan(double(f.angleDown))-std::tan(double(f.angleUp)))*y/256;
        const auto ray=oracle_rotate(view.pose.orientation,{rx,ry,-1});
        const double t=(center_.z-view.pose.position.z)/ray.z;
        const double px=128*(1+(view.pose.position.x+t*ray.x-center_.x)/extent_);
        const double py=128*(1-(view.pose.position.y+t*ray.y-center_.y)/extent_);
        margin=1e9;int result=-1;
        for (unsigned group=0;group<2;++group) {
            const double left=group==0?40.5:150.5,right=group==0?103.5:213.5;
            const double top=group==0?145.5:50.5,bottom=group==0?208.5:113.5;
            margin=std::min({margin,std::abs(px-left)/256,std::abs(px-right)/256,
                std::abs(py-top)/256,std::abs(py-bottom)/256});
            if (t>0 && px>=left && px<=right && py>=top && py<=bottom) result=int(group);
        }
        return result;
    }
};

XrResult XRAPI_CALL extensions(const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* output) {
    *count = 1;
    if (capacity) {
        mock.expect(output && output->type == XR_TYPE_EXTENSION_PROPERTIES);
        std::strcpy(output->extensionName, mock.startup_error=="extension"?
            XR_EXT_DEBUG_UTILS_EXTENSION_NAME:XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        output->extensionVersion = 9;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create_instance(const XrInstanceCreateInfo* info, XrInstance* output) {
    mock.expect(info->enabledExtensionCount == 1 &&
                std::strcmp(info->enabledExtensionNames[0], XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0);
    *output = mock.instance;
    mock.created_instance=true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_instance(XrInstance instance) {
    mock.expect(instance == mock.instance &&
        mock.destroyed==UINT(mock.created_swap)+UINT(mock.created_space)+UINT(mock.created_session));
    ++mock.destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL system(XrInstance, const XrSystemGetInfo* info, XrSystemId* output) {
    mock.expect(info->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY);
    *output = 1;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL requirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* output) {
    output->adapterLuid = mock.luid;
    if (mock.startup_error=="adapter") output->adapterLuid={0xffffffff,0x7fffffff};
    output->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    mock.requirements = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL configuration(XrInstance, XrSystemId, XrViewConfigurationType type,
                                  uint32_t capacity, uint32_t* count, XrViewConfigurationView* output) {
    mock.expect(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    *count = 2;
    if (mock.startup_error=="view-count") *count=1;
    if (capacity) for (UINT eye = 0; eye < 2; ++eye) {
        output[eye].recommendedImageRectWidth = mock.asymmetric_extents?(eye?256:320):mock.extent;
        output[eye].recommendedImageRectHeight = mock.asymmetric_extents?(eye?272:240):mock.height();
        output[eye].maxImageRectWidth = mock.asymmetric_extents?(eye?256:320):1024;
        output[eye].maxImageRectHeight = mock.asymmetric_extents?(eye?272:240):1024;
        output[eye].recommendedSwapchainSampleCount = output[eye].maxSwapchainSampleCount = 1;
        if (mock.startup_error=="limits") output[eye].recommendedImageRectWidth=0;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL modes(XrInstance, XrSystemId, XrViewConfigurationType,
                          uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* output) {
    *count = 1;
    if (capacity) *output = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create_session(XrInstance, const XrSessionCreateInfo* info, XrSession* output) {
    const auto* binding = static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
    mock.expect(mock.requirements && binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR);
    mock.device = binding->device;
    mock.queue = binding->queue;
    const auto luid = mock.device->GetAdapterLuid();
    mock.expect(luid.LowPart == mock.luid.LowPart && luid.HighPart == mock.luid.HighPart);
    *output = mock.session;
    mock.created_session=true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_session(XrSession) {
    mock.expect(!mock.begun && !mock.acquired &&
        mock.destroyed==UINT(mock.created_swap)+UINT(mock.created_space));
    ++mock.destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL begin_session(XrSession, const XrSessionBeginInfo* info) {
    mock.expect(!mock.running && info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    mock.expect(mock.native_probe || motion_mock::fake.attached==1);
    mock.running = true;
    ++mock.session_begins;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL end_session(XrSession) {
    mock.expect(mock.running && !mock.begun);
    mock.running = false;
    mock.stopped = true;
    ++mock.session_ends;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL poll(XrInstance, XrEventDataBuffer* buffer) {
    if (mock.queued_state) {
        auto* event=reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
        event->type=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;event->session=mock.session;
        event->state=*mock.queued_state;mock.queued_state.reset();
        return XR_SUCCESS;
    }
    if (mock.reference_test && mock.ready && !mock.origins_queued) {
        mock.origins_queued=true;
        auto change=[&](XrSession session,XrReferenceSpaceType type,UINT frame,bool valid,float y) {
            XrEventDataReferenceSpaceChangePending event{};
            event.type=XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
            event.session=session;event.referenceSpaceType=type;event.changeTime=frame*11111111LL;
            event.poseValid=valid;event.poseInPreviousSpace.orientation.w=valid?1:0;
            event.poseInPreviousSpace.position.y=y;
            mock.origin_events.push_back(event);
        };
        const auto selected=mock.stage_supported?XR_REFERENCE_SPACE_TYPE_STAGE:XR_REFERENCE_SPACE_TYPE_LOCAL;
        change(mock.session,selected,7,true,-.1f);
        change(mock.session,XR_REFERENCE_SPACE_TYPE_LOCAL,3,false,0);
        change(mock.session,selected,5,true,.2f);
        change(reinterpret_cast<XrSession>(99),selected,2,true,0);
        change(mock.session,XR_REFERENCE_SPACE_TYPE_VIEW,2,true,0);
        change(mock.session,XR_REFERENCE_SPACE_TYPE_LOCAL,7,false,0);
    }
    if (!mock.origin_events.empty()) {
        std::memcpy(buffer,&mock.origin_events.front(),sizeof(mock.origin_events.front()));
        mock.origin_events.erase(mock.origin_events.begin());
        return XR_SUCCESS;
    }
    if (mock.ready && !mock.stopping) return XR_EVENT_UNAVAILABLE;
    if (mock.stopped) return XR_EVENT_UNAVAILABLE;
    auto* event = reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
    event->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    event->session = mock.session;
    event->state = mock.stopping ? XR_SESSION_STATE_STOPPING : XR_SESSION_STATE_READY;
    mock.ready = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL reference_spaces(XrSession session,uint32_t capacity,uint32_t* count,
                                    XrReferenceSpaceType* output) {
    mock.expect(session==mock.session);
    *count=mock.stage_supported?3:2;
    if (capacity) {
        mock.expect(capacity>=*count);
        output[0]=XR_REFERENCE_SPACE_TYPE_VIEW;output[1]=XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (mock.stage_supported) output[2]=XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create_space(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* output) {
    mock.expect(info->referenceSpaceType == (mock.stage_supported?
                XR_REFERENCE_SPACE_TYPE_STAGE:XR_REFERENCE_SPACE_TYPE_LOCAL) &&
                info->poseInReferenceSpace.orientation.w == 1);
    *output = mock.space;
    mock.created_space=true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace space) {
    if (space!=mock.space) return motion_mock::destroy_space(space);
    mock.expect(mock.destroyed == UINT(mock.created_swap));
    ++mock.destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL formats(XrSession, uint32_t capacity, uint32_t* count, int64_t* output) {
    *count = 1;
    if (capacity) *output = mock.startup_error=="format"?
        DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create_swap(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output) {
    mock.expect(info->width == mock.extent && info->height == mock.height() && info->arraySize == 2 &&
                info->sampleCount == 1 && info->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
                (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT));
    if (mock.startup_error=="swapchain") return XR_ERROR_RUNTIME_FAILURE;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = mock.extent;
    description.Height = mock.height();
    description.DepthOrArraySize = 2;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (auto& image : mock.images)
        gpu_check(mock.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                       __uuidof(ID3D12Resource),
                                                       reinterpret_cast<void**>(image.put())), "Create mock eye image");
    *output = mock.swap;
    mock.created_swap=true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_swap(XrSwapchain) {
    mock.expect(!mock.acquired && mock.destroyed == 0);
    for (auto& image : mock.images) image.reset();
    ++mock.destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL images(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* output) {
    *count = 2;
    if (capacity) {
        auto* entries = reinterpret_cast<XrSwapchainImageD3D12KHR*>(output);
        for (UINT index = 0; index < 2; ++index) {
            mock.expect(entries[index].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR);
            entries[index].texture = mock.images[index].get();
            if (mock.startup_error=="images") entries[index].texture=nullptr;
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL wait_frame(XrSession, const XrFrameWaitInfo*, XrFrameState* output) {
    mock.expect(mock.running && !mock.begun && !mock.acquired);
    ++mock.frame;
    output->predictedDisplayTime = mock.frame * 11111111LL;
    output->predictedDisplayPeriod = 11111111LL;
    output->shouldRender = mock.native_probe || mock.frame % 5 != 0;
    if (mock.native_probe) Sleep(11);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL begin_frame(XrSession, const XrFrameBeginInfo*) {
    mock.expect(!mock.begun);
    mock.begun = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                           uint32_t capacity, uint32_t* count, XrView* output) {
    mock.expect(mock.begun && capacity == 2 && info->space == mock.space &&
                info->displayTime == mock.frame * 11111111LL);
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
    for (UINT eye = 0; eye < 2; ++eye) {
        output[eye].pose.orientation.w = 1;
        output[eye].pose.position = {mock.frame * .001f + (eye ? .5f : -.5f)*mock.ipd(), .01f * mock.frame, -.02f};
        output[eye].fov = {-.8f + eye * .03f, .85f + eye * .03f, .75f, -.7f};
        mock.views[eye] = output[eye];
    }
    if (mock.flat_frame && mock.frame%5==3) {
        const auto q=oracle_multiply({0,std::sin(.1f),0,std::cos(.1f)},
            {std::sin(-.06f),0,0,std::cos(-.06f)});
        for (unsigned eye=0;eye<2;++eye) {
            const auto offset=oracle_rotate(q,{eye?.032:-.032,0,0});
            output[eye].pose.orientation=q;
            output[eye].pose.position={float(mock.frame*.001+offset.x),float(mock.frame*.01+offset.y),
                float(-.02+offset.z)};
            mock.views[eye]=output[eye];
        }
    }
    if (mock.ui) {
        const float yaw=mock.frame%3==0?-.7f:mock.frame%3==1?.4f:0;
        const float pitch=mock.frame%2?.25f:-.3f,roll=mock.frame%4?.35f:-.5f;
        mock.ui_head=oracle_multiply(oracle_multiply({0,std::sin(yaw/2),0,std::cos(yaw/2)},
            {std::sin(pitch/2),0,0,std::cos(pitch/2)}),{0,0,std::sin(roll/2),std::cos(roll/2)});
        for (UINT eye=0;eye<2;++eye) {
            const float cant=eye?.06f:-.06f;
            output[eye].pose.orientation=oracle_multiply(mock.ui_head,{0,std::sin(cant/2),0,std::cos(cant/2)});
            const auto offset=oracle_rotate(mock.ui_head,{eye?.032:-.032,0,0});
            output[eye].pose.position={float(offset.x+mock.frame*.001),float(offset.y+.01*mock.frame),float(offset.z-.02)};
            output[eye].fov=eye?XrFovf{-.53f,.91f,.78f,-.65f}:XrFovf{-.95f,.55f,.72f,-.69f};
            mock.views[eye]=output[eye];
        }
    }
    if (mock.native_probe) {
        const float yaw=.08f*std::sin(float(mock.frame)*.005f);
        const XrQuaternionf head{0,std::sin(yaw/2),0,std::cos(yaw/2)};
        for (UINT eye=0;eye<2;++eye) {
        const auto offset=oracle_rotate(head,{eye?.032:-.032,0,0});
        output[eye].pose.orientation=head;
        output[eye].pose.position={float(offset.x),float(offset.y),float(offset.z)};
        output[eye].fov={-.8f,.8f,.8f,-.8f};
        mock.views[eye]=output[eye];
        }
    }
    if (mock.floor_startup_test) {
        const float height=mock.frame<=2?0.f:mock.frame<=4?1.7f:mock.frame<=6?0.f:
            mock.frame<=12?1.7f:1.2f;
        if (mock.frame<=4)
            state->viewStateFlags&=~(XR_VIEW_STATE_POSITION_TRACKED_BIT|XR_VIEW_STATE_ORIENTATION_TRACKED_BIT);
        for (UINT eye=0;eye<2;++eye) {
            output[eye].pose.position.y=height;
            mock.views[eye]=output[eye];
        }
    }
    if (mock.invalid_pose) {
        output[0].pose.orientation.w = 2;
        ++mock.invalid_poses;
    }
    if (mock.tracking_resume) {
        if (mock.frame==2 || mock.frame==3 || mock.frame==7 || mock.frame==8) {
            state->viewStateFlags=mock.frame==7?XR_VIEW_STATE_POSITION_VALID_BIT:
                mock.frame==8?XR_VIEW_STATE_ORIENTATION_VALID_BIT:0;
            if (mock.frame==3) *count=0;
            for (auto& view:std::array<XrView*,2>{output,output+1}) {
                view->pose.position.x=std::numeric_limits<float>::quiet_NaN();
                view->pose.orientation.w=std::numeric_limits<float>::quiet_NaN();
            }
            ++mock.tracking_gaps;
        } else if (mock.frame==9) {
            state->viewStateFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* output) {
    mock.expect(mock.begun && !mock.acquired && (mock.native_probe || mock.frame % 5 != 0));
    mock.acquired = true;
    mock.waited = false;
    mock.image = mock.frame % 2;
    *output = mock.image;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL wait_image(XrSwapchain, const XrSwapchainImageWaitInfo* info) {
    mock.expect(mock.acquired && info->timeout > 0);
    mock.waited = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL release(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    mock.expect(mock.acquired && mock.waited);
    if (mock.gated_release_test) {
        if (mock.fault_serial_wait)
            while (!gate_opened.load(std::memory_order_acquire)) Sleep(1);
        const bool blocked=!gate_opened.load(std::memory_order_acquire);
        if (blocked) ++mock.gated_releases;
        release_observed.store(true,std::memory_order_release);
    }
    mock.acquired = false;
    return XR_SUCCESS;
}

bool inspect_gpu_eyes() {
    auto* texture = mock.images[mock.image].get();
    const auto description = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
    UINT64 bytes = 0;
    mock.device->GetCopyableFootprints(&description, 0, 2, 0, footprints, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> readback;
    gpu_check(mock.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   __uuidof(ID3D12Resource),
                                                   reinterpret_cast<void**>(readback.put())), "Create compositor readback");
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> commands;
    Com<ID3D12Fence> fence;
    gpu_check(mock.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 __uuidof(ID3D12CommandAllocator),
                                                 reinterpret_cast<void**>(allocator.put())), "Create compositor allocator");
    gpu_check(mock.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                            __uuidof(ID3D12GraphicsCommandList),
                                            reinterpret_cast<void**>(commands.put())), "Create compositor list");
    for (UINT eye = 0; eye < 2; ++eye) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = eye;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, target{};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = eye;
        target.pResource = readback.get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = footprints[eye];
        commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
    }
    gpu_check(commands->Close(), "Close compositor commands");
    ID3D12CommandList* lists[]{commands.get()};
    mock.queue->ExecuteCommandLists(1, lists);
    gpu_check(mock.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                      reinterpret_cast<void**>(fence.put())), "Create compositor fence");
    gpu_check(mock.queue->Signal(fence.get(), 1), "Signal compositor readback");
    const auto deadline = GetTickCount64() + 10000;
    while (fence->GetCompletedValue() < 1) {
        if (GetTickCount64() >= deadline) throw std::runtime_error("Compositor inspection timed out");
        Sleep(1);
    }
    if (fence->GetCompletedValue() == UINT64_MAX) throw std::runtime_error("Compositor GPU removed");
    void* pointer = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    gpu_check(readback->Map(0, &range, &pointer), "Read actual submitted stereo contents");
    bool valid = true;
    double sums[2][2]{};
    double y_sums[2][2]{},expected_sums[2][2]{},expected_y[2][2]{};
    UINT counts[2][2]{};
    UINT expected_counts[2][2]{};
    const auto ui=mock.ui?std::make_unique<UIOracle>():nullptr;
    const auto snap=mock.snap_test?std::make_unique<SnapOracle>():nullptr;
    const auto flat=mock.flat_frame?std::make_unique<FlatOracle>():nullptr;
    for (UINT eye = 0; eye < 2; ++eye)
        for (UINT y = 0; y < mock.height(); ++y)
            for (UINT x = 0; x < mock.extent; ++x) {
                DWORD pixel;
                std::memcpy(&pixel, static_cast<const char*>(pointer) + footprints[eye].Offset +
                            y * footprints[eye].Footprint.RowPitch + x * 4, 4);
                if (mock.geometry) {
                    const auto color = pixel & 0xffffff;
                    const int group = color == 0xff0000 ? 0 : color == 0x00ff00 ? 1 : -1;
                    if (group >= 0) {
                        sums[eye][group] += x;
                        y_sums[eye][group] += y;
                        ++counts[eye][group];
                    }
                    if (ui || snap || flat) {
                        double margin=0;
                        const int expected=ui?ui->label(eye,x,y,margin):snap?
                            snap->label(eye,x,y,margin):flat->label(eye,x,y,margin);
                        if (expected>=0) {
                            expected_sums[eye][expected]+=x;expected_y[eye][expected]+=y;
                            ++expected_counts[eye][expected];
                        }
                        if (margin>(ui?.03:.01)) {
                            valid=valid && group==expected;
                            ++mock.ui_checked;
                        }
                    }
                } else {
                    valid = valid && pixel == mock.color(eye);
                }
                ++mock.pixels;
            }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    if (mock.geometry) {
        for (UINT group = 0; group < 2; ++group) {
            if (counts[0][group] < 100 || counts[1][group] < 100) {
                std::cerr<<"Compositor geometry coverage failed: group="<<group
                    <<"; left="<<counts[0][group]<<"; right="<<counts[1][group]<<'\n';
                return false;
            }
            if (ui || snap || flat) {
                for (unsigned eye=0;eye<2;++eye) {
                    if (expected_counts[eye][group]<100) throw std::runtime_error("Independent projected scene coverage was insufficient");
                    const auto x_error=std::abs(sums[eye][group]/counts[eye][group]-
                        expected_sums[eye][group]/expected_counts[eye][group]);
                    const auto y_error=std::abs(y_sums[eye][group]/counts[eye][group]-
                        expected_y[eye][group]/expected_counts[eye][group]);
                    mock.maximum_disparity_error=std::max({mock.maximum_disparity_error,x_error,y_error});
                    valid=valid && x_error<=1 && y_error<=1;
                    if (x_error>1 || y_error>1)
                        std::cerr<<"Compositor projected ray alignment failed: eye="<<eye<<"; group="<<group
                            <<"; frame="<<mock.frame<<"; x_error="<<x_error<<"; y_error="<<y_error
                            <<"; actual_x="<<sums[eye][group]/counts[eye][group]
                            <<"; expected_x="<<expected_sums[eye][group]/expected_counts[eye][group]
                            <<"; actual_y="<<y_sums[eye][group]/counts[eye][group]
                            <<"; expected_y="<<expected_y[eye][group]/expected_counts[eye][group]<<'\n';
                }
                continue;
            }
            const double disparity = sums[1][group] / counts[1][group] - sums[0][group] / counts[0][group];
            const double x = (group == 0 ? -.6 : .9)-mock.native_camera_offset_m.x;
            const double depth = (group == 0 ? 2. : 6.) +
                (mock.recentered?0:mock.views[0].pose.position.z)-mock.native_camera_offset_m.z;
            double expected_x[2]{};
            for (UINT eye = 0; eye < 2; ++eye) {
                const auto& view = mock.views[eye];
                const double left = std::tan(static_cast<double>(view.fov.angleLeft));
                const double right = std::tan(static_cast<double>(view.fov.angleRight));
                const double eye_x=(view.pose.position.x-(mock.recentered?.001:0))/mock.comfort_world_scale;
                expected_x[eye] = 128. * (2*(x-eye_x)/((right-left)*depth) -
                                          (right+left)/(right-left) + 1);
                if (mock.upright_camera) {
                    const double top=std::tan(double(view.fov.angleUp)),bottom=std::tan(double(view.fov.angleDown));
                    const double world_y=(group==0?-.35:.6)-mock.native_camera_offset_m.y;
                    const double eye_y=mock.comfort_seated?0:(view.pose.position.y-.01)/mock.comfort_world_scale;
                    const double expected_y=128*(1-2*(world_y-eye_y)/((top-bottom)*depth)+(top+bottom)/(top-bottom));
                    const double x_error=std::abs(sums[eye][group]/counts[eye][group]-expected_x[eye]);
                    const double y_error=std::abs(y_sums[eye][group]/counts[eye][group]-expected_y);
                    valid=valid && x_error<=1 && y_error<=1;
                    mock.maximum_disparity_error=std::max({mock.maximum_disparity_error,x_error,y_error});
                    if (x_error>1 || y_error>1)
                        std::cerr<<"Compositor upright camera alignment failed: eye="<<eye
                            <<"; group="<<group<<"; x_error="<<x_error<<"; y_error="<<y_error<<'\n';
                }
            }
            const double expected = expected_x[1] - expected_x[0];
            const double error = std::abs(disparity - expected);
            mock.maximum_disparity_error = std::max(mock.maximum_disparity_error, error);
            valid = valid && error <= 1.;
            if (error>1) std::cerr<<"Compositor disparity failed: group="<<group
                <<"; actual="<<disparity<<"; expected="<<expected<<'\n';
        }
        const unsigned expected_draws=mock.flat_after_world?2u:1u;
        const unsigned expected_instances=mock.expected_instances+(mock.flat_after_world?2u:0u);
        valid = valid && mock.counter && mock.counter->indexed_calls == expected_draws &&
                mock.counter->plain_calls == 0 && mock.counter->instances == expected_instances;
        if (mock.counter) {
            const auto executed=mock.counter->events();
            valid=valid && executed.size()==expected_draws;
            for (unsigned i=0;i<executed.size();++i)
                valid=valid && executed[i].indexed && executed[i].instances==(i?2u:mock.expected_instances);
            if (executed.size()!=expected_draws)
                std::cerr<<"Compositor executed command-list draw count failed: executed="<<executed.size()<<'\n';
        }
        if (mock.counter && (mock.counter->indexed_calls!=expected_draws || mock.counter->plain_calls!=0 ||
            mock.counter->instances!=expected_instances))
            std::cerr<<"Compositor physical draw count failed: indexed="<<mock.counter->indexed_calls
                <<"; plain="<<mock.counter->plain_calls<<"; instances="<<mock.counter->instances<<'\n';
    }
    return valid;
}
XrResult XRAPI_CALL end_frame(XrSession, const XrFrameEndInfo* info) {
    mock.expect(mock.begun && !mock.acquired && info->displayTime == mock.frame * 11111111LL);
    mock.begun = false;
    if (!info->layerCount) {
        mock.expect(mock.frame % 5 == 0 || mock.invalid_pose ||
            (mock.tracking_resume && (mock.frame==2 || mock.frame==3 || mock.frame==7 || mock.frame==8)));
        ++mock.blank;
    } else {
        mock.expect(info->layerCount == 1 && mock.frame % 5 != 0 &&
                    info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        const auto* layer = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
        mock.expect(layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && layer->viewCount == 2 &&
                    layer->space == mock.space);
        for (UINT eye = 0; eye < 2; ++eye) {
            const auto& view = layer->views[eye];
            mock.expect(view.subImage.swapchain == mock.swap && view.subImage.imageArrayIndex == eye &&
                        view.subImage.imageRect.extent.width == int(mock.extent) &&
                        view.subImage.imageRect.extent.height == int(mock.height()) &&
                        view.subImage.imageRect.offset.x == 0 && view.subImage.imageRect.offset.y == 0 &&
                        std::memcmp(&view.pose, &mock.views[eye].pose, sizeof(XrPosef)) == 0 &&
                        std::memcmp(&view.fov, &mock.views[eye].fov, sizeof(XrFovf)) == 0);
        }
        if (!inspect_gpu_eyes()) {
            std::cout << (mock.geometry ? "REJECTED: compositor stereo content or single-draw contract failed\n"
                                       : "REJECTED: compositor received swapped/stale/wrong eye pixels\n");
            return XR_ERROR_VALIDATION_FAILURE;
        }
        ++mock.submitted;
    }
    return mock.failed ? XR_ERROR_VALIDATION_FAILURE : XR_SUCCESS;
}

template<class T> void export_function(PFN_xrVoidFunction* output, T function) {
    static_assert(sizeof(*output) == sizeof(function));
    std::memcpy(output, &function, sizeof(function));
}
XrResult XRAPI_CALL motion_sync(XrSession session,const XrActionsSyncInfo* info) {
    motion_mock::fake.fixture_head={
        (mock.views[0].pose.position.x+mock.views[1].pose.position.x)*.5f,
        (mock.views[0].pose.position.y+mock.views[1].pose.position.y)*.5f,
        (mock.views[0].pose.position.z+mock.views[1].pose.position.z)*.5f};
    if (!mock.native_probe) {
        motion_mock::fake.focused=!mock.motion_focus_test || mock.frame%3!=0 || mock.motion_focus_fault;
        motion_mock::fake.lost_left=mock.motion_focus_test && mock.frame%4==0;
    }
    return motion_mock::sync(session,info);
}
XrResult XRAPI_CALL resolve(XrInstance, const char* name, PFN_xrVoidFunction* output) {
#define ENTRY(symbol, implementation) if (std::strcmp(name, "xr" #symbol) == 0) { export_function(output, &implementation); return XR_SUCCESS; }
    ENTRY(EnumerateInstanceExtensionProperties, extensions)
    ENTRY(CreateInstance, create_instance) ENTRY(DestroyInstance, destroy_instance)
    ENTRY(GetSystem, system) ENTRY(GetD3D12GraphicsRequirementsKHR, requirements)
    ENTRY(EnumerateViewConfigurationViews, configuration) ENTRY(EnumerateEnvironmentBlendModes, modes)
    ENTRY(CreateSession, create_session) ENTRY(DestroySession, destroy_session)
    ENTRY(BeginSession, begin_session) ENTRY(EndSession, end_session) ENTRY(PollEvent, poll)
    ENTRY(EnumerateReferenceSpaces, reference_spaces)
    ENTRY(CreateReferenceSpace, create_space) ENTRY(DestroySpace, destroy_space)
    ENTRY(EnumerateSwapchainFormats, formats) ENTRY(CreateSwapchain, create_swap)
    ENTRY(DestroySwapchain, destroy_swap) ENTRY(EnumerateSwapchainImages, images)
    ENTRY(WaitFrame, wait_frame) ENTRY(BeginFrame, begin_frame) ENTRY(EndFrame, end_frame)
    ENTRY(LocateViews, locate) ENTRY(AcquireSwapchainImage, acquire)
    ENTRY(WaitSwapchainImage, wait_image) ENTRY(ReleaseSwapchainImage, release)
    ENTRY(SyncActions,motion_sync)
#undef ENTRY
    *output = nullptr;
    return motion_mock::resolve(mock.instance,name,output);
}
} // namespace

#ifndef OLVR_OPENXR_MOCK_ONLY
int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        const bool geometry_mono = argc == 5 && mode == "--geometry-fault-mono";
        const bool geometry_dual = argc == 5 && mode == "--geometry-fault-dual";
        const bool runtime_pair = argc == 5 && mode == "--geometry-runtime-pair";
        const bool geometry_mode = argc == 5 && (mode == "--geometry" || geometry_mono || geometry_dual || runtime_pair);
        const bool swapped = argc == 2 && std::string(argv[1]) == "--fault-swapped";
        const bool invalid = argc == 2 && std::string(argv[1]) == "--invalid-pose";
        const bool serial_fault=argc==2 && mode=="--fault-submit-wait";
        const bool queued=argc==2 && (mode=="--queued-release" || serial_fault);
        const bool motion_mode=argc==2 && (mode=="--motion-focus" || mode=="--fault-motion-time" ||
            mode=="--fault-motion-focus");
        const bool reference_mode=argc==2 && (mode=="--reference-stage" || mode=="--reference-local");
        const bool restart_test=argc==2 && mode=="--session-restart";
        mock.tracking_resume=argc==2 && mode=="--tracking-resume";
        mock.asymmetric_extents=argc==2 && mode=="--asymmetric-extents";
        if (mock.asymmetric_extents) mock.extent_height=240;
        const std::map<std::string,std::string> startup_errors{
            {"extension","Runtime does not support XR_KHR_D3D12_enable"},
            {"adapter","Select runtime-required GPU adapter"},
            {"view-count","OpenXR requires exactly two primary stereo views"},
            {"limits","Runtime provided invalid stereo view limits"},
            {"format","Runtime lacks the required display-encoded BGRA8 sRGB stereo copy format"},
            {"swapchain","xrCreateSwapchain"},
            {"images","Runtime returned a null swapchain texture"}};
        if (argc==2 && mode.rfind("--reject-",0)==0 && startup_errors.count(mode.substr(9)))
            mock.startup_error=mode.substr(9);
        mock.reference_test=reference_mode;mock.stage_supported=mode=="--reference-stage";
        mock.motion_focus_test=motion_mode;
        mock.motion_focus_fault=mode=="--fault-motion-focus";
        motion_mock::fake.stale_fault=mode=="--fault-motion-time";
        if (!geometry_mode && (argc > 2 || (argc == 2 && !swapped && !invalid && !queued &&
                                           !motion_mode && !reference_mode && !restart_test &&
                                           !mock.tracking_resume && !mock.asymmetric_extents &&
                                           mock.startup_error.empty())))
            throw std::invalid_argument("Usage: test_openxr_session.exe [--fault-swapped|--invalid-pose|--queued-release|--fault-submit-wait|--geometry[|-fault-mono|-fault-dual|-runtime-pair] fixtures sampler guard]");
        mock.invalid_pose = invalid;
        mock.fault_serial_wait=serial_fault;
        mock.geometry = geometry_mode;
        {
            D3D9On12Factory inventory;
            mock.luid = inventory.device()->GetAdapterLuid();
        }
        if (!mock.startup_error.empty()) {
            bool rejected=false;
            try {OpenXRStereoSession xr(&resolve);}
            catch (const std::runtime_error& error) {
                if (std::string(error.what()).find(startup_errors.at(mock.startup_error))==std::string::npos)
                    throw;
                rejected=true;
            }
            const UINT handles=UINT(mock.created_instance)+UINT(mock.created_session)+
                UINT(mock.created_space)+UINT(mock.created_swap);
            if (!rejected || mock.failed || mock.destroyed!=handles ||
                mock.begun || mock.acquired || mock.frame ||
                motion_mock::fake.destroyed_spaces!=(mock.created_space?4u:0u) ||
                motion_mock::fake.destroyed_sets!=(mock.created_space?1u:0u))
                throw std::runtime_error("Startup failure lost its diagnostic or leaked XR handles");
            std::cout<<"PASS: startup_rejection="<<mock.startup_error<<"; cleanup_handles="<<handles
                <<"; no_frame_or_image_acquired; original_error_preserved\n";
            return 0;
        }
        {
            OpenXRStereoSession xr(&resolve);
            if (xr.width()!=mock.extent || xr.height()!=mock.height())
                throw std::runtime_error("Runtime eye extent selection disagrees with supported array dimensions");
            if (xr.floor_known()!=mock.stage_supported)
                throw std::runtime_error("Enumerated XR floor capability was not preserved");
            UINT reference_changes=0;
            HiddenWindow window(xr.width()*2, xr.height());
            D3DPRESENT_PARAMETERS presentation{};
            presentation.Windowed = TRUE;
            presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
            presentation.BackBufferWidth = xr.width()*2;
            presentation.BackBufferHeight = xr.height();
            presentation.BackBufferFormat = D3DFMT_A8R8G8B8;
            presentation.hDeviceWindow = window.get();
            Com<IDirect3DDevice9> legacy;
            gpu_check(xr.graphics().get()->CreateDevice(xr.graphics().adapter_ordinal(), D3DDEVTYPE_HAL, window.get(),
                D3DCREATE_HARDWARE_VERTEXPROCESSING, &presentation, legacy.put()), "Create mock-connected renderer");
            Com<IDirect3DTexture9> atlas;
            Com<IDirect3DSurface9> surface;
            gpu_check(legacy->CreateTexture(xr.width()*2, xr.height(), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                            D3DPOOL_DEFAULT, atlas.put(), nullptr), "Create submitted scene atlas");
            gpu_check(atlas->GetSurfaceLevel(0, surface.put()), "Get submitted atlas surface");
            gpu_check(legacy->SetRenderTarget(0, surface.get()), "Bind submitted atlas surface");
            struct Vertex { float x, y, z, red, green, blue; };
            std::vector<Vertex> geometry;
            std::unique_ptr<D3D9StereoPipeline> fixture_pipeline;
            D3D9ShaderVariants variants(*legacy.get());
            D3D9StereoPipeline* pipeline = nullptr;
            std::unique_ptr<IsolatedD3D12DrawCounter> counter;
            Com<IDirect3DVertexShader9> vertex_shader;
            Com<IDirect3DPixelShader9> pixel_shader;
            Com<IDirect3DVertexDeclaration9> declaration;
            if (geometry_mode) {
                const auto directory = std::filesystem::path(argv[2]);
                const auto vertex_code = read_shader(directory / "vertex-fixture.bin");
                const auto pixel_code = read_shader(directory / "pixel-fixture.bin");
                const UINT sampler = static_cast<UINT>(std::stoul(argv[3]));
                const UINT guard = static_cast<UINT>(std::stoul(argv[4]));
                gpu_check(legacy->CreateVertexShader(vertex_code.data(), vertex_shader.put()), "Create submitted stereo vertex program");
                gpu_check(legacy->CreatePixelShader(pixel_code.data(), pixel_shader.put()), "Create submitted stereo pixel program");
                const D3DVERTEXELEMENT9 elements[]{
                    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                    D3DDECL_END()
                };
                gpu_check(legacy->CreateVertexDeclaration(elements, declaration.put()), "Create submitted geometry declaration");
                gpu_check(legacy->SetVertexDeclaration(declaration.get()), "Bind submitted geometry declaration");
                gpu_check(legacy->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "Set submitted geometry culling");
                gpu_check(legacy->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "Set submitted geometry depth");
                const float corners[6][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
                for (UINT group = 0; group < 2; ++group) {
                    const float z = group == 0 ? 2.f : 6.f;
                    const float x = group == 0 ? -.6f : .9f;
                    const float y = group == 0 ? -.35f : .6f;
                    for (const auto& corner : corners)
                        geometry.push_back({x + corner[0] * z * .1f, y + corner[1] * z * .1f, z,
                                            group == 0 ? 1.f : 0.f, group == 1 ? 1.f : 0.f, 0.f});
                }
                if (runtime_pair) {
                    gpu_check(legacy->SetVertexShader(vertex_shader.get()), "Bind original submitted vertex program");
                    gpu_check(legacy->SetPixelShader(pixel_shader.get()), "Bind original submitted pixel program");
                    pipeline = &variants.current();
                } else {
                    fixture_pipeline = std::make_unique<D3D9StereoPipeline>(*legacy.get(),
                        StereoShaderBinding{vertex_shader.get(), pixel_shader.get(), 15, sampler, guard});
                    pipeline = fixture_pipeline.get();
                }
                counter = std::make_unique<IsolatedD3D12DrawCounter>(*xr.graphics().device(),true,xr.graphics().queue());
                mock.counter = counter.get();
            }
            for (UINT frame = 0; frame < (restart_test?24u:20u); ++frame) {
                if (restart_test) {
                    if (frame==3 || frame==11) mock.stopping=true;
                    if (frame==4 || frame==12) mock.queued_state=XR_SESSION_STATE_IDLE;
                    if (frame==5 || frame==13) {
                        mock.stopping=mock.stopped=mock.ready=false;
                    }
                    motion_mock::fake.neutral_controls=true;
                    motion_mock::fake.menu_button=false;
                    motion_mock::fake.fixture_reload=motion_mock::fake.fixture_crouch=
                        (frame>=2 && frame<=5) || (frame>=10 && frame<=14);
                }
                bool renderable = false;
                try {
                    renderable = xr.begin_frame();
                } catch (const std::runtime_error& error) {
                    if (!invalid || std::string(error.what()) != "OpenXR did not provide two valid stereo poses and FOVs")
                        throw;
                    continue;
                }
                if (!renderable) {
                    if (xr.motion_frame().focused || xr.motion_frame().hands[0].grip_tracked ||
                        xr.motion_frame().turn.x!=0 ||
                        ((mock.tracking_resume || restart_test) && motion_mock::fake.vibrating[1]))
                        throw std::runtime_error("Skipped XR frame retained motion input");
                    continue;
                }
                if (restart_test && xr.motion_frame().menu!=(frame==2 || frame==10))
                    throw std::runtime_error("Restarted session replayed a held pause chord");
                const auto changes=xr.take_reference_changes();
                const std::size_t expected_changes=!reference_mode?0:
                    mock.frame==3 || mock.frame==6?1:mock.frame==7?2:0;
                if (changes.size()!=expected_changes)
                    throw std::runtime_error("XR origin change applied early, lost during skip, duplicated or accepted foreign event");
                for (const auto& change:changes) {
                    if (change.changeTime>xr.display_time() || change.session!=mock.session ||
                        (change.poseValid && ((mock.frame==6 && change.poseInPreviousSpace.position.y!=.2f) ||
                         (mock.frame==7 && change.poseInPreviousSpace.position.y!=-.1f))))
                        throw std::runtime_error("XR origin change lost effective-time or pose metadata");
                    ++reference_changes;
                }
                const auto& motion=xr.motion_frame();
                const bool focused=!mock.motion_focus_test || mock.frame%3!=0;
                const float expected_grip_z=restart_test?
                    (mock.views[0].pose.position.z+mock.views[1].pose.position.z)*.5f-.4f:
                    float(xr.display_time())/10000;
                if (motion.focused!=focused || motion.time!=xr.display_time() ||
                    (!focused && (motion.hands[1].grip_tracked || motion.turn.x!=0)) ||
                    (focused && (motion.hands[1].grip.position.z!=expected_grip_z ||
                        motion.hands[0].grip_tracked==(mock.motion_focus_test && mock.frame%4==0))))
                    throw std::runtime_error("Attached session motion predicted-time/focus/tracking oracle failed");
                const auto& views = xr.views();
                if (std::abs(views[1].pose.position.x - views[0].pose.position.x - mock.ipd()) > .000001f)
                    throw std::runtime_error("Located eye separation was corrupted");
                if (geometry_mode) {
                    if (runtime_pair && (&variants.current() != pipeline || variants.size() != 1))
                        throw std::runtime_error("Submitted runtime shader pair lost its original identity or cache entry");
                    counter->reset();
                    StereoViewConstants constants{};
                    for (UINT eye = 0; eye < 2; ++eye) {
                        const auto& view = views[eye];
                        const TrackingFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,-1},1,{0,0,0}};
                        const float camera_x = geometry_mono ? (views[0].pose.position.x + views[1].pose.position.x)*.5f
                                                            : view.pose.position.x;
                        constants[eye] = eye_constants(frame,
                            {{camera_x, view.pose.position.y, view.pose.position.z},
                             {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w}},
                            {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown}, .01f, 100.f);
                    }
                    gpu_check(legacy->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0), "Clear submitted stereo geometry");
                    gpu_check(legacy->BeginScene(), "Begin single submitted scene");
                    const auto result = pipeline->draw_primitive_up(D3DPT_TRIANGLELIST, 4, geometry.data(),
                        geometry.size() * sizeof(Vertex), sizeof(Vertex), constants);
                    if (!result.succeeded()) throw std::runtime_error("Single-draw submitted geometry failed");
                    if (geometry_dual) {
                        const auto replay = pipeline->draw_primitive_up(D3DPT_TRIANGLELIST, 4, geometry.data(),
                            geometry.size() * sizeof(Vertex), sizeof(Vertex), constants);
                        if (!replay.succeeded()) throw std::runtime_error("Dual-draw negative-control rendering failed");
                    }
                    gpu_check(legacy->EndScene(), "End single submitted scene");
                } else for (UINT eye = 0; eye < 2; ++eye) {
                    D3DRECT rectangle{static_cast<LONG>(eye * xr.width()), 0,
                                      static_cast<LONG>((eye + 1) * xr.width()), LONG(xr.height())};
                    gpu_check(legacy->Clear(1, &rectangle, D3DCLEAR_TARGET,
                                            mock.color(swapped ? 1 - eye : eye), 1, 0),
                              "Render synthetic compositor eye marker");
                }
                if (mock.tracking_resume || restart_test) xr.vibrate(1,.5f,1000000000);
                if (queued && !mock.submitted) {
                    Com<ID3D12Fence> gate;
                    gpu_check(mock.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),
                        reinterpret_cast<void**>(gate.put())),"Create controlled GPU queue gate");
                    gpu_check(mock.queue->Wait(gate.get(),1),"Block GPU before actual XR copy");
                    gate_opened.store(false,std::memory_order_release);
                    release_observed.store(false,std::memory_order_release);
                    mock.gated_release_test=true;
                    auto opener=std::async(std::launch::async,[fence=gate.get()] {
                        const auto deadline=GetTickCount64()+2000;
                        while (!release_observed.load(std::memory_order_acquire) && GetTickCount64()<deadline)
                            Sleep(1);
                        const bool released=release_observed.load(std::memory_order_acquire);
                        gate_opened.store(true,std::memory_order_release);
                        gpu_check(fence->Signal(1),"Open controlled GPU queue gate");
                        return released;
                    });
                    xr.submit(*legacy.get(),*atlas.get());
                    mock.gated_release_test=false;
                    if (!opener.get() || mock.gated_releases!=1)
                        throw std::runtime_error("Submission waited for GPU completion before releasing the OpenXR image");
                } else xr.submit(*legacy.get(), *atlas.get());
            }
            mock.stopping = true;
            if (xr.begin_frame()) throw std::runtime_error("STOPPING session started another frame");
            if (xr.motion_frame().focused)
                throw std::runtime_error("Stopped session retained motion input");
            const UINT expected_submitted = invalid ? 0 : mock.tracking_resume?12:16;
            const UINT expected_blank = invalid ? 20 : mock.tracking_resume?8:4;
            const UINT expected_pixels = expected_submitted*2*mock.extent*mock.height();
            if (mock.frame != 20 || mock.submitted != expected_submitted || mock.blank != expected_blank ||
                mock.pixels != expected_pixels || (invalid && mock.invalid_poses != 16) ||
                !mock.stopped || mock.failed)
                throw std::runtime_error("OpenXR proof was truncated or violated runtime contracts");
            if (mock.tracking_resume && mock.tracking_gaps!=4)
                throw std::runtime_error("Tracking loss/reacquisition coverage was incomplete");
            if (restart_test && (mock.session_begins!=3 || mock.session_ends!=3 ||
                motion_mock::fake.hand_haptics[1]!=16 || motion_mock::fake.vibrating[1]))
                throw std::runtime_error("Repeated session restart or haptic recovery coverage was incomplete");
            if (runtime_pair)
                std::cout << "PASS: native runtime pair reused across 16 independently inspected OpenXR projections\n";
            if (reference_mode) {
                if (reference_changes!=4)
                    throw std::runtime_error("XR reference-change coverage incomplete");
                std::cout<<"PASS: reference_changes=4; future_events_deferred; skipped_frame_retention; "
                    <<"foreign_events_ignored; floor_space="<<(xr.floor_known()?"STAGE":"LOCAL")<<'\n';
            }
        }
        if (mock.destroyed != 4 || swapped)
            throw std::runtime_error("OpenXR cleanup or swapped-eye rejection failed");
        if (motion_mock::fake.destroyed_spaces!=4 || motion_mock::fake.destroyed_sets!=1 ||
            motion_mock::fake.attached!=1)
            throw std::runtime_error("Attached session motion cleanup incomplete");
        std::cout<<"PASS: session_attached_motion; same_predicted_time_and_space; focus_skip_stop_neutral; "
            <<"lost_hand_tracking; action_spaces_destroyed_before_session\n";
        if (invalid) {
            std::cout << "PASS: invalid located poses rejected=16; frames_ended_empty=20; no images acquired; cleanup_handles=4\n";
            return 0;
        }
        if (geometry_mode) {
            std::cout << "PASS: single-draw stereo to OpenXR GPU images; projections=16; physical_draws_per_frame=1; "
                         "eye_instances=2; near_far_disparity_checks=32; max_error_px=" << mock.maximum_disparity_error
                      << "; located_pose_translation_applied; exact_subresource_order_verified\n";
            return 0;
        }
        if (queued)
            std::cout<<"PASS: GPU-blocked OpenXR image released before queue completion; gated_releases=1; "
                "ordered_compositor_pixels=2097152; allocator_reuse_and_cleanup_preserved\n";
        if (mock.tracking_resume)
            std::cout<<"PASS: tracking_gaps=4; recovered_stereo; invalid_unavailable_data_ignored; "
                "motion_and_haptics_neutral; valid_estimated_pose_accepted\n";
        if (mock.asymmetric_extents)
            std::cout<<"PASS: asymmetric_recommendations_and_maxima; common_eye_extent=256x240\n";
        if (restart_test)
            std::cout<<"PASS: stopping_idle_ready_cycles=2; held_pause_not_replayed; "
                "stereo_and_haptics_recovered; action_set_attached_once\n";
        std::cout << "PASS: real-GPU OpenXR mock; frames=20; projections="<<mock.submitted
            <<"; empty_frames="<<mock.blank<<"; exact_pixels="<<mock.pixels
            <<"; tracked_poses_and_fovs="<<mock.submitted*2<<"; cleanup_handles=4\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
#endif
