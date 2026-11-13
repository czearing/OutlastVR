#include "d3d9_stereo_pipeline.h"
#include "stereo_reconstruction.h"
#include "d3d9_state_restore.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include <fstream>
#include <filesystem>

namespace olvr {
struct StereoReconstructionCache {
    Matrix4 original_world_inverse{};
    StereoViewConstants views{};
    std::array<std::array<Matrix4,5>,2> transforms{};
    std::array<std::array<bool,5>,2> ready{};
    bool valid=false;
    void invalidate() noexcept {valid=false;}
    void locate(const StereoViewConstants& current) {
        if (valid && views==current) return;
        views=current;valid=true;
        for (auto& eye:ready) eye.fill(false);
    }
    const Matrix4& transform(const StereoReconstructionContext& context,unsigned eye,unsigned kind) {
        if (!ready[eye][kind]) {
            auto& value=transforms[eye][kind];
            if (kind==0) {
                const auto offset=context.original_camera.pretranslation;
                value=matrix_product(translation_matrix(scale(offset,-1)),
                    matrix_product(transform(context,eye,1),translation_matrix(offset)));
            } else if (kind==1) value=reconstruction_delta(context,views[eye],true);
            else if (kind==2) value=reconstruction_rotation(context,views[eye]);
            else if (kind==3) value=matrix_inverse(transform(context,eye,2));
            else value=screen_input_delta(context,views[eye]);
            ready[eye][kind]=true;
        }
        return transforms[eye][kind];
    }
};

D3D9StereoPipeline::D3D9StereoPipeline(IDirect3DDevice9& device, const StereoShaderBinding& shaders,
    std::shared_ptr<D3D9StateCache> state_cache)
    : state_cache_(state_cache?std::move(state_cache):std::make_shared<D3D9StateCache>()),
      shaders_(shaders),reconstruction_cache_(std::make_unique<StereoReconstructionCache>()) {
    if (!shaders.vertex || !shaders.pixel || shaders.eye_input_semantic >= 16 ||
        shaders.vertex_sampler >= 4 || shaders.pixel_guard_register >= 224 ||
        shaders.pixel_view_sampler>=16 || shaders.view_texture_width<8 ||
        shaders.view_texture_width>256 || (shaders.view_texture_width&(shaders.view_texture_width-1)))
        throw std::invalid_argument("Invalid paired stereo shader binding");
    *device_.put() = &device;
    device.AddRef();
    *vertex_.put() = shaders.vertex;
    shaders.vertex->AddRef();
    *pixel_.put() = shaders.pixel;
    shaders.pixel->AddRef();
    gpu_check(device.GetDeviceCaps(&caps_), "Query stereo device capabilities");
    if (caps_.VertexShaderVersion < D3DVS_VERSION(3, 0) ||
        caps_.PixelShaderVersion < D3DPS_VERSION(3, 0) ||
        !(caps_.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ||
        caps_.MaxStreams < 2 || caps_.MaxStreams > 16)
        throw std::runtime_error("Device cannot supply the required hardware stereo streams");
    Com<IDirect3D9> factory;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    D3DDISPLAYMODE display{};
    gpu_check(device.GetDirect3D(factory.put()), "Query stereo device factory");
    gpu_check(device.GetCreationParameters(&creation), "Query stereo device creation parameters");
    gpu_check(factory->GetAdapterDisplayMode(creation.AdapterOrdinal, &display), "Query adapter format");
    gpu_check(factory->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, display.Format,
                                        D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE,
                                        D3DFMT_A32B32G32R32F), "Check float32 stereo vertex texture");
    std::vector<std::size_t> order;
    for (std::size_t index=0;index<shaders_.tracked_uniforms.size();++index) {
        const auto& uniform=shaders_.tracked_uniforms[index];
        const auto limit=uniform.vertex?caps_.MaxVertexShaderConst:224u;
        if (!uniform.rows || uniform.rows>4 || uniform.original_register>=limit ||
            uniform.rows>limit-uniform.original_register || uniform.texture_row<8 ||
            uniform.texture_row>=shaders_.view_texture_width ||
            uniform.rows>shaders_.view_texture_width-uniform.texture_row ||
            (uniform.material_world_position &&
             (uniform.vertex || uniform.kind!=sm3::TrackedUniformKind::material_camera)))
            throw std::invalid_argument("Tracked shader requires bounded native reconstruction records");
        order.push_back(index);
    }
    std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
        const auto& left=shaders_.tracked_uniforms[a];
        const auto& right=shaders_.tracked_uniforms[b];
        if (left.vertex!=right.vertex) return left.vertex>right.vertex;
        return left.original_register<right.original_register;
    });
    for (const auto index:order) {
        const auto& uniform=shaders_.tracked_uniforms[index];
        if (uniform_reads_.empty() || uniform_reads_.back().vertex!=uniform.vertex ||
            uniform.original_register>uniform_reads_.back().first+uniform_reads_.back().rows)
            uniform_reads_.push_back({uniform.vertex,uniform.original_register,uniform.rows});
        else {
            auto& read=uniform_reads_.back();
            read.rows=std::max(read.rows,uniform.original_register+uniform.rows-read.first);
        }
    }
    std::size_t values=0;
    for (auto& read:uniform_reads_) {read.offset=values;values+=read.rows*4;}
    original_uniforms_.resize(values);
    for (const auto& uniform:shaders_.tracked_uniforms) {
        const auto read=std::find_if(uniform_reads_.begin(),uniform_reads_.end(),[&](const UniformRead& span) {
            return span.vertex==uniform.vertex && span.first<=uniform.original_register &&
                span.first+span.rows>=uniform.original_register+uniform.rows;
        });
        if (read==uniform_reads_.end()) throw std::logic_error("Native uniform is outside its read span");
        uniform_value_offsets_.push_back(read->offset+(uniform.original_register-read->first)*4);
    }
}

D3D9StereoPipeline::~D3D9StereoPipeline()=default;

