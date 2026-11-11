#pragma once
#include "outlast_skeleton.h"
#include "d3d9_shader_variants.h"
#include "sha256.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace olvr {

struct NativeCamcorderProbeBindings {
    using Class=void*(WINAPI*)();
    using Construct=void*(WINAPI*)(void*,void*,UINT64,UINT64,void*,void*,void*,void*);
    using Initialize=void(WINAPI*)(void*,UINT,UINT,BYTE,UINT);
    using Attach=void(WINAPI*)(void*,void*);
    using SetView=void(WINAPI*)(void*,const Vec3*,const INT*);
    using Projection=void(WINAPI*)(void*);
    using Activate=void(WINAPI*)(void*,INT);
    using LocalPlayer=void*(WINAPI*)(void*);
    using ShowFlags=UINT64*(WINAPI*)(void*,UINT64*);
    using CloseMovie=void(*)(void*);
    using SetScale=void(WINAPI*)(void*,BYTE);
    using GetScale=UINT(WINAPI*)(void*);
    Class capture_class=nullptr,target_class=nullptr;
    Construct construct=nullptr;
    Initialize initialize=nullptr;
    Attach attach=nullptr;
    Attach detach=nullptr;
    SetView set_view=nullptr;
    Projection projection=nullptr;
    Activate activate_night_vision=nullptr;
    LocalPlayer local_player=nullptr;
    ShowFlags show_flags=nullptr;
    CloseMovie close_movie=nullptr;
    SetScale set_scale=nullptr;
    GetScale get_scale=nullptr;
    void** errors=nullptr;
    void** engine=nullptr;
    bool night_vision_experiment=false;
    bool native_postprocess=false;
};

