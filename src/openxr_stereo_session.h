#pragma once
#include "d3d9_on12_transfer.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "openxr_function.h"
#include "openxr_motion_input.h"
#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace olvr {

class OpenXRStereoSession {
    PFN_xrGetInstanceProcAddr resolve_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrReferenceSpaceType reference_type_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    std::vector<XrEventDataReferenceSpaceChangePending> pending_reference_changes_, reference_changes_;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrEnvironmentBlendMode blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool running_ = false, frame_open_ = false, renderable_ = false, poisoned_ = false;
    XrFrameState frame_{};
    XrViewStateFlags view_state_flags_=0;
    std::array<XrView, 2> views_{};
    std::vector<XrSwapchainImageD3D12KHR> images_;
    UINT width_ = 0, height_ = 0;
    std::unique_ptr<D3D9On12Factory> graphics_;
    std::unique_ptr<D3D9On12StereoTransfer> transfer_;
    std::unique_ptr<OpenXRMotionInput> motion_;
    MotionFrame motion_frame_{};

#define OLVR_XR_FUNCTIONS(F) \
    F(DestroyInstance) F(GetSystem) F(GetD3D12GraphicsRequirementsKHR) \
    F(EnumerateViewConfigurationViews) F(EnumerateEnvironmentBlendModes) \
    F(CreateSession) F(DestroySession) F(BeginSession) F(EndSession) F(PollEvent) \
    F(EnumerateReferenceSpaces) F(CreateReferenceSpace) F(DestroySpace) F(EnumerateSwapchainFormats) \
    F(CreateSwapchain) F(DestroySwapchain) F(EnumerateSwapchainImages) \
    F(WaitFrame) F(BeginFrame) F(EndFrame) F(LocateViews) \
    F(AcquireSwapchainImage) F(WaitSwapchainImage) F(ReleaseSwapchainImage)
#define OLVR_DECLARE_XR(name) PFN_xr##name name##_ = nullptr;
    OLVR_XR_FUNCTIONS(OLVR_DECLARE_XR)
#undef OLVR_DECLARE_XR

