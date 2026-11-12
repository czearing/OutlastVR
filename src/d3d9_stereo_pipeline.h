#pragma once

#include "d3d9_resource.h"
#include "d3d9_indexed_device.h"
#include "sm3_tracked_uniforms.h"
#include "d3d9_point_atlas.h"
#include <array>
#include <memory>

namespace olvr {

using StereoViewConstants = std::array<std::array<float, 32>, 2>;
struct StereoReconstructionContext;
struct StereoReconstructionCache;
class StateRestore;
struct D3D9StateCache;

struct StereoShaderBinding {
    IDirect3DVertexShader9* vertex;
    IDirect3DPixelShader9* pixel;
    UINT eye_input_semantic;
    UINT vertex_sampler;
    UINT pixel_guard_register;
    std::vector<std::pair<UINT,UINT>> atlas_sampler_constants{};
    int pixel_position_constant=-1,camera_base_constant=-1,camera_delta_constant=-1;
    int scissor_constant=-1;
    int scissor_delta_constant=-1;
    int viewport_constant=-1;
    bool caller_scissor=false;
    bool ui_plane=false;
    bool vertex_texture=true;
    int pixel_view_sampler=-1;
    UINT view_texture_width=8;
    std::vector<sm3::TrackedUniform> tracked_uniforms{};
    std::vector<sm3::TrackedPair::ConstantRow> pixel_constant_rows{};
    bool tracked_world_scissor=false;
    std::vector<std::pair<unsigned,unsigned>> atlas_point_samplers{};
};

// Requires a compatible transformed shader pair and an already bound stereo atlas.
// This is render-thread-owned; it does not classify game passes or install hooks.
class D3D9StereoPipeline {
public:
    D3D9StereoPipeline(IDirect3DDevice9& device, const StereoShaderBinding& shaders,
                      std::shared_ptr<D3D9StateCache> state_cache=nullptr);
    ~D3D9StereoPipeline();
    void set_reconstruction_context(const StereoReconstructionContext& context);
    void set_native_view_columns(bool enabled) noexcept {
        if (native_view_columns_!=enabled) view_records_valid_=false;
        native_view_columns_=enabled;
    }
    SubmissionResult draw_indexed(const Draw& draw, const StereoViewConstants& views,
                                  UINT software_repeat_count = 1,const RECT* logical_scissor = nullptr);
    SubmissionResult draw_non_indexed(UINT topology, UINT start_vertex, UINT primitive_count,
                                      const StereoViewConstants& views,const RECT* logical_scissor = nullptr);
    SubmissionResult draw_primitive_up(UINT topology, UINT primitive_count, const void* vertices,
                                      std::size_t vertex_bytes, UINT stride,
                                      const StereoViewConstants& views,const RECT* logical_scissor = nullptr);
    SubmissionResult draw_indexed_up(const Draw& draw, const void* indices, std::size_t index_bytes,
                                    D3DFORMAT format, const void* vertices, std::size_t vertex_bytes,
                                    UINT stride, const StereoViewConstants& views,const RECT* logical_scissor = nullptr);
    void release_device_resources() noexcept;
    std::uint64_t view_uploads() const noexcept {return view_uploads_;}
    std::uint64_t view_record_builds() const noexcept {return view_record_builds_;}
    std::uint64_t view_record_cache_hits() const noexcept {return view_record_cache_hits_;}
    std::uint64_t declaration_builds() const noexcept {return declaration_builds_;}
    const StereoShaderBinding& diagnostic_binding() const noexcept {return shaders_;}
    IDirect3DTexture9* diagnostic_view_texture() const noexcept {return views_.get();}
    void set_camera_directions_view_dependent(bool enabled) noexcept {
        if (camera_directions_view_dependent_!=enabled) view_records_valid_=false;
        camera_directions_view_dependent_=enabled;
    }
    bool camera_directions_view_dependent() const noexcept {return camera_directions_view_dependent_;}

private:
    Com<IDirect3DDevice9> device_;
    std::shared_ptr<D3D9StateCache> state_cache_;
    Com<IDirect3DVertexShader9> vertex_;
    Com<IDirect3DPixelShader9> pixel_;
    Com<IDirect3DTexture9> views_;
    Com<IDirect3DVertexBuffer9> eyes_;
    Com<IDirect3DVertexDeclaration9> base_declaration_;
    Com<IDirect3DVertexDeclaration9> eye_declaration_;
    Com<IDirect3DIndexBuffer9> converted_indices_;
    Com<IDirect3DVertexBuffer9> uploaded_vertices_;
    Com<IDirect3DStateBlock9> caller_state_;
    Com<IDirect3DVertexShader9> caller_vertex_;
    Com<IDirect3DPixelShader9> caller_pixel_;
    std::array<UINT,18> caller_samplers_{};
    UINT caller_sampler_count_=0;
    StereoShaderBinding shaders_;
    D3D9PointAtlasLookup point_atlas_;
    bool native_view_columns_=false;
    std::unique_ptr<StereoReconstructionContext> reconstruction_;
    std::unique_ptr<StereoReconstructionCache> reconstruction_cache_;
    std::vector<float> view_records_;
    std::vector<float> uploaded_view_records_;
    std::vector<std::array<float,16>> original_uniforms_;
    std::vector<std::array<float,16>> cached_original_uniforms_;
    StereoViewConstants cached_views_{};
    std::array<bool,16> occupied_streams_{};
    std::vector<Stream> streams_;
    bool view_records_valid_=false;
    std::uint64_t view_record_builds_=0,view_record_cache_hits_=0,declaration_builds_=0;
    std::uint64_t view_uploads_=0;
    bool uniform_evidence_written_=false;
    bool camera_directions_view_dependent_=true;
    D3DCAPS9 caps_{};
    UINT eye_capacity_ = 0;
    UINT cached_eye_stream_ = 0;
    UINT index_capacity_ = 0;
    UINT vertex_capacity_ = 0;
    D3DFORMAT index_format_ = D3DFMT_UNKNOWN;
    IndexedStereoPlan indexed_plan_{};
    UINT element_count(UINT topology, UINT primitive_count) const;
    void prepare_indices(const void* data, UINT count, D3DFORMAT format);
    void prepare_vertices(const void* data, std::size_t bytes, UINT stride);
    void prepare_declaration();
    IDirect3DStateBlock9& prepare_caller_state();
    SubmissionResult draw_indexed_preserving(const Draw& draw,const StereoViewConstants& views,
        UINT software_repeat_count,const RECT* logical_scissor,StateRestore& restore);
};

} // namespace olvr