class NativeCamcorderProbe {
    NativeCamcorderProbeBindings bindings_;
    void* owner_=nullptr;
    void* component_=nullptr;
    void* target_=nullptr;
    void* hud_target_=nullptr;
    void* hud_movie_=nullptr;
    void* original_hud_target_=nullptr;
    BYTE original_hud_mode_=0;
    UINT original_hud_scale_=0;
    bool hud_scale_saved_=false;
    std::atomic<std::uintptr_t> hud_texture_{0};
    std::atomic<std::uintptr_t> hud_ready_texture_{0};
    std::atomic<std::uintptr_t> hud_movie_identity_{0},hud_target_identity_{0};
    std::atomic<bool> hud_configured_{false};
    std::atomic<void*> published_target_{nullptr};
    std::atomic<std::uintptr_t> published_component_{0},published_chain_{0};
    std::atomic<INT> published_proxies_{0};
    std::atomic<std::uintptr_t> published_texture_{0};
    std::atomic<std::uintptr_t> published_render_target_{0};
    std::atomic<bool> image_written_{false};
    std::atomic<UINT> frame_{0};
    ULONGLONG settled_since_=0;
    std::atomic<UINT> settled_ms_{0};
    std::atomic<BYTE> capture_mode_{0};
    std::filesystem::path folder_;
    std::ofstream evidence_;
    std::ofstream views_,samples_,hud_draws_,hud_commands_,hud_readiness_;
    bool sample_waiting_for_hud_=false;
    UINT hud_draw_count_=0;
    UINT hud_command_count_=0;
    unsigned sampled_=0;
    void* night_vision_chain_=nullptr;
    void* configured_chain_=nullptr;
    struct TextureAssociation {
        Com<IDirect3DTexture9> image;
        bool published=false;
    };
    std::unordered_map<std::uintptr_t,TextureAssociation> textures_;
    template<class T> static void write(void* object,std::size_t offset,T value) {
        std::memcpy(static_cast<unsigned char*>(object)+offset,&value,sizeof(value));
    }
public:
    static bool gameplay_effect_visible(UINT flags) {return (flags&2)!=0;}
    bool owns_component(void* component) const {return component && component_==component;}
    bool owns_hud_record(void* movie,void* target) const {
        return movie && target &&
            reinterpret_cast<std::uintptr_t>(movie)==hud_movie_identity_.load(std::memory_order_acquire) &&
            reinterpret_cast<std::uintptr_t>(target)==hud_target_identity_.load(std::memory_order_acquire);
    }
    void release_hud() {
        hud_movie_identity_.store(0,std::memory_order_release);
        hud_target_identity_.store(0,std::memory_order_release);
        if (hud_movie_) {
            bindings_.close_movie(hud_movie_);
            if (hud_scale_saved_) bindings_.set_scale(hud_movie_,BYTE(original_hud_scale_));
            write(hud_movie_,0x118,original_hud_target_);
            write(hud_movie_,0x16d,original_hud_mode_);
        }
        hud_target_=hud_movie_=original_hud_target_=nullptr;
        hud_scale_saved_=false;
        hud_texture_.store(0,std::memory_order_release);
        hud_ready_texture_.store(0,std::memory_order_release);
        hud_configured_.store(false,std::memory_order_release);
    }
    void bind_hud(void* movie) {
        if (!movie || !bindings_.close_movie || !bindings_.set_scale || !bindings_.get_scale)
            throw std::invalid_argument("Native camera HUD requires the admitted movie and close binding");
        if (hud_movie_==movie) return;
        release_hud();
        if (!*bindings_.errors)
            throw std::runtime_error("Native HUD render-target factory is not initialized");
        hud_target_=bindings_.construct(bindings_.target_class(),movie,0,0x400000000000,
            nullptr,*bindings_.errors,nullptr,nullptr);
        if (!hud_target_) throw std::runtime_error("Native camera HUD render-target construction failed");
        write(hud_target_,0x140,std::array<float,4>{{0,0,0,0}});
        bindings_.initialize(hud_target_,512,288,2,0);
        original_hud_target_=skeleton_field<void*>(movie,0x118);
        original_hud_mode_=skeleton_field<BYTE>(movie,0x16d);
        if (skeleton_field<void*>(movie,0x60)) bindings_.close_movie(movie);
        hud_movie_=movie;
        write(movie,0x118,hud_target_);
        write(movie,0x16d,BYTE(1));
        hud_target_identity_.store(reinterpret_cast<std::uintptr_t>(hud_target_),std::memory_order_release);
        hud_movie_identity_.store(reinterpret_cast<std::uintptr_t>(movie),std::memory_order_release);
        hud_configured_.store(true,std::memory_order_release);
    }
    void refresh_hud() {
        if (!hud_movie_) return;
        auto* native=skeleton_field<void*>(hud_movie_,0x60);
        if (!native || skeleton_field<void*>(native,0x70)!=hud_movie_ ||
            skeleton_field<void*>(native,0x78)!=hud_target_ ||
            skeleton_field<void*>(hud_movie_,0x118)!=hud_target_ ||
            skeleton_field<BYTE>(hud_movie_,0x16d)!=1)
            throw std::runtime_error("Actual stock GFx camera movie did not bind its independent alpha render target");
        auto* view=skeleton_field<void*>(native,0x3c);
        if (!view) throw std::runtime_error("Actual stock GFx camera movie has no native view");
        const auto scale=bindings_.get_scale(view);
        if (scale>3) throw std::runtime_error("Actual stock GFx camera movie has an unsupported scale mode");
        if (!hud_scale_saved_) {original_hud_scale_=scale;hud_scale_saved_=true;}
        if (scale!=2) bindings_.set_scale(hud_movie_,2);
        if (bindings_.get_scale(view)!=2)
            throw std::runtime_error("Stock GFx camera HUD did not accept native render-target scaling");
        auto* resource=skeleton_field<void*>(hud_target_,0xf8);
        const auto handle=resource?skeleton_field<std::uintptr_t>(resource,0x24):0;
        if (handle!=hud_texture_.load(std::memory_order_acquire))
            hud_ready_texture_.store(0,std::memory_order_release);
        hud_texture_.store(handle,std::memory_order_release);
    }
    void* hud_movie() const {return hud_movie_;}
    std::uintptr_t hud_texture_handle() const {
        const auto ready=hud_ready_texture_.load(std::memory_order_acquire);
        return ready==hud_texture_.load(std::memory_order_acquire)?ready:0;
    }
    bool needs_hud_image() const {
        const auto pending=hud_texture_.load(std::memory_order_acquire);
        return pending && pending!=hud_ready_texture_.load(std::memory_order_acquire);
    }
    IDirect3DTexture9* hud_texture() {
        const auto found=textures_.find(hud_texture_handle());
        return found==textures_.end()?nullptr:found->second.image.get();
    }
    void release() {
        release_hud();
        settled_since_=0;settled_ms_.store(0,std::memory_order_release);
        image_written_.store(false,std::memory_order_release);
        published_texture_.store(0,std::memory_order_release);
        published_render_target_.store(0,std::memory_order_release);
        published_target_.store(nullptr,std::memory_order_release);
        published_component_.store(0,std::memory_order_release);
        published_chain_.store(0,std::memory_order_release);
        published_proxies_.store(0,std::memory_order_release);
        if (component_ && owner_ && skeleton_field<void*>(component_,0x78)==owner_)
            bindings_.detach(owner_,component_);
        owner_=component_=target_=nullptr;
        configured_chain_=night_vision_chain_=nullptr;
    }
    void hide() {
        settled_since_=0;settled_ms_.store(0,std::memory_order_release);
        image_written_.store(false,std::memory_order_release);
        published_texture_.store(0,std::memory_order_release);
        if (component_ && owner_ && skeleton_field<void*>(component_,0x78)==owner_)
            bindings_.detach(owner_,component_);
    }
    void create(void* owner) {
        if (!owner) throw std::invalid_argument("Native lens capture requires an owned hero");
        if (owner_ && owner_!=owner) release();
        if (component_) return;
        if (!*bindings_.errors)
            throw std::runtime_error("Native object factory error device is not initialized");
        target_=bindings_.construct(bindings_.target_class(),owner,0,0x400000000000,
            nullptr,*bindings_.errors,nullptr,nullptr);
        if (!target_) throw std::runtime_error("Native camcorder render target construction failed");
        write(target_,0x140,std::array<float,4>{{0,0,0,1}});
        bindings_.initialize(target_,512,288,2,0);
        component_=bindings_.construct(bindings_.capture_class(),owner,0,0x400000000000,
            nullptr,*bindings_.errors,nullptr,nullptr);
        if (!component_) throw std::runtime_error("Native camcorder capture construction failed");
        write(component_,0xd0,target_);
        write(component_,0xd8,70.f);
        write(component_,0xdc,1.f);
        write(component_,0xe0,20000.f);
        write(component_,0xe4,UINT(1));
        write(component_,0x88,UINT(bindings_.native_postprocess?7:5));
        write(component_,0x98,30.f);
        bindings_.projection(component_);
        write(component_,0xe4,UINT(0));
        owner_=owner;
    }
    void update_pose(void* owner,Vec3 position,const std::array<INT,3>& rotation,bool night_vision,
                     bool native_raised=true) {
        if (!finite(position)) throw std::invalid_argument("Nonfinite independent lens position");
        create(owner);
        const BYTE mode=night_vision && !bindings_.native_postprocess?1:0;
        void* chain=nullptr;
        if (bindings_.native_postprocess) {
            auto* engine=*bindings_.engine;
            auto* effects=engine?skeleton_field<void*>(engine,0xa74):nullptr;
            chain=effects?skeleton_field<void*>(effects,night_vision?0x338:0x328):nullptr;
            const auto count=chain?skeleton_field<INT>(chain,0x68):0;
            if (count<1 || count>32 || !skeleton_field<void*>(chain,0x60))
                throw std::runtime_error("Actual retail camera postprocess chain is unavailable or invalid");
        }
        if ((skeleton_field<BYTE>(component_,0x90)!=mode || configured_chain_!=chain) &&
            skeleton_field<void*>(component_,0x78))
            bindings_.detach(owner_,component_);
        if (!skeleton_field<void*>(component_,0x78))
            image_written_.store(false,std::memory_order_release);
        const auto now=GetTickCount64();
        if (!native_raised) settled_since_=0;
        else if (!settled_since_) settled_since_=now;
        settled_ms_.store(settled_since_?UINT(std::min<ULONGLONG>(UINT_MAX,now-settled_since_)):0,
            std::memory_order_release);
        write(component_,0x90,mode);
        if (bindings_.native_postprocess) write(component_,0x9c,chain);
        capture_mode_.store(mode,std::memory_order_release);
        bindings_.set_view(component_,&position,rotation.data());
        if (!skeleton_field<void*>(component_,0x78)) bindings_.attach(owner_,component_);
        if (skeleton_field<void*>(component_,0x78)!=owner_ ||
            skeleton_field<void*>(component_,0xd0)!=target_)
            throw std::runtime_error("Native lens target/component lost engine ownership");
        if (bindings_.native_postprocess) {
            const auto count=skeleton_field<INT>(component_,0xc8);
            UINT64 flags[2]{};bindings_.show_flags(component_,flags);
            if (count<1 || count>32 || !(flags[0]&0x40) ||
                skeleton_field<void*>(component_,0x9c)!=chain)
                throw std::runtime_error("Native lens did not instantiate its actual retail postprocess effects");
            published_proxies_.store(count,std::memory_order_release);
        }
        configured_chain_=chain;
        published_component_.store(reinterpret_cast<std::uintptr_t>(component_),std::memory_order_release);
        published_chain_.store(reinterpret_cast<std::uintptr_t>(chain),std::memory_order_release);
        published_target_.store(target_,std::memory_order_release);
        auto* resource=skeleton_field<void*>(target_,0xf8);
        published_render_target_.store(resource?reinterpret_cast<std::uintptr_t>(resource)+0x5c:0,
            std::memory_order_release);
        published_texture_.store(resource?skeleton_field<std::uintptr_t>(resource,0x24):0,
            std::memory_order_release);
        refresh_hud();
    }
    IDirect3DTexture9* texture() {
        const auto handle=texture_handle();
        const auto found=textures_.find(handle);
        if (!handle || found==textures_.end()) return nullptr;
        const auto hud=hud_texture_.load(std::memory_order_acquire);
        for (auto it=textures_.begin();it!=textures_.end();) {
            // RHI allocation can precede game-thread publication of either target.
            if (it->first==handle || it->first==hud) {
                it->second.published=true;
                ++it;
            } else if (it->second.published) {
                it=textures_.erase(it);
            } else ++it;
        }
        return found->second.image.get();
    }
    bool native_postprocess() const {return bindings_.native_postprocess;}
    std::uintptr_t render_target_address() const {
        return published_render_target_.load(std::memory_order_acquire);
    }
    std::uintptr_t component_address() const {return published_component_.load(std::memory_order_acquire);}
    std::uintptr_t postprocess_chain() const {return published_chain_.load(std::memory_order_acquire);}
    std::uintptr_t texture_handle() const {
        return image_written_.load(std::memory_order_acquire)?pending_texture_handle():0;
    }
    std::uintptr_t pending_texture_handle() const {
        return published_texture_.load(std::memory_order_acquire);
    }
    bool needs_image() const {
        return pending_texture_handle() && !image_written_.load(std::memory_order_acquire);
    }
    void acknowledge_image(std::uintptr_t handle) {
        if (handle && handle==pending_texture_handle()) image_written_.store(true,std::memory_order_release);
    }
    void surface_written(IDirect3DSurface9& surface) {
        const auto hud=hud_texture_.load(std::memory_order_acquire);
        const auto image=textures_.find(hud);
        if (hud && hud!=hud_ready_texture_.load(std::memory_order_acquire) && image!=textures_.end()) {
            Com<IDirect3DSurface9> target;
            gpu_check(image->second.image->GetSurfaceLevel(0,target.put()),"Identify completed stock camera HUD output");
            if (target.get()==&surface) {
                image->second.published=true;
                hud_ready_texture_.store(hud,std::memory_order_release);
            }
        }
        if (!needs_image()) return;
        const auto handle=pending_texture_handle();
        const auto found=textures_.find(handle);
        if (found==textures_.end()) return;
        Com<IDirect3DSurface9> target;
        gpu_check(found->second.image->GetSurfaceLevel(0,target.put()),"Identify completed native lens output");
        if (target.get()==&surface) {
            found->second.published=true;
            acknowledge_image(handle);
        }
    }
    bool needs_hud_draw_trace(UINT frame) const {
        return hud_draws_.is_open() && frame>=400 && hud_draw_count_<256;
    }
    void observe_hud_command(const void* record,void* root,UINT frame,INT index,INT count) {
        if (!hud_commands_.is_open() || frame<400 || hud_command_count_>=64 || !root) return;
        const auto* handle=skeleton_field<const unsigned char*>(record,0x44);
        if (!handle) throw std::runtime_error("Owned HUD command has no native display handle");
        const auto hex=[&](const void* pointer,std::size_t size) {
            constexpr char digits[]="0123456789abcdef";
            const auto* bytes=static_cast<const unsigned char*>(pointer);
            for (std::size_t i=0;i<size;++i)
                hud_commands_<<digits[bytes[i]>>4]<<digits[bytes[i]&15];
        };
        hud_commands_<<"{\"frame\":"<<frame<<",\"index\":"<<index<<",\"count\":"<<count
            <<",\"root\":"<<reinterpret_cast<std::uintptr_t>(root)<<",\"record_hex\":\"";
        hex(record,0x8c);hud_commands_<<"\",\"handle_hex\":\"";hex(handle,0x38);
        hud_commands_<<"\"}\n";hud_commands_.flush();
        if (!hud_commands_) throw std::runtime_error("Cannot retain owned native HUD command");
        ++hud_command_count_;
    }
    void trace_hud_draw(IDirect3DDevice9& device,IDirect3DSurface9& surface,UINT frame,
        std::uintptr_t native_base,bool owned_native_hud=false,bool corrected_alpha=false,
        std::uintptr_t display_root=0) {
        if (!needs_hud_draw_trace(frame)) return;
        const auto handle=hud_texture_.load(std::memory_order_acquire);
        const auto found=textures_.find(handle);
        if (!handle || found==textures_.end()) return;
        Com<IDirect3DSurface9> target;
        gpu_check(found->second.image->GetSurfaceLevel(0,target.put()),"Identify diagnostic native HUD draw");
        const bool exact_output=target.get()==&surface;
        D3DSURFACE_DESC extent{};
        gpu_check(surface.GetDesc(&extent),"Read diagnostic HUD candidate target extent");
        if (!exact_output && !owned_native_hud && (extent.Width!=512 || extent.Height!=288)) return;
        Com<IDirect3DPixelShader9> shader;
        gpu_check(device.GetPixelShader(shader.put()),"Read actual native HUD pixel program");
        if (!shader.get()) throw std::runtime_error("Native HUD draw has no pixel program");
        UINT bytes=0;
        gpu_check(shader->GetFunction(nullptr,&bytes),"Read native HUD pixel program extent");
        if (bytes<8 || bytes%4 || bytes>1048576)
            throw std::runtime_error("Invalid native HUD pixel program extent");
        std::vector<DWORD> code(bytes/4);
        const auto expected=bytes;
        gpu_check(shader->GetFunction(code.data(),&bytes),"Read native HUD pixel program");
        if (bytes!=expected) throw std::runtime_error("Native HUD pixel program extent changed");
        const auto hash=sha256_bytes(code.data(),bytes);
        if (!exact_output && !owned_native_hud &&
            hash!="556202ee49f342033594922037c8e007b4deeb0ce0bad04fd9cf7b56e0a84681" &&
            hash!="1450de20a3e8075d08efa8e1c2d7b85e67528a5b04483be2a873ef95c3ca3ffd" &&
            hash!="5da322672b247345a528964e1773fba3c294e3fe3279b738ba072396b76dd58f") return;
        const auto path=folder_/("native-hud-shader-"+hash+".bin");
        if (!std::filesystem::exists(path)) {
            std::ofstream output(path,std::ios::binary);
            output.write(reinterpret_cast<const char*>(code.data()),bytes);output.close();
            if (!output) throw std::runtime_error("Cannot retain actual native HUD pixel program");
        }
        hud_draws_<<"{\"frame\":"<<frame<<",\"rhi_texture\":"<<handle
            <<",\"surface\":"<<reinterpret_cast<std::uintptr_t>(&surface)
            <<",\"exact_hud_output\":"<<(exact_output?"true":"false")
            <<",\"source_candidate\":"<<(!exact_output?"true":"false")
            <<",\"owned_native_hud\":"<<(owned_native_hud?"true":"false")
            <<",\"corrected_alpha\":"<<(corrected_alpha?"true":"false")
            <<",\"display_root\":"<<display_root
            <<",\"pixel_shader_sha256\":"<<std::quoted(hash)<<",\"states\":{";
        bool separator=false;
        for (auto state:{D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,
            D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,
            D3DRS_SRGBWRITEENABLE,D3DRS_COLORWRITEENABLE,D3DRS_STENCILENABLE}) {
            DWORD value=0;
            gpu_check(device.GetRenderState(state,&value),"Read actual native HUD blend state");
            hud_draws_<<(separator?",":"")<<std::quoted(std::to_string(UINT(state)))<<':'<<value;
            separator=true;
        }
        std::array<float,4> gamma{};
        gpu_check(device.GetPixelShaderConstantF(0,gamma.data(),1),"Read actual native HUD gamma constant");
        std::array<UINT,4> gamma_bits{};
        std::memcpy(gamma_bits.data(),gamma.data(),sizeof(gamma));
        hud_draws_<<"},\"pixel_c0_bits\":[";
        for (UINT i=0;i<4;++i) hud_draws_<<(i?",":"")<<gamma_bits[i];
        hud_draws_<<"],\"native_stack_rvas\":[";
        void* stack[24]{};
        const auto depth=CaptureStackBackTrace(0,UINT(std::size(stack)),stack,nullptr);
        separator=false;
        for (USHORT i=0;i<depth;++i) {
            const auto address=reinterpret_cast<std::uintptr_t>(stack[i]);
            if (address>=native_base && address-native_base<0x3000000) {
                hud_draws_<<(separator?",":"")<<address-native_base;separator=true;
            }
        }
        hud_draws_<<"]}\n";hud_draws_.flush();
        if (!hud_draws_) throw std::runtime_error("Cannot retain actual native HUD blend evidence");
        ++hud_draw_count_;
    }
    template<class Submit> HRESULT draw_hud_alpha(IDirect3DDevice9& device,Submit submit) {
        DWORD enabled=0,write_mask=0;
        gpu_check(device.GetRenderState(D3DRS_ALPHABLENDENABLE,&enabled),"Read owned HUD alpha admission");
        gpu_check(device.GetRenderState(D3DRS_COLORWRITEENABLE,&write_mask),"Read owned HUD alpha write mask");
        if (!enabled || !(write_mask&D3DCOLORWRITEENABLE_ALPHA)) return submit();
        for (auto state:{D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP}) {
            DWORD actual=0;
            gpu_check(device.GetRenderState(state,&actual),"Read owned HUD source-over contract");
            const DWORD expected=state==D3DRS_SRCBLEND?DWORD(D3DBLEND_SRCALPHA):
                state==D3DRS_DESTBLEND?DWORD(D3DBLEND_INVSRCALPHA):DWORD(D3DBLENDOP_ADD);
            if (actual!=expected) throw std::runtime_error("Owned HUD has an unsupported native blend contract");
        }
        const std::array<D3DRENDERSTATETYPE,4> states{{D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,
            D3DRS_BLENDOPALPHA,D3DRS_SEPARATEALPHABLENDENABLE}};
        const std::array<DWORD,4> selected{{D3DBLEND_ONE,D3DBLEND_INVSRCALPHA,D3DBLENDOP_ADD,TRUE}};
        std::array<DWORD,4> original{};
        for (UINT i=0;i<states.size();++i)
            gpu_check(device.GetRenderState(states[i],&original[i]),"Save owned HUD native alpha state");
        const auto restore=[&] {
            for (UINT i=0;i<states.size();++i)
                gpu_check(device.SetRenderState(states[i],original[i]),"Restore owned HUD native alpha state");
        };
        try {
            for (UINT i=0;i<states.size();++i)
                gpu_check(device.SetRenderState(states[i],selected[i]),"Retain native HUD source-over coverage");
            const auto result=submit();restore();return result;
        } catch (...) {restore();throw;}
    }
    void remember_texture(std::uintptr_t handle,IDirect3DTexture9& texture) {
        if (!handle) throw std::runtime_error("Native lens GPU association has no RHI handle");
        texture.AddRef();
        auto& retained=textures_[handle];
        retained.image.reset();*retained.image.put()=&texture;
        retained.published=false;
    }
    void release_device_resources() {
        published_render_target_.store(0,std::memory_order_release);
        hud_ready_texture_.store(0,std::memory_order_release);
        hud_texture_.store(0,std::memory_order_release);
        image_written_.store(false,std::memory_order_release);
        published_texture_.store(0,std::memory_order_release);
        textures_.clear();
    }
    UINT frame() const {return frame_.load(std::memory_order_acquire);}
    void advance_frame(UINT frame) {frame_.store(frame,std::memory_order_release);}
    UINT sampled() const {return sampled_;}
    bool diagnostic_ready() const {return settled_ms_.load(std::memory_order_acquire)>=500;}
    bool hud_timed_out() const {return settled_ms_.load(std::memory_order_acquire)>=5000;}
    NativeCamcorderProbe(NativeCamcorderProbeBindings bindings,const std::filesystem::path& folder)
        :bindings_(bindings),folder_(folder) {
        if (!bindings.capture_class || !bindings.target_class || !bindings.construct ||
            !bindings.initialize || !bindings.attach || !bindings.detach || !bindings.set_view ||
            !bindings.projection || !bindings.errors || !bindings.engine ||
            !bindings.activate_night_vision || !bindings.local_player || !bindings.show_flags)
            throw std::invalid_argument("Incomplete native camcorder probe bindings");
        if (folder_.empty()) return;
        evidence_.open(folder_/"native-lens-probe.jsonl");
        views_.open(folder_/"native-lens-views.jsonl");
        samples_.open(folder_/"native-lens-samples.jsonl");
        hud_draws_.open(folder_/"native-hud-draws.jsonl");
        hud_commands_.open(folder_/"native-hud-commands.jsonl");
        hud_readiness_.open(folder_/"native-hud-readiness.jsonl");
        if (!evidence_ || !views_ || !samples_ || !hud_draws_ || !hud_commands_ || !hud_readiness_)
            throw std::runtime_error("Cannot retain native lens probe evidence");
    }
    void observe_view(Vec3 origin,const std::array<float,16>& projection,UINT frame,const TrackingFrame& tracking,
                      INT postprocess_count) {
        if (!views_.is_open()) return;
        views_<<std::setprecision(9)<<"{\"frame\":"<<frame<<",\"origin\":["
            <<origin.x<<','<<origin.y<<','<<origin.z<<"],\"projection\":[";
        for (unsigned i=0;i<16;++i) views_<<(i?",":"")<<projection[i];
        views_<<"],\"right\":["<<tracking.right.x<<','<<tracking.right.y<<','<<tracking.right.z
            <<"],\"up\":["<<tracking.up.x<<','<<tracking.up.y<<','<<tracking.up.z
            <<"],\"backward\":["<<tracking.backward.x<<','<<tracking.backward.y<<','<<tracking.backward.z
            <<"],\"render_view_postprocess_count\":"<<postprocess_count<<"}\n";views_.flush();
        if (!views_) throw std::runtime_error("Cannot retain actual native capture view");
    }
    void update(void* owner,const std::array<float,6>& native_camera,UINT frame) {
        if (!owner) throw std::invalid_argument("Native lens capture requires an owned hero");
        for (float value:native_camera)
            if (!std::isfinite(value)) throw std::runtime_error("Nonfinite native lens source pose");
        const Vec3 lens{native_camera[0]+40,native_camera[1]+20,native_camera[2]};
        const float yaw=frame<650?45.f:-45.f;
        const std::array<INT,3> rotation{INT(std::lround(native_camera[4]*(65536./360))),
            INT(std::lround((native_camera[3]+yaw)*(65536./360))),
            INT(std::lround(native_camera[5]*(65536./360)))};
        const bool night_vision=bindings_.night_vision_experiment && frame>=950;
        update_pose(owner,lens,rotation,night_vision);
        evidence_<<std::setprecision(9)<<"{\"frame\":"<<frame
            <<",\"owner\":"<<reinterpret_cast<std::uintptr_t>(owner)
            <<",\"component\":"<<reinterpret_cast<std::uintptr_t>(component_)
            <<",\"target\":"<<reinterpret_cast<std::uintptr_t>(target_)
            <<",\"position\":["<<lens.x<<','<<lens.y<<','<<lens.z<<"],\"native_camera\":[";
        for (unsigned i=0;i<6;++i) evidence_<<(i?",":"")<<native_camera[i];
        UINT64 flags[2]{};bindings_.show_flags(component_,flags);
        evidence_<<"],\"show_flags\":["<<flags[0]<<','<<flags[1]
            <<"],\"lens_yaw_offset\":"<<yaw<<",\"native_postprocess_chain\":0,\"chain_effects\":0"
            <<",\"capture_mode\":"<<unsigned(skeleton_field<BYTE>(component_,0x90))
            <<",\"night_vision\":"<<(night_vision?"true":"false")<<",\"capture_proxies\":"
            <<skeleton_field<INT>(component_,0xc8)<<"}\n";evidence_.flush();
        if (!evidence_) throw std::runtime_error("Cannot retain independent native lens pose");
    }
    void sample(IDirect3DDevice9& device,UINT frame) {
        advance_frame(frame);
        auto* target=published_target_.load(std::memory_order_acquire);
        if (folder_.empty() || !target || !diagnostic_ready() ||
            sampled_>=3 || frame<400+sampled_*300) return;
        const auto handle=texture_handle();
        if (!handle) return;
        const auto found=textures_.find(handle);
        if (found==textures_.end()) {
            if (frame>450+sampled_*300)
                throw std::runtime_error("Native lens resource has no observed D3D texture association");
            return;
        }
        if (hud_configured_.load(std::memory_order_acquire)) {
            const bool ready=hud_texture_handle() && hud_texture();
            if ((!ready && !sample_waiting_for_hud_) || (ready && sample_waiting_for_hud_)) {
                hud_readiness_<<"{\"frame\":"<<frame<<",\"ready\":"<<(ready?"true":"false")
                    <<",\"settled_ms\":"<<settled_ms_.load(std::memory_order_acquire)
                    <<",\"pending_hud_texture\":"<<hud_texture_.load(std::memory_order_acquire)
                    <<",\"hud_association_retained\":"
                    <<(textures_.count(hud_texture_.load(std::memory_order_acquire))?"true":"false")
                    <<",\"ready_hud_texture\":"<<hud_ready_texture_.load(std::memory_order_acquire)
                    <<",\"retained_texture_associations\":"<<textures_.size()<<"}\n";
                hud_readiness_.flush();
                if (!hud_readiness_) throw std::runtime_error("Cannot retain native HUD sample readiness");
                sample_waiting_for_hud_=!ready;
            }
            if (!ready) {
                if (hud_timed_out())
                    throw std::runtime_error("Native camera sample has no completed stock GFx HUD image");
                return;
            }
        }
        auto* texture=found->second.image.get();
        D3DSURFACE_DESC description{};
        gpu_check(texture->GetLevelDesc(0,&description),"Inspect independent native lens image");
        if (description.Width!=512 || description.Height!=288 || description.Format!=D3DFMT_A8R8G8B8)
            throw std::runtime_error("Independent native lens image has unexpected extent/format");
        Com<IDirect3DSurface9> source,readback;
        gpu_check(texture->GetSurfaceLevel(0,source.put()),"Acquire actual native lens surface");
        gpu_check(device.CreateOffscreenPlainSurface(512,288,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create diagnostic native lens readback");
        gpu_check(device.GetRenderTargetData(source.get(),readback.get()),"Read actual independent native lens image");
        D3DLOCKED_RECT locked{};
        gpu_check(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Synchronize independent native lens image");
        const auto path=folder_/("native-lens-"+std::to_string(sampled_)+".ppm");
        std::ofstream image(path,std::ios::binary);
        image<<"P6\n512 288\n255\n";
        for (UINT y=0;y<288;++y) for (UINT x=0;x<512;++x) {
            const DWORD pixel=reinterpret_cast<const DWORD*>(static_cast<const char*>(locked.pBits)+y*locked.Pitch)[x];
            const char rgb[]{char(pixel>>16),char(pixel>>8),char(pixel)};
            image.write(rgb,3);
        }
        gpu_check(readback->UnlockRect(),"Release native lens diagnostic readback");
        if (!image) throw std::runtime_error("Cannot retain actual native lens image");
        image.close();
        if (!image) throw std::runtime_error("Cannot finalize actual native lens image");
        std::uintptr_t hud_handle=0,hud_d3d=0;
        std::string hud_sha;
        if (hud_configured_.load(std::memory_order_acquire)) {
            auto* hud=hud_texture();
            hud_handle=hud_texture_handle();
            if (!hud || !hud_handle)
                throw std::runtime_error("Native camera sample has no completed stock GFx HUD image");
            D3DSURFACE_DESC hud_description{};
            gpu_check(hud->GetLevelDesc(0,&hud_description),"Inspect actual native GFx HUD image");
            if (hud_description.Width!=512 || hud_description.Height!=288 ||
                hud_description.Format!=D3DFMT_A8R8G8B8)
                throw std::runtime_error("Actual native GFx HUD image has unexpected extent/format");
            Com<IDirect3DSurface9> hud_surface;
            gpu_check(hud->GetSurfaceLevel(0,hud_surface.put()),"Acquire actual native GFx HUD surface");
            gpu_check(device.GetRenderTargetData(hud_surface.get(),readback.get()),"Read actual native GFx HUD image");
            gpu_check(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Synchronize actual native GFx HUD pixels");
            std::vector<unsigned char> hud_pixels(512*288*4);
            for (UINT y=0;y<288;++y)
                std::memcpy(hud_pixels.data()+y*512*4,
                    static_cast<const char*>(locked.pBits)+y*locked.Pitch,512*4);
            gpu_check(readback->UnlockRect(),"Release native GFx HUD diagnostic readback");
            hud_sha=sha256_bytes(hud_pixels.data(),hud_pixels.size());
            std::ofstream hud_image(folder_/("native-hud-"+std::to_string(sampled_)+".bgra"),std::ios::binary);
            hud_image.write(reinterpret_cast<const char*>(hud_pixels.data()),hud_pixels.size());
            hud_image.close();
            if (!hud_image) throw std::runtime_error("Cannot retain actual native GFx HUD alpha image");
            hud_d3d=reinterpret_cast<std::uintptr_t>(hud);
        }
        samples_<<"{\"frame\":"<<frame<<",\"sample\":"<<sampled_
            <<",\"settled_ms\":"<<settled_ms_.load(std::memory_order_acquire)
            <<",\"capture_mode\":"<<unsigned(capture_mode_.load(std::memory_order_acquire))
            <<",\"native_target\":"<<reinterpret_cast<std::uintptr_t>(target)
            <<",\"native_component\":"<<component_address()
            <<",\"native_postprocess_chain\":"<<postprocess_chain()
            <<",\"native_postprocess_proxies\":"<<published_proxies_.load(std::memory_order_acquire)
            <<",\"rhi_texture\":"<<handle<<",\"d3d_texture\":"
            <<reinterpret_cast<std::uintptr_t>(texture)<<",\"width\":512,\"height\":288"
            <<",\"native_hud_rhi_texture\":"<<hud_handle<<",\"native_hud_d3d_texture\":"<<hud_d3d
            <<",\"native_hud_sha256\":"<<std::quoted(hud_sha)<<"}\n";
        samples_.flush();
        if (!samples_) throw std::runtime_error("Cannot retain native lens GPU association");
        ++sampled_;
    }
};
}