    template<class T> void load(const char* name, T& function, XrInstance instance) {
        xr_load_function(resolve_,instance,name,function);
    }
    void finish(const XrCompositionLayerBaseHeader* layer = nullptr) {
        if (!frame_open_)
            throw std::logic_error("OpenXR frame is not open");
        XrFrameEndInfo end{};
        end.type = XR_TYPE_FRAME_END_INFO;
        end.displayTime = frame_.predictedDisplayTime;
        end.environmentBlendMode = blend_;
        end.layerCount = layer ? 1 : 0;
        end.layers = layer ? &layer : nullptr;
        const auto result = EndFrame_(session_, &end);
        frame_open_ = false;
        renderable_ = false;
        if (XR_FAILED(result))
            poisoned_ = true;
        xr_check(result, "xrEndFrame");
    }
    void cleanup() noexcept {
        transfer_.reset();
        motion_frame_={};
        motion_.reset();
        auto destroy = [](XrResult result, const char* operation) {
            if (XR_FAILED(result)) {
                const auto message = std::string(operation) + " failed during OpenXR cleanup: " +
                                     std::to_string(result);
                OutputDebugStringA(message.c_str());
            }
        };
        if (frame_open_ && EndFrame_) {
            try { finish(); }
            catch (const std::exception& error) { OutputDebugStringA(error.what()); }
        }
        if (swapchain_ && DestroySwapchain_) destroy(DestroySwapchain_(swapchain_), "xrDestroySwapchain");
        if (space_ && DestroySpace_) destroy(DestroySpace_(space_), "xrDestroySpace");
        if (session_ && DestroySession_) destroy(DestroySession_(session_), "xrDestroySession");
        graphics_.reset();
        if (instance_ && DestroyInstance_) destroy(DestroyInstance_(instance_), "xrDestroyInstance");
    }
    static bool valid_view(const XrView& view) {
        const auto& p = view.pose.position;
        const auto& q = view.pose.orientation;
        const auto& f = view.fov;
        const double norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        constexpr float half_pi = 1.57079632679f;
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
            std::isfinite(norm) && std::abs(norm - 1.) < .01 &&
            std::isfinite(f.angleLeft) && std::isfinite(f.angleRight) &&
            std::isfinite(f.angleUp) && std::isfinite(f.angleDown) &&
            f.angleLeft > -half_pi && f.angleRight < half_pi &&
            f.angleDown > -half_pi && f.angleUp < half_pi &&
            f.angleLeft < f.angleRight && f.angleDown < f.angleUp;
    }
public:
    explicit OpenXRStereoSession(PFN_xrGetInstanceProcAddr resolve,
                                const char* required_runtime = nullptr,
                                bool motion_controls = true) : resolve_(resolve) {
        if (!resolve_)
            throw std::invalid_argument("A real OpenXR loader entry is required");
        try {
            PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
            PFN_xrCreateInstance create = nullptr;
            load("xrEnumerateInstanceExtensionProperties", enumerate, XR_NULL_HANDLE);
            load("xrCreateInstance", create, XR_NULL_HANDLE);
            uint32_t count = 0;
            xr_check(enumerate(nullptr, 0, &count, nullptr), "Enumerate runtime extensions");
            if (count == 0 || count > 4096)
                throw std::runtime_error("Runtime extension inventory is empty or unreasonable");
            std::vector<XrExtensionProperties> extensions(count);
            for (auto& extension : extensions) extension.type = XR_TYPE_EXTENSION_PROPERTIES;
            xr_check(enumerate(nullptr, count, &count, extensions.data()), "Read runtime extensions");
            bool supported = false;
            for (uint32_t i = 0; i < count; ++i)
                supported |= std::strcmp(extensions[i].extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
            if (!supported)
                throw std::runtime_error("Runtime does not support XR_KHR_D3D12_enable");
            const char* enabled[]{XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
            XrInstanceCreateInfo info{};
            info.type = XR_TYPE_INSTANCE_CREATE_INFO;
            std::strcpy(info.applicationInfo.applicationName, "Outlast single-pass stereo");
            info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
            info.enabledExtensionCount = 1;
            info.enabledExtensionNames = enabled;
            xr_check(create(&info, &instance_), "xrCreateInstance");
            if (required_runtime) {
                PFN_xrGetInstanceProperties properties = nullptr;
                load("xrGetInstanceProperties", properties, instance_);
                XrInstanceProperties identity{};
                identity.type = XR_TYPE_INSTANCE_PROPERTIES;
                xr_check(properties(instance_, &identity), "Verify headless runtime identity");
                if (std::strcmp(identity.runtimeName, required_runtime) != 0)
                    throw std::runtime_error("Headset-free verification refuses a different OpenXR runtime");
            }
#define OLVR_LOAD_XR(name) load("xr" #name, name##_, instance_);
            OLVR_XR_FUNCTIONS(OLVR_LOAD_XR)
#undef OLVR_LOAD_XR
            XrSystemGetInfo system_info{};
            system_info.type = XR_TYPE_SYSTEM_GET_INFO;
            system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            xr_check(GetSystem_(instance_, &system_info, &system_), "xrGetSystem");
            XrGraphicsRequirementsD3D12KHR requirements{};
            requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
            xr_check(GetD3D12GraphicsRequirementsKHR_(instance_, system_, &requirements),
                     "xrGetD3D12GraphicsRequirementsKHR");
            graphics_ = std::make_unique<D3D9On12Factory>(&requirements.adapterLuid,
                                                        requirements.minFeatureLevel);
            count = 0;
            xr_check(EnumerateViewConfigurationViews_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       0, &count, nullptr), "Count stereo views");
            if (count != 2)
                throw std::runtime_error("OpenXR requires exactly two primary stereo views");
            std::array<XrViewConfigurationView, 2> configuration{};
            for (auto& view : configuration) view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
            xr_check(EnumerateViewConfigurationViews_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       2, &count, configuration.data()), "Read stereo extent");
            if (count != 2)
                throw std::runtime_error("OpenXR view count changed");
            width_ = std::max(configuration[0].recommendedImageRectWidth, configuration[1].recommendedImageRectWidth);
            height_ = std::max(configuration[0].recommendedImageRectHeight, configuration[1].recommendedImageRectHeight);
            if (!width_ || !height_ || width_ > 8192 || height_ > 8192 ||
                width_ > configuration[0].maxImageRectWidth || width_ > configuration[1].maxImageRectWidth ||
                height_ > configuration[0].maxImageRectHeight || height_ > configuration[1].maxImageRectHeight)
                throw std::runtime_error("Runtime recommends incompatible stereo dimensions");
            count = 0;
            xr_check(EnumerateEnvironmentBlendModes_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      0, &count, nullptr), "Count environment blend modes");
            if (!count || count > 16) throw std::runtime_error("Invalid environment blend inventory");
            std::vector<XrEnvironmentBlendMode> modes(count);
            xr_check(EnumerateEnvironmentBlendModes_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      count, &count, modes.data()), "Read environment blend modes");
            if (std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end())
                throw std::runtime_error("Outlast VR requires opaque headset composition");
            XrGraphicsBindingD3D12KHR binding{};
            binding.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
            binding.device = graphics_->device();
            binding.queue = graphics_->queue();
            XrSessionCreateInfo session_info{};
            session_info.type = XR_TYPE_SESSION_CREATE_INFO;
            session_info.next = &binding;
            session_info.systemId = system_;
            xr_check(CreateSession_(instance_, &session_info, &session_), "xrCreateSession");
            count = 0;
            xr_check(EnumerateReferenceSpaces_(session_, 0, &count, nullptr), "Count reference spaces");
            if (!count || count > 64) throw std::runtime_error("Invalid reference-space inventory");
            std::vector<XrReferenceSpaceType> spaces(count);
            xr_check(EnumerateReferenceSpaces_(session_, count, &count, spaces.data()), "Read reference spaces");
            if (count != spaces.size() ||
                std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_LOCAL) == spaces.end())
                throw std::runtime_error("Runtime reference-space inventory changed or lacks LOCAL");
            if (std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end())
                reference_type_ = XR_REFERENCE_SPACE_TYPE_STAGE;
            XrReferenceSpaceCreateInfo reference{};
            reference.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
            reference.referenceSpaceType = reference_type_;
            reference.poseInReferenceSpace.orientation.w = 1;
            xr_check(CreateReferenceSpace_(session_, &reference, &space_), "xrCreateReferenceSpace");
            if (motion_controls)
                motion_=std::make_unique<OpenXRMotionInput>(resolve_,instance_,session_);
            count = 0;
            xr_check(EnumerateSwapchainFormats_(session_, 0, &count, nullptr), "Count swapchain formats");
            if (!count || count > 4096) throw std::runtime_error("Invalid swapchain format inventory");
            std::vector<int64_t> formats(count);
            xr_check(EnumerateSwapchainFormats_(session_, count, &count, formats.data()), "Read swapchain formats");
            if (std::find(formats.begin(), formats.end(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == formats.end())
                throw std::runtime_error("Runtime lacks the required display-encoded BGRA8 sRGB stereo copy format");
            XrSwapchainCreateInfo swap{};
            swap.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
            swap.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            // The final native display image is already encoded; UNORM would
            // tell the compositor these bytes are linear and brighten shadows.
            swap.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            swap.sampleCount = 1;
            swap.width = width_;
            swap.height = height_;
            swap.faceCount = 1;
            swap.arraySize = 2;
            swap.mipCount = 1;
            xr_check(CreateSwapchain_(session_, &swap, &swapchain_), "xrCreateSwapchain");
            count = 0;
            xr_check(EnumerateSwapchainImages_(swapchain_, 0, &count, nullptr), "Count stereo images");
            if (!count || count > 64) throw std::runtime_error("Invalid swapchain image inventory");
            images_.resize(count);
            for (auto& image : images_) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            xr_check(EnumerateSwapchainImages_(swapchain_, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())), "Read stereo GPU images");
            if (count != images_.size()) throw std::runtime_error("Swapchain image count changed");
            for (const auto& image : images_)
                if (!image.texture) throw std::runtime_error("Runtime returned a null swapchain texture");
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~OpenXRStereoSession() { cleanup(); }
    OpenXRStereoSession(const OpenXRStereoSession&) = delete;
    OpenXRStereoSession& operator=(const OpenXRStereoSession&) = delete;
    D3D9On12Factory& graphics() const { return *graphics_; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    const std::array<XrView, 2>& views() const {
        if (!frame_open_ || !renderable_) throw std::logic_error("No located renderable stereo frame");
        return views_;
    }
    XrTime display_time() const { return frame_.predictedDisplayTime; }
    bool floor_known() const { return reference_type_ == XR_REFERENCE_SPACE_TYPE_STAGE; }
    XrViewStateFlags view_state_flags() const {
        if (!frame_open_ || !renderable_) throw std::logic_error("Tracking flags require current located views");
        return view_state_flags_;
    }
    XrReferenceSpaceType reference_space_type() const { return reference_type_; }
    std::vector<XrEventDataReferenceSpaceChangePending> take_reference_changes() {
        if (!frame_open_ || !renderable_)
            throw std::logic_error("Reference changes require current valid located views");
        auto changes = std::move(reference_changes_);
        reference_changes_.clear();
        return changes;
    }
    const MotionFrame& motion_frame() const { return motion_frame_; }
    void vibrate(unsigned hand,float amplitude,XrDuration duration) {
        if (!motion_ || (amplitude!=0 && !motion_frame_.focused))
            throw std::logic_error("Controller haptics require focused attached motion actions");
        if (amplitude==0) motion_->stop_vibration(hand);
        else motion_->vibrate(hand,amplitude,duration);
    }

    bool begin_frame() {
        motion_frame_={};
        view_state_flags_=0;
        if (poisoned_) throw std::runtime_error("OpenXR session is poisoned");
        if (frame_open_) throw std::logic_error("Previous OpenXR frame has not ended");
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        for (;;) {
            const auto result = PollEvent_(instance_, &event);
            if (result == XR_EVENT_UNAVAILABLE) break;
            xr_check(result, "xrPollEvent");
            if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                poisoned_ = true;
                throw std::runtime_error("OpenXR instance loss pending");
            }
            if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
                if (change.session == session_ &&
                    (change.referenceSpaceType == reference_type_ ||
                     change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)) {
                    if (pending_reference_changes_.size() + reference_changes_.size() >= 256)
                        throw std::runtime_error("Too many unconsumed OpenXR origin changes");
                    pending_reference_changes_.push_back(change);
                    pending_reference_changes_.back().next = nullptr;
                }
            }
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
                if (state.session == session_) {
                    if (state.state == XR_SESSION_STATE_READY && !running_) {
                        XrSessionBeginInfo begin{};
                        begin.type = XR_TYPE_SESSION_BEGIN_INFO;
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        xr_check(BeginSession_(session_, &begin), "xrBeginSession");
                        running_ = true;
                    } else if (state.state == XR_SESSION_STATE_STOPPING && running_) {
                        if (motion_) motion_->stop_vibrations();
                        xr_check(EndSession_(session_), "xrEndSession");
                        running_ = false;
                    } else if (state.state == XR_SESSION_STATE_EXITING || state.state == XR_SESSION_STATE_LOSS_PENDING) {
                        poisoned_ = true;
                        throw std::runtime_error("OpenXR session exit/loss requested");
                    }
                }
            }
            event = {};
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
        }
        if (!running_) {
            if (motion_) motion_->stop_vibrations();
            return false;
        }
        XrFrameWaitInfo wait{};
        wait.type = XR_TYPE_FRAME_WAIT_INFO;
        frame_ = {};
        frame_.type = XR_TYPE_FRAME_STATE;
        xr_check(WaitFrame_(session_, &wait, &frame_), "xrWaitFrame");
        std::stable_sort(pending_reference_changes_.begin(), pending_reference_changes_.end(),
            [](const auto& a, const auto& b) { return a.changeTime < b.changeTime; });
        const auto future = std::find_if(pending_reference_changes_.begin(), pending_reference_changes_.end(),
            [&](const auto& change) { return change.changeTime > frame_.predictedDisplayTime; });
        reference_changes_.insert(reference_changes_.end(), pending_reference_changes_.begin(), future);
        pending_reference_changes_.erase(pending_reference_changes_.begin(), future);
        XrFrameBeginInfo begin{};
        begin.type = XR_TYPE_FRAME_BEGIN_INFO;
        xr_check(BeginFrame_(session_, &begin), "xrBeginFrame");
        frame_open_ = true;
        if (!frame_.shouldRender) {
            if (motion_) motion_->stop_vibrations();
            finish();
            return false;
        }
        try {
            XrViewLocateInfo locate{};
            locate.type = XR_TYPE_VIEW_LOCATE_INFO;
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = frame_.predictedDisplayTime;
            locate.space = space_;
            XrViewState state{};
            state.type = XR_TYPE_VIEW_STATE;
            for (auto& view : views_) { view = {}; view.type = XR_TYPE_VIEW; }
            uint32_t count = 0;
            xr_check(LocateViews_(session_, &locate, &state, 2, &count, views_.data()), "xrLocateViews");
            const auto required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            if (count != 2 || (state.viewStateFlags & required) != required ||
                !valid_view(views_[0]) || !valid_view(views_[1]))
                throw std::runtime_error("OpenXR did not provide two valid stereo poses and FOVs");
            view_state_flags_=state.viewStateFlags;
            renderable_ = true;
            if (motion_) motion_frame_=motion_->sample(space_,frame_.predictedDisplayTime);
        } catch (...) {
            motion_frame_={};
            const auto failure=std::current_exception();
            try {finish();}
            catch (const std::exception& error) {OutputDebugStringA(error.what());}
            std::rethrow_exception(failure);
        }
        return true;
    }

    void cancel_frame() {
        motion_frame_={};
        if (motion_) motion_->stop_vibrations();
        finish();
    }
    void drain_transfers() { if (transfer_) transfer_->wait(); }

    void submit(IDirect3DDevice9& legacy, IDirect3DTexture9& atlas) {
        if (!frame_open_ || !renderable_ || poisoned_)
            throw std::logic_error("Cannot submit without a valid located stereo frame");
        bool acquired = false, waited = false;
        try {
            D3DSURFACE_DESC description{};
            gpu_check(atlas.GetLevelDesc(0, &description), "Read submitted stereo extent");
            if (description.Width != width_ * 2 || description.Height != height_)
                throw std::invalid_argument("Rendered atlas extent differs from the submitted eye extent");
            if (!transfer_)
                transfer_ = std::make_unique<D3D9On12StereoTransfer>(legacy, *graphics_->device(), *graphics_->queue());
            XrSwapchainImageAcquireInfo acquire{};
            acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            uint32_t index = 0;
            xr_check(AcquireSwapchainImage_(swapchain_, &acquire, &index), "xrAcquireSwapchainImage");
            acquired = true;
            if (index >= images_.size()) throw std::runtime_error("Runtime returned an invalid swapchain image index");
            XrSwapchainImageWaitInfo wait{};
            wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
            wait.timeout = 10000000000LL;
            const auto result = WaitSwapchainImage_(swapchain_, &wait);
            if (result == XR_TIMEOUT_EXPIRED) throw std::runtime_error("OpenXR image wait timed out");
            xr_check(result, "xrWaitSwapchainImage");
            waited = true;
            transfer_->copy(atlas, {{{images_[index].texture, 0}, {images_[index].texture, 1}}});
            // Copy and compositor work share the bound graphics queue. Resource
            // ownership remains fenced until allocator reuse, reset or teardown.
            XrSwapchainImageReleaseInfo release{};
            release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            xr_check(ReleaseSwapchainImage_(swapchain_, &release), "xrReleaseSwapchainImage");
            acquired = false;
            std::array<XrCompositionLayerProjectionView, 2> projection{};
            for (UINT eye = 0; eye < 2; ++eye) {
                projection[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projection[eye].pose = views_[eye].pose;
                projection[eye].fov = views_[eye].fov;
                projection[eye].subImage.swapchain = swapchain_;
                projection[eye].subImage.imageRect.extent = {static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
                projection[eye].subImage.imageArrayIndex = eye;
            }
            XrCompositionLayerProjection layer{};
            layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            layer.space = space_;
            layer.viewCount = 2;
            layer.views = projection.data();
            finish(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
        } catch (...) {
            const auto failure=std::current_exception();
            bool safe_release = true;
            if (transfer_) {
                try { transfer_->wait(); }
                catch (const std::exception& error) {
                    safe_release = false;
                    OutputDebugStringA(error.what());
                }
            }
            if (acquired && waited && safe_release) {
                XrSwapchainImageReleaseInfo release{};
                release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
                const auto result = ReleaseSwapchainImage_(swapchain_, &release);
                if (XR_FAILED(result)) OutputDebugStringA("Failed to release acquired stereo image during error cleanup");
            }
            poisoned_ = true;
            if (frame_open_) {
                try {finish();}
                catch (const std::exception& error) {OutputDebugStringA(error.what());}
            }
            std::rethrow_exception(failure);
        }
    }
};
#undef OLVR_XR_FUNCTIONS

} // namespace olvr