void D3D9StereoPipeline::set_reconstruction_context(const StereoReconstructionContext& context) {
    if (reconstruction_) {
        const auto& a=reconstruction_->original_camera;
        const auto& b=context.original_camera;
        const auto same=[](Vec3 x,Vec3 y){return x.x==y.x && x.y==y.y && x.z==y.z;};
        if (reconstruction_->original_projection==context.original_projection &&
            same(a.origin,b.origin) && same(a.pretranslation,b.pretranslation) &&
            same(a.right,b.right) && same(a.up,b.up) && same(a.backward,b.backward) &&
            a.units_per_meter==b.units_per_meter) return;
    }
    const auto inverse=matrix_inverse(context.original_projection);
    if (!finite(context.original_camera.origin) || !finite(context.original_camera.pretranslation) ||
        !finite(context.original_camera.right) || !finite(context.original_camera.up) ||
        !finite(context.original_camera.backward))
        throw std::invalid_argument("Native reconstruction camera contains nonfinite values");
    if (reconstruction_) *reconstruction_=context;
    else reconstruction_=std::make_unique<StereoReconstructionContext>(context);
    reconstruction_cache_->original_world_inverse=matrix_product(
        translation_matrix(scale(context.original_camera.pretranslation,-1)),inverse);
    reconstruction_cache_->invalidate();
    view_records_valid_=false;
}

void D3D9StereoPipeline::release_device_resources() noexcept {
    point_atlas_.clear();
    caller_state_.reset();
    caller_vertex_.reset();caller_pixel_.reset();
    state_cache_->block.reset();
    eye_declaration_.reset();
    base_declaration_.reset();
    eyes_.reset();
    views_.reset();
    uploaded_view_records_.clear();
    view_records_valid_=false;
    converted_indices_.reset();
    uploaded_vertices_.reset();
    eye_capacity_ = 0;
    cached_eye_stream_ = 0;
    index_capacity_ = vertex_capacity_ = 0;
    sequential_index_count_ = 0;
    index_format_ = D3DFMT_UNKNOWN;
}

