#pragma once
#include "d3d9_stereo_pipeline.h"
#include "d3d9_state_restore.h"
#include "sm3_runtime.h"
#include "outlast_target_roles.h"
#include "outlast_hit_distortion.h"
#include <memory>
#include <cmath>
#include <unordered_map>
#include <filesystem>
#include <fstream>

namespace olvr {

class D3D9ShaderVariants {
    struct Key {
        IDirect3DVertexShader9* vertex;
        IDirect3DPixelShader9* pixel;
        unsigned atlas_samplers;
        bool eye_camera;
        bool eye_view;
        bool scissor;
        bool viewport_transform;
        bool tracked_uniforms;
        bool ui_plane;
        unsigned point_atlas_samplers=0;
        bool operator==(const Key& other) const {
            return vertex==other.vertex && pixel==other.pixel &&
                atlas_samplers==other.atlas_samplers && eye_camera==other.eye_camera &&
                eye_view==other.eye_view && scissor==other.scissor &&
                viewport_transform==other.viewport_transform && tracked_uniforms==other.tracked_uniforms &&
                ui_plane==other.ui_plane && point_atlas_samplers==other.point_atlas_samplers;
        }
    };
    struct Hash {
        std::size_t operator()(const Key& key) const {
            return std::hash<void*>{}(key.vertex)^(std::hash<void*>{}(key.pixel)<<1)^
                (std::hash<unsigned>{}(key.atlas_samplers)<<2)^std::hash<bool>{}(key.eye_camera)^
                (std::hash<bool>{}(key.eye_view)<<3)^(std::hash<bool>{}(key.scissor)<<4)^
                (std::hash<bool>{}(key.viewport_transform)<<5)^(std::hash<bool>{}(key.tracked_uniforms)<<6)^
                (std::hash<bool>{}(key.ui_plane)<<7)^(std::hash<unsigned>{}(key.point_atlas_samplers)<<8);
        }
    };
    struct Variant {
        Com<IDirect3DVertexShader9> original_vertex,vertex;
        Com<IDirect3DPixelShader9> original_pixel,pixel;
        std::unique_ptr<D3D9StereoPipeline> pipeline;
    };
    struct PixelSamplers {
        Com<IDirect3DPixelShader9> original;
        unsigned declared=0,two_dimensional=0;
        OutlastEffectConstants effects;
    };
    struct MonoVariant {
        Com<IDirect3DPixelShader9> original,pixel;
        std::vector<std::pair<unsigned,unsigned>> sampler_constants;
        std::vector<std::pair<unsigned,unsigned>> point_samplers;
    };
    struct VertexInfo {
        Com<IDirect3DVertexShader9> original;
        bool view_projection=false;
    };
    Com<IDirect3DDevice9> device_;
    std::shared_ptr<D3D9StateCache> state_cache_=std::make_shared<D3D9StateCache>();
    std::unordered_map<Key,std::unique_ptr<Variant>,Hash> cache_;
    std::unordered_map<Key,std::unique_ptr<MonoVariant>,Hash> mono_cache_;
    std::unordered_map<IDirect3DPixelShader9*,std::unique_ptr<PixelSamplers>> pixel_samplers_;
    std::unordered_map<IDirect3DVertexShader9*,std::unique_ptr<VertexInfo>> vertex_info_;
    D3D9PointAtlasLookup mono_point_atlas_;
    bool hit_distortion_=true;
    bool bloom_=true,film_grain_=true;
    template<class T> static sm3::Words bytecode(T& shader) {
        UINT size=0;
        gpu_check(shader.GetFunction(nullptr,&size),"Read bound shader extent");
        if (size<8 || size%4 || size>1048576)
            throw std::runtime_error("Bound shader exceeds supported SM3 extent");
        sm3::Words result(size/4);
        const UINT expected=size;
        gpu_check(shader.GetFunction(result.data(),&size),"Read exact bound shader bytecode");
        if (size!=expected) throw std::runtime_error("Bound shader bytecode changed while reading");
        return result;
    }
    const PixelSamplers& sampler_usage(IDirect3DPixelShader9& pixel) {
        auto found=pixel_samplers_.find(&pixel);
        if (found==pixel_samplers_.end()) {
            auto usage=std::make_unique<PixelSamplers>();
            *usage->original.put()=&pixel;pixel.AddRef();
            const auto code=bytecode(pixel);
            usage->effects=outlast_effect_constants(code,bloom_,film_grain_);
            for (const auto& instruction:sm3::parse(code))
                if (instruction.opcode==31 && sm3::kind(instruction.tokens[2])==10) {
                    const auto index=sm3::index(instruction.tokens[2]);
                    if (index>=16) throw std::invalid_argument("Pixel sampler exceeds the native SM3 range");
                    usage->declared|=1u<<index;
                    if (((instruction.tokens[1]>>27)&15)==2) usage->two_dimensional|=1u<<index;
                }
            found=pixel_samplers_.emplace(&pixel,std::move(usage)).first;
        }
        return *found->second;
    }
    static unsigned checked_atlas_samplers(unsigned mask,const PixelSamplers& usage) {
        if (mask & usage.declared & ~usage.two_dimensional)
            throw std::invalid_argument("A bound native atlas conflicts with a non-2D pixel sampler");
        return mask & usage.two_dimensional;
    }
    static void retain_rejection(const sm3::Words& vertex,const sm3::Words& pixel,
                                 const sm3::Pair& patched,const Key& key) {
        std::array<wchar_t,32768> path{};
        const auto length=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",path.data(),DWORD(path.size()));
        if (!length) return;
        if (length>=path.size()) throw std::runtime_error("Rejected-shader evidence path is too long");
        const std::filesystem::path directory(path.data());
        if (!directory.is_absolute() || !std::filesystem::is_directory(directory))
            throw std::runtime_error("Rejected shader requires its existing absolute probe evidence directory");
        const std::array<std::pair<const char*,const sm3::Words*>,4> programs{{
            {"shader-rejection-original-vs.bin",&vertex},{"shader-rejection-original-ps.bin",&pixel},
            {"shader-rejection-patched-vs.bin",&patched.vertex},{"shader-rejection-patched-ps.bin",&patched.pixel}}};
        for (const auto& program:programs) {
            const auto output=directory/program.first;
            if (std::filesystem::exists(output))
                throw std::runtime_error("Refusing to overwrite an earlier rejected native shader");
            std::ofstream stream(output,std::ios::binary);
            stream.write(reinterpret_cast<const char*>(program.second->data()),
                static_cast<std::streamsize>(program.second->size()*4));
            if (!stream) throw std::runtime_error("Cannot retain exact rejected native shader bytes");
        }
        std::ofstream metadata(directory/"shader-rejection.json");
        metadata<<"{\"atlas_samplers\":"<<key.atlas_samplers
            <<",\"point_atlas_samplers\":"<<key.point_atlas_samplers
            <<",\"eye_camera\":"<<key.eye_camera<<",\"eye_view\":"<<key.eye_view
            <<",\"scissor\":"<<key.scissor<<",\"viewport_transform\":"<<key.viewport_transform
            <<",\"tracked_uniforms\":"<<key.tracked_uniforms<<",\"ui_plane\":"<<key.ui_plane<<"}\n";
        if (!metadata) throw std::runtime_error("Cannot retain rejected native shader configuration");
    }
public:
    void effects(bool bloom,bool film_grain) {
        if (bloom_==bloom && film_grain_==film_grain) return;
        cache_.clear();mono_cache_.clear();pixel_samplers_.clear();
        bloom_=bloom;film_grain_=film_grain;
    }
    void hit_distortion(bool enabled) {
        if (hit_distortion_==enabled) return;
        cache_.clear();mono_cache_.clear();
        hit_distortion_=enabled;
    }
    std::uint64_t state_allocations() const {return state_cache_->allocations;}
    std::uint64_t state_captures() const {return state_cache_->captures;}
    explicit D3D9ShaderVariants(IDirect3DDevice9& device,bool hit_distortion=true):
        hit_distortion_(hit_distortion) {
        *device_.put()=&device;
        device.AddRef();
    }
    unsigned active_atlas_samplers(unsigned mask) {
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device_->GetPixelShader(pixel.put()),"Get declared atlas sampler shader");
        if (!pixel.get()) throw std::runtime_error("Selected atlas draw has no programmable pixel shader");
        return checked_atlas_samplers(mask,sampler_usage(*pixel.get()));
    }
    unsigned bound_atlas_samplers(Com<IDirect3DPixelShader9>* selected=nullptr) {
        Com<IDirect3DPixelShader9> local;
        auto& pixel=selected?*selected:local;
        gpu_check(device_->GetPixelShader(pixel.put()),"Get declared atlas sampler shader");
        if (!pixel.get()) throw std::runtime_error("Selected atlas draw has no programmable pixel shader");
        const auto& usage=sampler_usage(*pixel.get());
        return checked_atlas_samplers(atlas_sampler_mask(*device_.get(),usage.declared),usage);
    }
    template<class Draw> HRESULT draw_mono(Draw draw,bool logical_atlas_output=false) {
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device_->GetPixelShader(pixel.put()),"Read native mono resolve shader");
        unsigned samplers=0;
        const PixelSamplers* usage=nullptr;
        if (pixel.get()) {
            usage=&sampler_usage(*pixel.get());
            samplers=checked_atlas_samplers(atlas_sampler_mask(*device_.get(),usage->declared),*usage);
        }
        Com<IDirect3DSurface9> target;
        OutlastAtlasTag tag{};
        if (logical_atlas_output)
            gpu_check(device_->GetRenderTarget(0,target.put()),"Read native mono destination");
        const bool atlas_output=target.get() && atlas_tag(*target.get(),tag);
        if (!samplers && !atlas_output && (!usage || usage->effects.empty())) return draw();
        MonoVariant* variant=nullptr;
        if (samplers) {
            const auto points=native_point_atlas_samplers(*device_.get(),samplers);
            const Key key{nullptr,pixel.get(),samplers,false,false,false,false,false,false,points};
            auto found=mono_cache_.find(key);
            if (found==mono_cache_.end()) {
                auto created=std::make_unique<MonoVariant>();
                const auto original=bytecode(*pixel.get());
                const auto patched=sm3::patch_mono_atlas(hit_distortion_?original:
                    outlast_without_hit_distortion(original),samplers,points);
                const std::vector<DWORD> code(patched.pixel.begin(),patched.pixel.end());
                gpu_check(device_->CreatePixelShader(code.data(),created->pixel.put()),"Create native mono atlas resolve");
                *created->original.put()=pixel.get();pixel->AddRef();
                created->sampler_constants=patched.sampler_constants;
                created->point_samplers=patched.point_samplers;
                found=mono_cache_.emplace(key,std::move(created)).first;
            }
            variant=found->second.get();
        }
        StateRestore restore(*device_.get(),*state_cache_);
        if (atlas_output) {
            D3DVIEWPORT9 viewport{};
            gpu_check(device_->GetViewport(&viewport),"Read physical native mono atlas viewport");
            if (viewport.X%2 || !viewport.Width || viewport.Width%2)
                throw std::runtime_error("Native mono destination does not preserve logical viewport coordinates");
            viewport.X/=2;viewport.Width/=2;
            gpu_check(device_->SetViewport(&viewport),"Keep the owned native camera in its logical atlas view");
        }
        if (variant) gpu_check(device_->SetPixelShader(variant->pixel.get()),"Bind native mono atlas resolve");
        if (variant) mono_point_atlas_.bind(*device_.get(),variant->point_samplers);
        if (variant) for (const auto& entry:variant->sampler_constants) {
            Com<IDirect3DBaseTexture9> texture;
            gpu_check(device_->GetTexture(entry.first,texture.put()),"Read mono resolve source");
            if (!texture.get() || texture->GetType()!=D3DRTYPE_TEXTURE)
                throw std::runtime_error("Mono atlas resolve requires its admitted 2D source");
            D3DSURFACE_DESC description{};
            gpu_check(static_cast<IDirect3DTexture9*>(texture.get())->GetLevelDesc(0,&description),
                "Read mono resolve atlas extent");
            if (description.Width<2 || description.Width%2)
                throw std::runtime_error("Mono resolve source lacks two equal-width eyes");
            const float texel=.5f/static_cast<float>(description.Width);
            const float binding[]{.5f,0,texel,.5f-texel};
            gpu_check(device_->SetPixelShaderConstantF(entry.second,binding,1),"Map native mono UVs into their rendered view");
        }
        if (usage) outlast_apply_effect_constants(*device_.get(),usage->effects);
        const auto result=draw();
        const auto restored=restore.apply();
        return FAILED(result)?result:restored;
    }
    bool orthographic_view() {
        Com<IDirect3DVertexShader9> vertex;
        gpu_check(device_->GetVertexShader(vertex.put()),"Get native UI projection shader");
        if (!vertex.get()) throw std::runtime_error("Selected atlas draw has no programmable vertex shader");
        auto found=vertex_info_.find(vertex.get());
        if (found==vertex_info_.end()) {
            auto info=std::make_unique<VertexInfo>();
            *info->original.put()=vertex.get();vertex->AddRef();
            info->view_projection=sm3::inspect(sm3::parse(bytecode(*vertex.get()))).view_projection;
            found=vertex_info_.emplace(vertex.get(),std::move(info)).first;
        }
        if (!found->second->view_projection) return false;
        std::array<float,16> matrix{};
        gpu_check(device_->GetVertexShaderConstantF(0,matrix.data(),4),"Read original native UI projection");
        for (float value:matrix) if (!std::isfinite(value))
            throw std::invalid_argument("Native UI projection contains nonfinite values");
        return std::abs(matrix[3])+std::abs(matrix[7])+std::abs(matrix[11])<.0001f &&
            std::abs(matrix[15]-1)<.0001f;
    }
    D3D9StereoPipeline& current(unsigned atlas_samplers=0,bool eye_camera=false,
                               bool eye_view=true,bool scissor=false,bool viewport_transform=false,
                               bool tracked_uniforms=false,bool ui_plane=false,
                               IDirect3DPixelShader9* selected_pixel=nullptr) {
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device_->GetVertexShader(vertex.put()),"Get original bound vertex shader");
        if (selected_pixel) {*pixel.put()=selected_pixel;selected_pixel->AddRef();}
        else gpu_check(device_->GetPixelShader(pixel.put()),"Get original bound pixel shader");
        if (!vertex.get() || !pixel.get()) throw std::runtime_error("Selected scene draw has no programmable SM3 shader pair");
        atlas_samplers=checked_atlas_samplers(atlas_samplers,sampler_usage(*pixel.get()));
        const auto points=native_point_atlas_samplers(*device_.get(),atlas_samplers);
        const Key key{vertex.get(),pixel.get(),atlas_samplers,eye_camera,eye_view,scissor,viewport_transform,tracked_uniforms,ui_plane,points};
        const auto found=cache_.find(key);
        if (found!=cache_.end()) return *found->second->pipeline;
        const auto original_vertex=bytecode(*vertex.get()),original_pixel=bytecode(*pixel.get());
        auto patched=sm3::patch_pair(original_vertex,hit_distortion_?original_pixel:
            outlast_without_hit_distortion(original_pixel),
            {atlas_samplers,eye_camera,eye_view,scissor,viewport_transform,tracked_uniforms,ui_plane,points});
        sm3::TrackedPair tracking;
        if (tracked_uniforms) {
            tracking=sm3::track_uniforms(std::move(patched));
            patched=std::move(tracking.pair);
        }
        auto variant=std::make_unique<Variant>();
        *variant->original_vertex.put()=vertex.get();
        vertex->AddRef();
        *variant->original_pixel.put()=pixel.get();
        pixel->AddRef();
        const std::vector<DWORD> vertex_code(patched.vertex.begin(),patched.vertex.end());
        const std::vector<DWORD> pixel_code(patched.pixel.begin(),patched.pixel.end());
        gpu_check(device_->CreateVertexShader(vertex_code.data(),variant->vertex.put()),"Create joint native stereo vertex variant");
        const auto pixel_status=device_->CreatePixelShader(pixel_code.data(),variant->pixel.put());
        if (FAILED(pixel_status)) retain_rejection(original_vertex,original_pixel,patched,key);
        gpu_check(pixel_status,"Create joint native stereo pixel variant");
        StereoShaderBinding bindings{
            variant->vertex.get(),variant->pixel.get(),patched.input_semantic,patched.sampler,patched.guard};
        bindings.effect_constants=sampler_usage(*pixel.get()).effects;
        bindings.atlas_sampler_constants=patched.atlas_sampler_constants;
        bindings.atlas_point_samplers=patched.atlas_point_samplers;
        bindings.pixel_position_constant=patched.pixel_position_constant;
        bindings.camera_base_constant=patched.camera_base_constant;
        bindings.camera_delta_constant=patched.camera_delta_constant;
        bindings.scissor_constant=patched.scissor_constant;
        bindings.scissor_delta_constant=patched.scissor_delta_constant;
        bindings.viewport_constant=patched.viewport_constant;
        bindings.caller_scissor=scissor;
        bindings.ui_plane=ui_plane;
        bindings.vertex_texture=tracked_uniforms?tracking.vertex_texture:patched.view_texture;
        bindings.pixel_view_sampler=tracking.pixel_sampler;
        bindings.view_texture_width=tracking.texture_width;
        bindings.tracked_uniforms=std::move(tracking.uniforms);
        bindings.pixel_constant_rows=std::move(tracking.pixel_constant_rows);
        bindings.tracked_world_scissor=tracked_uniforms && patched.view_texture && scissor &&
            viewport_transform && !ui_plane;
        variant->pipeline=std::make_unique<D3D9StereoPipeline>(*device_.get(),bindings,state_cache_);
        auto* pipeline=variant->pipeline.get();
        cache_.emplace(key,std::move(variant));
        return *pipeline;
    }
    void release_device_resources() noexcept {
        for (auto& entry:cache_) entry.second->pipeline->release_device_resources();
        mono_cache_.clear();
        mono_point_atlas_.clear();
    }
    std::size_t size() const { return cache_.size(); }
};

} // namespace olvr