IDirect3DStateBlock9& D3D9StereoPipeline::prepare_caller_state() {
    Com<IDirect3DVertexShader9> native_vertex;Com<IDirect3DPixelShader9> native_pixel;
    gpu_check(device_->GetVertexShader(native_vertex.put()),"Identify precise caller vertex shader");
    gpu_check(device_->GetPixelShader(native_pixel.put()),"Identify precise caller pixel shader");
    if (caller_state_.get() && native_vertex.get()==caller_vertex_.get() && native_pixel.get()==caller_pixel_.get())
        return *caller_state_.get();
    if (state_cache_->active || (state_cache_->device && state_cache_->device!=device_.get()))
        throw std::logic_error("Cannot record caller-state mask inside another caller-state scope");
    std::vector<UINT> vertex_constants,pixel_constants{shaders_.pixel_guard_register},samplers;
    std::vector<UINT> vertex_integers,pixel_integers,vertex_booleans,pixel_booleans;
    auto constant=[](std::vector<UINT>& list,int index,UINT limit) {
        if (index<0 || UINT(index)>=limit) throw std::invalid_argument("Invalid caller-state constant mask");
        list.push_back(UINT(index));
    };
    auto literals=[&](auto* shader,std::vector<UINT>& floats,std::vector<UINT>& integers,
                      std::vector<UINT>& booleans,UINT float_limit) {
        if (!shader) return;
        UINT size=0;gpu_check(shader->GetFunction(nullptr,&size),"Read caller-state shader extent");
        if (size<8 || size%4 || size>1048576) throw std::invalid_argument("Invalid caller-state shader extent");
        sm3::Words words(size/4);
        const auto expected=size;
        gpu_check(shader->GetFunction(words.data(),&size),"Read exact caller-state shader");
        if (size!=expected) throw std::runtime_error("Caller-state shader changed during mask creation");
        for (const auto& instruction:sm3::parse(words)) {
            if (instruction.opcode==81) constant(floats,sm3::index(instruction.tokens.at(1)),float_limit);
            if (instruction.opcode==48) constant(integers,sm3::index(instruction.tokens.at(1)),16);
            if (instruction.opcode==47) constant(booleans,sm3::index(instruction.tokens.at(1)),16);
        }
    };
    // Shader binding can expose DEF/DEFI/DEFB registers through driver state too.
    for (auto* shader:{native_vertex.get(),vertex_.get()})
        literals(shader,vertex_constants,vertex_integers,vertex_booleans,caps_.MaxVertexShaderConst);
    for (auto* shader:{native_pixel.get(),pixel_.get()})
        literals(shader,pixel_constants,pixel_integers,pixel_booleans,224);
    if (shaders_.viewport_constant>=0)
        constant(vertex_constants,shaders_.viewport_constant,caps_.MaxVertexShaderConst);
    for (int index:{shaders_.pixel_position_constant,shaders_.scissor_constant,shaders_.scissor_delta_constant})
        if (index>=0) constant(pixel_constants,index,224);
    if (shaders_.camera_base_constant>=0) {
        constant(pixel_constants,shaders_.camera_base_constant,224);
        constant(pixel_constants,shaders_.camera_delta_constant,224);
    }
    for (const auto& entry:shaders_.atlas_sampler_constants) {
        if (entry.second>=224) throw std::invalid_argument("Invalid atlas caller-state constant mask");
        pixel_constants.push_back(entry.second);
    }
    for (const auto& row:shaders_.pixel_constant_rows) {
        constant(pixel_constants,row.left,224);
        constant(pixel_constants,row.right,224);
    }
    if (shaders_.vertex_texture) samplers.push_back(D3DVERTEXTEXTURESAMPLER0+shaders_.vertex_sampler);
    if (shaders_.pixel_view_sampler>=0) samplers.push_back(UINT(shaders_.pixel_view_sampler));
    for (const auto& entry:shaders_.atlas_point_samplers) samplers.push_back(entry.second);
    std::sort(samplers.begin(),samplers.end());
    samplers.erase(std::unique(samplers.begin(),samplers.end()),samplers.end());
    if (samplers.size()>caller_samplers_.size())
        throw std::invalid_argument("Stereo point selector exceeds the caller-state sampler mask");
    for (auto* list:{&vertex_constants,&pixel_constants,&vertex_integers,&pixel_integers,
                     &vertex_booleans,&pixel_booleans}) {
        std::sort(list->begin(),list->end());
        list->erase(std::unique(list->begin(),list->end()),list->end());
    }
    gpu_check(device_->BeginStateBlock(),"Begin precise stereo caller-state mask");
    HRESULT first=D3D_OK;
    auto record=[&](HRESULT status) {if (FAILED(status) && SUCCEEDED(first)) first=status;};
    record(device_->SetVertexShader(nullptr));
    record(device_->SetPixelShader(nullptr));
    record(device_->SetVertexDeclaration(nullptr));
    for (UINT stream=0;stream<caps_.MaxStreams;++stream) {
        record(device_->SetStreamSourceFreq(stream,1));
    }
    const float zero[4]{};
    for (auto index:vertex_constants) record(device_->SetVertexShaderConstantF(index,zero,1));
    for (auto index:pixel_constants) record(device_->SetPixelShaderConstantF(index,zero,1));
    const int zero_int[4]{};const BOOL zero_bool=FALSE;
    for (auto index:vertex_integers) record(device_->SetVertexShaderConstantI(index,zero_int,1));
    for (auto index:pixel_integers) record(device_->SetPixelShaderConstantI(index,zero_int,1));
    for (auto index:vertex_booleans) record(device_->SetVertexShaderConstantB(index,&zero_bool,1));
    for (auto index:pixel_booleans) record(device_->SetPixelShaderConstantB(index,&zero_bool,1));
    for (auto sampler:samplers) {
        for (auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            record(device_->SetSamplerState(sampler,filter,D3DTEXF_POINT));
        record(device_->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        record(device_->SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        record(device_->SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        record(device_->SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,FALSE));
    }
    if (shaders_.viewport_constant>=0) {
        const D3DVIEWPORT9 viewport{0,0,1,1,0,1};
        record(device_->SetViewport(&viewport));
    }
    if (shaders_.scissor_constant>=0)
        record(device_->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
    Com<IDirect3DStateBlock9> selected;
    const auto ended=device_->EndStateBlock(selected.put());
    gpu_check(first,"Record precise stereo caller-state mask");
    gpu_check(ended,"Finish precise stereo caller-state mask");
    caller_state_.reset();caller_vertex_.reset();caller_pixel_.reset();
    *caller_state_.put()=selected.get();selected->AddRef();
    if (native_vertex.get()) {*caller_vertex_.put()=native_vertex.get();native_vertex->AddRef();}
    if (native_pixel.get()) {*caller_pixel_.put()=native_pixel.get();native_pixel->AddRef();}
    caller_sampler_count_=UINT(samplers.size());
    std::copy(samplers.begin(),samplers.end(),caller_samplers_.begin());
    ++state_cache_->allocations;
    return *caller_state_.get();
}

UINT D3D9StereoPipeline::element_count(UINT topology, UINT primitive_count) const {
    if (!primitive_count || primitive_count > caps_.MaxPrimitiveCount)
        throw std::invalid_argument("Converted draw has an invalid primitive count");
    std::uint64_t count = primitive_count;
    if (topology == D3DPT_LINELIST)
        count *= 2;
    else if (topology == D3DPT_TRIANGLELIST)
        count *= 3;
    else if (topology == D3DPT_TRIANGLESTRIP)
        count += 2;
    else
        throw std::invalid_argument("Converted draw is not one of the recovered Outlast topologies");
    if (count > std::numeric_limits<UINT>::max() / sizeof(DWORD))
        throw std::invalid_argument("Converted index data exceeds the native buffer range");
    return static_cast<UINT>(count);
}

void D3D9StereoPipeline::prepare_indices(const void* data, UINT count, D3DFORMAT format) {
    if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)
        throw std::invalid_argument("Converted index format must be 16-bit or 32-bit");
    if (!data && format!=D3DFMT_INDEX32)
        throw std::invalid_argument("Generated indices require the 32-bit converted format");
    const auto bytes = count * (format == D3DFMT_INDEX16 ? sizeof(WORD) : sizeof(DWORD));
    if (!data && count<=sequential_index_count_) return;
    sequential_index_count_=0;
    if (!converted_indices_.get() || bytes > index_capacity_ || format != index_format_) {
        converted_indices_.reset();
        index_capacity_ = 0;
        gpu_check(device_->CreateIndexBuffer(static_cast<UINT>(bytes), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                              format, D3DPOOL_DEFAULT, converted_indices_.put(), nullptr),
                  "Create converted index buffer");
        index_capacity_ = static_cast<UINT>(bytes);
        index_format_ = format;
    }
    void* pointer = nullptr;
    gpu_check(converted_indices_->Lock(0, static_cast<UINT>(bytes), &pointer, D3DLOCK_DISCARD),
              "Lock converted index data");
    if (data)
        std::memcpy(pointer, data, bytes);
    else
        for (UINT index = 0; index < count; ++index)
            static_cast<DWORD*>(pointer)[index] = index;
    gpu_check(converted_indices_->Unlock(), "Unlock converted index data");
    if (!data) sequential_index_count_=count;
}

void D3D9StereoPipeline::prepare_declaration() {
    Com<IDirect3DVertexDeclaration9> declaration;
    gpu_check(device_->GetVertexDeclaration(declaration.put()), "Query original vertex declaration");
    if (!declaration.get())
        throw std::invalid_argument("Stereo pipeline requires an explicit vertex declaration");
    if (declaration.get()==base_declaration_.get()) return;
    UINT count = 0;
    gpu_check(declaration->GetDeclaration(nullptr, &count), "Query original declaration length");
    if (count < 2 || count >= MAXD3DDECLLENGTH)
        throw std::invalid_argument("Original declaration has no room for the eye element");
    std::vector<D3DVERTEXELEMENT9> elements(count);
    gpu_check(declaration->GetDeclaration(elements.data(), &count), "Read original vertex declaration");
    if (elements.back().Stream != 0xff || elements.back().Type != D3DDECLTYPE_UNUSED)
        throw std::invalid_argument("Original vertex declaration is not terminated");
    std::array<bool,16> occupied{};
    for (std::size_t index=0;index+1<elements.size();++index) {
        const auto& element=elements[index];
        if (element.Stream>=caps_.MaxStreams)
            throw std::invalid_argument("Original declaration references an unsupported stream");
        if (element.Usage==D3DDECLUSAGE_TEXCOORD && element.UsageIndex==shaders_.eye_input_semantic)
            throw std::invalid_argument("Original declaration conflicts with the paired eye input semantic");
        occupied[element.Stream]=true;
    }
    UINT eye_stream=1;
    while (eye_stream<caps_.MaxStreams && occupied[eye_stream]) ++eye_stream;
    if (eye_stream==caps_.MaxStreams || !occupied[0])
        throw std::invalid_argument("Original declaration has no supported spare eye stream");
    elements.back()={static_cast<WORD>(eye_stream),0,D3DDECLTYPE_FLOAT1,D3DDECLMETHOD_DEFAULT,
        D3DDECLUSAGE_TEXCOORD,static_cast<BYTE>(shaders_.eye_input_semantic)};
    const D3DVERTEXELEMENT9 end=D3DDECL_END();
    elements.push_back(end);
    Com<IDirect3DVertexDeclaration9> paired;
    gpu_check(device_->CreateVertexDeclaration(elements.data(),paired.put()),"Create paired eye declaration");
    eye_declaration_.reset();base_declaration_.reset();
    *eye_declaration_.put()=paired.get();paired->AddRef();
    *base_declaration_.put()=declaration.get();declaration->AddRef();
    occupied_streams_=occupied;
    cached_eye_stream_=eye_stream;
    ++declaration_builds_;
}

void D3D9StereoPipeline::prepare_vertices(const void* data, std::size_t bytes, UINT stride) {
    if (!data || !bytes || !stride || stride > caps_.MaxStreamStride ||
        bytes > std::numeric_limits<UINT>::max() || bytes % stride != 0)
        throw std::invalid_argument("Invalid bounded UP vertex span");
    prepare_declaration();
    if (std::any_of(occupied_streams_.begin()+1,occupied_streams_.end(),[](bool used){return used;}))
        throw std::invalid_argument("UP geometry must originate entirely in stream zero");
    if (!uploaded_vertices_.get() || bytes > vertex_capacity_) {
        uploaded_vertices_.reset();
        vertex_capacity_ = 0;
        gpu_check(device_->CreateVertexBuffer(static_cast<UINT>(bytes),
                                               D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                               D3DPOOL_DEFAULT, uploaded_vertices_.put(), nullptr),
                  "Create uploaded UP vertex buffer");
        vertex_capacity_ = static_cast<UINT>(bytes);
    }
    void* pointer = nullptr;
    gpu_check(uploaded_vertices_->Lock(0, static_cast<UINT>(bytes), &pointer, D3DLOCK_DISCARD),
              "Lock uploaded UP vertices");
    std::memcpy(pointer, data, bytes);
    gpu_check(uploaded_vertices_->Unlock(), "Unlock uploaded UP vertices");
}

SubmissionResult D3D9StereoPipeline::draw_non_indexed(
    UINT topology, UINT start_vertex, UINT primitive_count, const StereoViewConstants& views,
    const RECT* logical_scissor) {
    const auto count = element_count(topology, primitive_count);
    if (start_vertex > static_cast<UINT>(std::numeric_limits<INT>::max()))
        throw std::invalid_argument("Non-indexed start vertex exceeds the converted base-vertex range");
    prepare_indices(nullptr, count, D3DFMT_INDEX32);
    prepare_declaration();
    auto& caller=prepare_caller_state();
    StateRestore restore(*device_.get(),*state_cache_,caller,cached_eye_stream_,
        caller_samplers_,caller_sampler_count_,true);
    gpu_check(device_->SetIndices(converted_indices_.get()), "Bind generated geometry indices");
    return draw_indexed_preserving({topology, static_cast<INT>(start_vertex), 0, count, 0, primitive_count},
                                    views,1,logical_scissor,restore);
}

SubmissionResult D3D9StereoPipeline::draw_primitive_up(
    UINT topology, UINT primitive_count, const void* vertices, std::size_t vertex_bytes,
    UINT stride, const StereoViewConstants& views,const RECT* logical_scissor) {
    const auto count = element_count(topology, primitive_count);
    if (!stride || static_cast<std::uint64_t>(count) * stride > vertex_bytes)
        throw std::invalid_argument("UP vertex span is smaller than the recovered topology requires");
    prepare_vertices(vertices, vertex_bytes, stride);
    prepare_indices(nullptr, count, D3DFMT_INDEX32);
    auto& caller=prepare_caller_state();
    StateRestore restore(*device_.get(),*state_cache_,caller,cached_eye_stream_,
        caller_samplers_,caller_sampler_count_,true,true);
    gpu_check(device_->SetStreamSource(0, uploaded_vertices_.get(), 0, stride), "Bind uploaded UP vertices");
    gpu_check(device_->SetStreamSourceFreq(0, 1), "Reset UP geometry frequency");
    gpu_check(device_->SetIndices(converted_indices_.get()), "Bind generated UP indices");
    auto result = draw_indexed_preserving({topology, 0, 0, count, 0, primitive_count},
                                          views,1,logical_scissor,restore);
    // Native UP clears stream zero, while the recovered RHI resets its frequency.
    for (const auto cleanup : {device_->SetStreamSource(0, nullptr, 0, 0),
                               device_->SetStreamSourceFreq(0, 1)})
        if (FAILED(cleanup) && result.restoration_status >= 0)
            result.restoration_status = cleanup;
    return result;
}

SubmissionResult D3D9StereoPipeline::draw_indexed_up(
    const Draw& draw, const void* indices, std::size_t index_bytes, D3DFORMAT format,
    const void* vertices, std::size_t vertex_bytes, UINT stride, const StereoViewConstants& views,
    const RECT* logical_scissor) {
    const auto count = element_count(draw.primitive_type, draw.primitive_count);
    if (!indices || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32) ||
        draw.base_vertex != 0 || draw.start_index != 0 || !draw.vertex_count || !stride)
        throw std::invalid_argument("Invalid indexed-UP geometry contract");
    const auto width = format == D3DFMT_INDEX16 ? sizeof(WORD) : sizeof(DWORD);
    const auto end = static_cast<std::uint64_t>(draw.min_vertex) + draw.vertex_count;
    if (static_cast<std::uint64_t>(count) * width > index_bytes ||
        end > std::numeric_limits<UINT>::max() || end * stride > vertex_bytes)
        throw std::invalid_argument("Indexed-UP data span does not cover the declared vertex range");
    for (UINT index = 0; index < count; ++index) {
        DWORD vertex = 0;
        std::memcpy(&vertex, static_cast<const char*>(indices) + index * width, width);
        if (vertex < draw.min_vertex || vertex >= end)
            throw std::invalid_argument("Indexed-UP index is outside the declared vertex range");
    }
    prepare_vertices(vertices, vertex_bytes, stride);
    prepare_indices(indices, count, format);
    auto& caller=prepare_caller_state();
    StateRestore restore(*device_.get(),*state_cache_,caller,cached_eye_stream_,
        caller_samplers_,caller_sampler_count_,true,true);
    gpu_check(device_->SetStreamSource(0, uploaded_vertices_.get(), 0, stride), "Bind indexed-UP vertices");
    gpu_check(device_->SetStreamSourceFreq(0, 1), "Reset indexed-UP geometry frequency");
    gpu_check(device_->SetIndices(converted_indices_.get()), "Bind uploaded UP indices");
    auto result = draw_indexed_preserving(draw,views,1,logical_scissor,restore);
    for (const auto cleanup : {device_->SetStreamSource(0, nullptr, 0, 0),
                               device_->SetStreamSourceFreq(0, 1), device_->SetIndices(nullptr)})
        if (FAILED(cleanup) && result.restoration_status >= 0)
            result.restoration_status = cleanup;
    return result;
}

SubmissionResult D3D9StereoPipeline::draw_indexed(const Draw& draw, const StereoViewConstants& views,
                                                  UINT software_repeat_count,const RECT* logical_scissor) {
    prepare_declaration();
    auto& caller=prepare_caller_state();
    StateRestore restore(*device_.get(),*state_cache_,caller,cached_eye_stream_,
        caller_samplers_,caller_sampler_count_);
    return draw_indexed_preserving(draw,views,software_repeat_count,logical_scissor,restore);
}

SubmissionResult D3D9StereoPipeline::draw_indexed_preserving(const Draw& draw,
    const StereoViewConstants& views,UINT software_repeat_count,const RECT* logical_scissor,
    StateRestore& restore) {
    if (!draw.vertex_count || !draw.primitive_count ||
        (draw.primitive_type != 2 && draw.primitive_type != 4 && draw.primitive_type != 5))
        throw std::invalid_argument("Stereo pipeline requires nonempty supported indexed geometry");
    if (device_->GetSoftwareVertexProcessing())
        throw std::runtime_error("Stereo hardware instancing cannot use software vertex processing");
    D3DVIEWPORT9 viewport{};
    gpu_check(device_->GetViewport(&viewport), "Query stereo atlas viewport");
    if (viewport.Width < 2 || viewport.Width % 2 != 0 || viewport.Height == 0)
        throw std::invalid_argument("Stereo atlas viewport must contain two equal-width eyes");
    const auto eye_stream=cached_eye_stream_;
    auto& streams=streams_;
    streams.clear();
    UINT object_instances = 1;
    for (UINT slot = 0; slot < caps_.MaxStreams; ++slot) {
        if (!occupied_streams_[slot])
            continue;
        UINT frequency = 0;
        gpu_check(device_->GetStreamSourceFreq(slot, &frequency), "Query original stream frequency");
        const auto flags = frequency & ~frequency_mask;
        const auto divisor = frequency & frequency_mask;
        if (slot == 0 && flags == indexed_data && divisor != 0) {
            object_instances = divisor;
            streams.push_back({slot, StreamRate::vertex});
        } else if (slot != 0 && flags == instance_data && divisor != 0) {
            streams.push_back({slot, StreamRate::object_instance, divisor});
        } else if (frequency == 1) {
            streams.push_back({slot, StreamRate::vertex});
        } else {
            throw std::invalid_argument("Original stream frequency cannot be represented without replay");
        }
    }
    auto& plan=indexed_plan_;
    plan_indexed_stereo(DrawKind::indexed, true, software_repeat_count,
        object_instances, caps_.MaxStreams, eye_stream, streams,plan);
    if (plan.eye_records > std::numeric_limits<UINT>::max() / sizeof(float))
        throw std::invalid_argument("Stereo eye-buffer extent exceeds the native allocation range");
    if (!eyes_.get() || eye_capacity_ < plan.eye_records) {
        eyes_.reset();
        eye_capacity_ = 0;
        gpu_check(device_->CreateVertexBuffer(plan.eye_records * sizeof(float),
                                               D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                               D3DPOOL_DEFAULT, eyes_.put(), nullptr), "Create stereo eye buffer");
        void* pointer = nullptr;
        gpu_check(eyes_->Lock(0, 0, &pointer, D3DLOCK_DISCARD), "Lock stereo eye records");
        for (UINT index = 0; index < plan.eye_records; ++index)
            static_cast<float*>(pointer)[index] = static_cast<float>(index & 1);
        gpu_check(eyes_->Unlock(), "Unlock stereo eye records");
        eye_capacity_ = plan.eye_records;
    }
    auto& records=view_records_;
    if (reconstruction_) reconstruction_cache_->locate(views);
    if (!uniform_evidence_written_ && !shaders_.tracked_uniforms.empty()) {
        wchar_t folder[32768]{};
        const auto length=GetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",folder,UINT(std::size(folder)));
        if (length) {
            if (length>=std::size(folder) || !reconstruction_)
                throw std::runtime_error("Invalid tracked uniform evidence context");
            std::ofstream file(std::filesystem::path(std::wstring(folder,length))/L"tracked-uniforms.jsonl",std::ios::app);
            file<<"{\"projection\":[";
            for (unsigned i=0;i<16;++i) file<<(i?",":"")<<reconstruction_->original_projection[i];
            file<<"],\"uniforms\":[";
            bool separator=false;
            for (const auto& uniform:shaders_.tracked_uniforms) {
                float original[16]{};
                gpu_check(uniform.vertex?
                    device_->GetVertexShaderConstantF(uniform.original_register,original,uniform.rows):
                    device_->GetPixelShaderConstantF(uniform.original_register,original,uniform.rows),
                    "Read native tracked uniform evidence");
                file<<(separator?",":"")<<"{\"vertex\":"<<(uniform.vertex?"true":"false")
                    <<",\"kind\":"<<int(uniform.kind)<<",\"register\":"<<uniform.original_register
                    <<",\"rows\":"<<uniform.rows<<",\"column_major\":"<<(uniform.column_major?"true":"false")
                    <<",\"values\":[";
                for (unsigned i=0;i<uniform.rows*4;++i) file<<(i?",":"")<<original[i];
                file<<"]}";
                separator=true;
            }
            file<<"]}\n";
            file.close();
            if (!file) throw std::runtime_error("Native tracked uniform evidence write failed");
        }
        uniform_evidence_written_=true;
    }
    if (!uniform_reads_.empty() && !reconstruction_)
        throw std::invalid_argument("Tracked shader requires bounded native reconstruction records");
    for (const auto& read:uniform_reads_) {
        auto* values=original_uniforms_.data()+read.offset;
        gpu_check(read.vertex?
            device_->GetVertexShaderConstantF(read.first,values,read.rows):
            device_->GetPixelShaderConstantF(read.first,values,read.rows),
            "Read contiguous original native reconstruction uniforms");
    }
    if (!view_records_valid_ || cached_views_!=views || cached_original_uniforms_!=original_uniforms_) {
        for (const auto value:original_uniforms_)
            if (!std::isfinite(value))
                throw std::invalid_argument("Native reconstruction uniform contains nonfinite values");
        records.resize(shaders_.view_texture_width*4*2);
        std::fill(records.begin(),records.end(),0);
        for (UINT eye=0;eye<2;++eye) {
            auto* output=records.data()+eye*shaders_.view_texture_width*4;
            std::copy(views[eye].begin(),views[eye].end(),output);
            for (std::size_t uniform_index=0;uniform_index<shaders_.tracked_uniforms.size();++uniform_index) {
                const auto& uniform=shaders_.tracked_uniforms[uniform_index];
                if (!reconstruction_ || uniform.texture_row<8 ||
                    uniform.texture_row+uniform.rows>shaders_.view_texture_width ||
                    !uniform.rows || uniform.rows>4)
                    throw std::invalid_argument("Tracked shader requires a valid native camera and bounded uniform records");
                float original[16]{};
                std::copy_n(original_uniforms_.data()+uniform_value_offsets_[uniform_index],uniform.rows*4,original);
                if (uniform.kind==sm3::TrackedUniformKind::material_camera) {
                    if (!uniform.material_world_position)
                        for (unsigned axis=0;axis<3;++axis)
                            original[axis]=views[eye][16+axis]-views[eye][20+axis];
                    std::copy_n(original,4,output+uniform.texture_row*4);
                } else if (uniform.kind==sm3::TrackedUniformKind::camera_direction) {
                    const auto& rotation=reconstruction_cache_->transform(*reconstruction_,eye,2);
                    auto* value=output+uniform.texture_row*4;
                    std::copy_n(original,4,value);
                    for (unsigned row=0;camera_directions_view_dependent_ && row<3;++row) {
                        value[row]=0;
                        for (unsigned column=0;column<3;++column)
                            value[row]+=rotation[row*4+column]*original[column];
                    }
                } else if (uniform.kind==sm3::TrackedUniformKind::camera_world ||
                    uniform.kind==sm3::TrackedUniformKind::camera_translated) {
                    const auto& frame=reconstruction_->original_camera;
                    const float center[]{frame.origin.x+frame.pretranslation.x,
                        frame.origin.y+frame.pretranslation.y,frame.origin.z+frame.pretranslation.z};
                    for (unsigned axis=0;axis<3;++axis) original[axis]+=views[eye][16+axis]-center[axis];
                    std::copy_n(original,4,output+uniform.texture_row*4);
                } else {
                    // Native upload/ray evidence establishes matrix * vector here.
                    // CTAB then identifies whether its registers hold columns.
                    const bool columns=native_view_columns_ && uniform.column_major;
                    const auto transpose=[&](Matrix4& matrix) {
                        if (!columns) return;
                        const unsigned size=uniform.kind==sm3::TrackedUniformKind::view_to_world ||
                            uniform.kind==sm3::TrackedUniformKind::world_to_view?3:4;
                        for (unsigned row=0;row<size;++row) for (unsigned column=row+1;column<size;++column)
                            std::swap(matrix[row*4+column],matrix[column*4+row]);
                    };
                    auto matrix=identity_matrix();
                    if (uniform.kind==sm3::TrackedUniformKind::inverse_projective_world && uniform.rows==3) {
                        matrix=reconstruction_cache_->original_world_inverse;
                        transpose(matrix);
                        for (unsigned index=0;index<12;++index)
                            if (std::abs(matrix[index]-original[index])>.002f+
                                16*std::numeric_limits<float>::epsilon()*std::abs(matrix[index]))
                                throw std::invalid_argument("Pruned inverse projection differs from the verified native camera");
                    }
                    std::copy_n(original,uniform.rows*4,matrix.begin());
                    transpose(matrix);
                    Matrix4 value{};
                    if (uniform.kind==sm3::TrackedUniformKind::material_projection ||
                        uniform.kind==sm3::TrackedUniformKind::translated_projection) {
                        std::copy_n(views[eye].begin(),16,value.begin());
                        if (uniform.kind==sm3::TrackedUniformKind::material_projection) {
                            const auto camera=std::find_if(shaders_.tracked_uniforms.begin(),
                                shaders_.tracked_uniforms.end(),[&](const sm3::TrackedUniform& binding) {
                                    return binding.vertex==uniform.vertex && binding.material_world_position;
                                });
                            if (camera==shaders_.tracked_uniforms.end())
                                throw std::invalid_argument("Material projection lacks its native world offset");
                            const auto* offset=original_uniforms_.data()+
                                uniform_value_offsets_[std::size_t(camera-shaders_.tracked_uniforms.begin())];
                            for (unsigned row=0;row<4;++row) {
                                // Fold the camera rebase into projection, not unrelated material reads.
                                double translated=value[row*4+3];
                                for (unsigned axis=0;axis<3;++axis)
                                    translated+=double(value[row*4+axis])*
                                        (double(offset[axis])+double(views[eye][20+axis]));
                                if (!std::isfinite(translated) ||
                                    std::abs(translated)>std::numeric_limits<float>::max())
                                    throw std::overflow_error("Material projection translation is nonfinite");
                                value[row*4+3]=static_cast<float>(translated);
                            }
                        }
                    } else if (uniform.kind==sm3::TrackedUniformKind::screen_to_light)
                        value=matrix_product(matrix,reconstruction_cache_->transform(*reconstruction_,eye,4));
                    else if (uniform.kind==sm3::TrackedUniformKind::view_to_world)
                        value=matrix_product(reconstruction_cache_->transform(*reconstruction_,eye,2),matrix);
                    else if (uniform.kind==sm3::TrackedUniformKind::world_to_view)
                        value=matrix_product(matrix,reconstruction_cache_->transform(*reconstruction_,eye,3));
                    else
                        value=matrix_product(reconstruction_cache_->transform(*reconstruction_,eye,
                            uniform.kind==sm3::TrackedUniformKind::inverse_translated ||
                            (native_view_columns_ && uniform.rows==4 &&
                             uniform.kind==sm3::TrackedUniformKind::inverse_world &&
                             screen_reconstruction_translated(*reconstruction_,matrix))?1:0),matrix);
                    transpose(value);
                    std::copy_n(value.begin(),uniform.rows*4,output+uniform.texture_row*4);
                }
            }
            if (native_view_columns_ && !shaders_.ui_plane) {
                for (unsigned row=0;row<4;++row) for (unsigned column=row+1;column<4;++column)
                    std::swap(output[row*4+column],output[column*4+row]);
            }
        }
        cached_views_=views;
        cached_original_uniforms_=original_uniforms_;
        view_records_valid_=true;
        ++view_record_builds_;
    } else ++view_record_cache_hits_;
    if (!views_.get())
        gpu_check(device_->CreateTexture(shaders_.view_texture_width, 2, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED,
                                         views_.put(), nullptr), "Create paired view texture");
    if (uploaded_view_records_!=records) {
        D3DLOCKED_RECT locked{};
        gpu_check(views_->LockRect(0, &locked, nullptr, 0), "Lock paired view constants");
        for (UINT eye = 0; eye < 2; ++eye)
            std::memcpy(static_cast<char*>(locked.pBits) + eye * locked.Pitch,
                        records.data()+eye*shaders_.view_texture_width*4,
                        shaders_.view_texture_width*4*sizeof(float));
        gpu_check(views_->UnlockRect(0), "Unlock paired view constants");
        uploaded_view_records_=records;
        ++view_uploads_;
    }

    D3DVIEWPORT9 atlas_viewport=viewport;
    RECT viewport_rectangle{};
    if (shaders_.viewport_constant>=0) {
        if (viewport.X%2) throw std::invalid_argument("Native atlas viewport X must preserve logical integer coordinates");
        Com<IDirect3DSurface9> target;
        gpu_check(device_->GetRenderTarget(0,target.put()),"Get native atlas viewport target");
        D3DSURFACE_DESC description{};
        gpu_check(target->GetDesc(&description),"Read native atlas viewport target extent");
        if (!description.Width || description.Width%2 || !description.Height ||
            std::uint64_t(viewport.X)+viewport.Width>description.Width ||
            std::uint64_t(viewport.Y)+viewport.Height>description.Height)
            throw std::invalid_argument("Native stereo viewport exceeds its actual target");
        const float target_width=static_cast<float>(description.Width);
        const float target_height=static_cast<float>(description.Height);
        float native_viewport[]{
            static_cast<float>(viewport.Width)/target_width*.5f,
            static_cast<float>(viewport.X)/target_width+
                static_cast<float>(viewport.Width)/target_width*.5f-1,
            static_cast<float>(viewport.Height)/target_height,
            1-(static_cast<float>(viewport.Y)*2+static_cast<float>(viewport.Height))/target_height};
        if (shaders_.ui_plane) {
            const float placement[]{.5f,-.5f,1,0};
            std::copy_n(placement,4,native_viewport);
        }
        gpu_check(device_->SetVertexShaderConstantF(shaders_.viewport_constant,native_viewport,1),
                  "Bind exact native viewport placement in both target halves");
        viewport_rectangle={static_cast<LONG>(viewport.X/2),static_cast<LONG>(viewport.Y),
            static_cast<LONG>((viewport.X+viewport.Width)/2),static_cast<LONG>(viewport.Y+viewport.Height)};
        atlas_viewport={0,0,description.Width,description.Height,viewport.MinZ,viewport.MaxZ};
        gpu_check(device_->SetViewport(&atlas_viewport),"Bind full target for disjoint native eye viewports");
    }
    gpu_check(device_->SetVertexDeclaration(eye_declaration_.get()), "Bind paired eye declaration");
    gpu_check(device_->SetStreamSource(eye_stream, eyes_.get(), 0, sizeof(float)), "Bind paired eye stream");
    gpu_check(device_->SetVertexShader(vertex_.get()), "Bind stereo vertex shader");
    gpu_check(device_->SetPixelShader(pixel_.get()), "Bind stereo pixel shader");
    pixel_constant_upload_.resize(shaders_.pixel_constant_rows.size()*8);
    UINT constant_start=0,constant_count=0;
    const auto flush_constants=[&] {
        if (constant_count)
            gpu_check(device_->SetPixelShaderConstantF(constant_start,pixel_constant_upload_.data(),constant_count),
                "Bind contiguous per-eye material constants without texture reads");
        constant_count=0;
    };
    const auto append_constant=[&](UINT index,const float* value) {
        if (constant_count && index!=constant_start+constant_count) flush_constants();
        if (!constant_count) constant_start=index;
        std::copy_n(value,4,pixel_constant_upload_.data()+constant_count*4);
        ++constant_count;
    };
    for (const auto& row:shaders_.pixel_constant_rows) {
        if (row.texture_row>=shaders_.view_texture_width || row.left>=224 || row.right>=224)
            throw std::invalid_argument("Invalid per-eye material constant binding");
        append_constant(row.left,records.data()+row.texture_row*4);
        append_constant(row.right,records.data()+(shaders_.view_texture_width+row.texture_row)*4);
    }
    flush_constants();
    std::array<UINT,2> view_samplers{};
    unsigned view_sampler_count=0;
    if (shaders_.vertex_texture) view_samplers[view_sampler_count++]=D3DVERTEXTEXTURESAMPLER0+shaders_.vertex_sampler;
    if (shaders_.pixel_view_sampler>=0) view_samplers[view_sampler_count++]=static_cast<UINT>(shaders_.pixel_view_sampler);
    for (unsigned index=0;index<view_sampler_count;++index) {
        const auto sampler=view_samplers[index];
        gpu_check(device_->SetTexture(sampler, views_.get()), "Bind paired view texture");
        for (const auto filter : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER})
            gpu_check(device_->SetSamplerState(sampler, filter, D3DTEXF_POINT), "Set paired view point filtering");
        gpu_check(device_->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "Disable view mip filtering");
        gpu_check(device_->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP), "Clamp view texture U");
        gpu_check(device_->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP), "Clamp view texture V");
        gpu_check(device_->SetSamplerState(sampler, D3DSAMP_SRGBTEXTURE, FALSE), "Disable view texture gamma conversion");
    }
    const float guard[]{static_cast<float>(atlas_viewport.X) + static_cast<float>(atlas_viewport.Width) / 2 - .5f, 2, -1, 0};
    point_atlas_.bind(*device_.get(),shaders_.atlas_point_samplers);
    gpu_check(device_->SetPixelShaderConstantF(shaders_.pixel_guard_register, guard, 1), "Bind atlas seam guard");
    for (const auto& entry:shaders_.atlas_sampler_constants) {
        Com<IDirect3DBaseTexture9> texture;
        gpu_check(device_->GetTexture(entry.first,texture.put()),"Read role-selected atlas sampler");
        if (!texture.get() || texture->GetType()!=D3DRTYPE_TEXTURE)
            throw std::runtime_error("Role-selected atlas sampler lacks a 2D texture");
        auto* atlas=static_cast<IDirect3DTexture9*>(texture.get());
        D3DSURFACE_DESC description{};
        gpu_check(atlas->GetLevelDesc(0,&description),"Read role-selected atlas extent");
        if (description.Width<2 || description.Width%2)
            throw std::runtime_error("Role-selected atlas sampler lacks two equal-width eyes");
        const float texel=.5f/static_cast<float>(description.Width);
        const float binding[]{.5f,.5f,texel,.5f-texel};
        gpu_check(device_->SetPixelShaderConstantF(entry.second,binding,1),"Bind eye-isolated atlas sampling");
    }
    if (shaders_.pixel_position_constant>=0) {
        float position[]{static_cast<float>(atlas_viewport.Width)/2,static_cast<float>(atlas_viewport.X)/2,0,0};
        if (shaders_.ui_plane) {
            position[0]=float(viewport.Width)/4;
            position[1]=float(viewport.X)/2+position[0];
            position[2]=-float(viewport.Height)/2;
            position[3]=float(viewport.Y)+float(viewport.Height)/2;
        }
        gpu_check(device_->SetPixelShaderConstantF(shaders_.pixel_position_constant,position,1),"Bind eye-local pixel coordinates");
    }
    if (shaders_.scissor_constant>=0) {
        std::array<RECT,2> rectangles{};
        if (shaders_.viewport_constant>=0) rectangles={viewport_rectangle,viewport_rectangle};
        if (shaders_.caller_scissor) {
            RECT caller{};
            if (logical_scissor) caller=*logical_scissor;
            else gpu_check(device_->GetScissorRect(&caller),"Read logical caller scissor");
            if (caller.left>caller.right || caller.top>caller.bottom)
                throw std::invalid_argument("Stereo caller scissor has invalid half-open bounds");
            std::array<RECT,2> callers{caller,caller};
            if (shaders_.tracked_world_scissor) {
                if (!reconstruction_ || shaders_.viewport_constant<0 || shaders_.ui_plane ||
                    shaders_.scissor_delta_constant<0)
                    throw std::invalid_argument("Tracked world scissor lacks its native camera or viewport");
                const std::array<Matrix4,2> transforms{
                    reconstruction_cache_->transform(*reconstruction_,0,4),
                    reconstruction_cache_->transform(*reconstruction_,1,4)};
                callers=stereo_world_scissors(*reconstruction_,transforms,caller,viewport_rectangle);
            }
            for (unsigned eye=0;eye<2;++eye) {
                auto& rectangle=rectangles[eye];
                caller=callers[eye];
                if (shaders_.viewport_constant>=0) {
                    rectangle.left=std::max(rectangle.left,caller.left);
                    rectangle.top=std::max(rectangle.top,caller.top);
                    rectangle.right=std::min(rectangle.right,caller.right);
                    rectangle.bottom=std::min(rectangle.bottom,caller.bottom);
                    if (rectangle.left>rectangle.right) rectangle.right=rectangle.left;
                    if (rectangle.top>rectangle.bottom) rectangle.bottom=rectangle.top;
                } else rectangle=caller;
            }
        }
        float bounds[2][4]{};
        for (unsigned eye=0;eye<2;++eye) {
            const auto& rectangle=rectangles[eye];
            if (rectangle.left>rectangle.right || rectangle.top>rectangle.bottom)
                throw std::invalid_argument("Stereo caller scissor has invalid half-open bounds");
            bounds[eye][0]=static_cast<float>(rectangle.left);
            bounds[eye][1]=static_cast<float>(rectangle.top);
            bounds[eye][2]=static_cast<float>(rectangle.right)-.5f;
            bounds[eye][3]=static_cast<float>(rectangle.bottom)-.5f;
        }
        gpu_check(device_->SetPixelShaderConstantF(shaders_.scissor_constant,bounds[0],1),
                  "Bind per-eye caller scissor");
        if (shaders_.scissor_delta_constant>=0) {
            for (unsigned axis=0;axis<4;++axis) bounds[1][axis]-=bounds[0][axis];
            gpu_check(device_->SetPixelShaderConstantF(shaders_.scissor_delta_constant,bounds[1],1),
                      "Bind right-eye caller scissor difference");
        }
        gpu_check(device_->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE),
                  "Replace single-rectangle hardware scissor with per-eye pixel clipping");
    }
    if (shaders_.camera_base_constant>=0) {
        float center[4]{},delta[4]{};
        for (UINT axis=0;axis<3;++axis) {
            const float left=views[0][16+axis]-views[0][20+axis];
            const float right=views[1][16+axis]-views[1][20+axis];
            center[axis]=(left+right)*.5f;
            delta[axis]=(right-left)*.5f;
        }
        center[3]=1;
        gpu_check(device_->SetPixelShaderConstantF(shaders_.camera_base_constant,center,1),"Bind tracked pixel camera center");
        gpu_check(device_->SetPixelShaderConstantF(shaders_.camera_delta_constant,delta,1),"Bind per-eye pixel camera separation");
    }
    D3D9IndexedDevice native(*device_.get());
    auto result = submit_indexed_stereo(native, plan, draw);
    const auto status = restore.apply();
    if (FAILED(status) && result.restoration_status >= 0)
        result.restoration_status = status;
    return result;
}

} // namespace olvr
