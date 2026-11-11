#pragma once
#include "d3d9_stereo_pipeline.h"
#include "d3d9_shader_variants.h"
#include "sha256.h"
#include "stereo_reconstruction.h"
#include "native_pixel_delta.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <climits>

namespace olvr {

inline bool native_glass_reference_sample(std::uint64_t world,bool temporal,unsigned stride=1) {
    if (stride!=1 && stride!=20)
        throw std::invalid_argument("Unknown original-material readback cadence");
    return world>=320 && world<=420 && (world-320)%(temporal?stride:20)==0;
}

inline RECT native_glass_reference_eye_scissor(const D3DVIEWPORT9& viewport,
                                               UINT eye_width,unsigned eye,
                                               const RECT* native=nullptr) {
    if (eye>1 || !eye_width || eye_width>UINT(LONG_MAX)/2 ||
        viewport.X%2 || !viewport.Width || viewport.Width%2 ||
        std::uint64_t(viewport.X)+viewport.Width>std::uint64_t(eye_width)*2 ||
        std::uint64_t(viewport.Y)+viewport.Height>LONG_MAX ||
        (native && (native->left>native->right || native->top>native->bottom)))
        throw std::invalid_argument("Invalid original-glass reference eye bounds");
    const LONG offset=LONG(eye*eye_width);
    RECT result{LONG(viewport.X/2),LONG(viewport.Y),
        LONG((viewport.X+viewport.Width)/2),LONG(viewport.Y+viewport.Height)};
    if (native) {
        result.left=std::clamp(native->left,result.left,result.right);
        result.top=std::clamp(native->top,result.top,result.bottom);
        result.right=std::clamp(native->right,result.left,result.right);
        result.bottom=std::clamp(native->bottom,result.top,result.bottom);
    }
    result.left+=offset;result.right+=offset;
    return result;
}

inline std::array<float,4> native_glass_reference_eye_mapping(const D3DVIEWPORT9& viewport,
                                                            UINT eye_width,unsigned eye) {
    native_glass_reference_eye_scissor(viewport,eye_width,eye);
    const float scale=float(viewport.Width)/(float(eye_width)*4);
    return {scale,float(viewport.X)/(float(eye_width)*2)+scale-1+eye,0,0};
}

class NativeGlassReferenceProbe {
    std::filesystem::path folder_;
    std::ofstream trace_;
    std::uint64_t frame_;
    bool sweep_;
    bool temporal_;
    unsigned material_sample_stride_=1;
    bool reflective_;
    bool contribution_;
    bool particle_;
    unsigned samples_=0;
    unsigned temporal_samples_=0;
    std::ofstream temporal_trace_;
    std::set<std::string> temporal_programs_;
    using WorldFrame=BOOL(WINAPI*)(UINT64*);
    WorldFrame world_frame_=nullptr;
    static void reference_layout(std::ostream& out,const D3DVIEWPORT9& viewport,UINT eye_width,
                                 bool scissor,const std::array<RECT,2>& tracked,unsigned stride) {
        out<<",\"material_readback_stride\":"<<stride
            <<",\"reference_eye_ownership_version\":1,\"reference_source_viewport\":["
            <<viewport.X<<','<<viewport.Y<<','<<viewport.Width<<','<<viewport.Height
            <<"],\"reference_native_scissor_enabled\":"<<(scissor?"true":"false")
            <<",\"reference_tracked_scissors\":[";
        for (unsigned eye=0;eye<2;++eye) {
            const auto& bounds=tracked[eye];
            out<<(eye?",[":"[")<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom<<']';
        }
        out<<"],\"reference_eye_scissors\":[";
        for (unsigned eye=0;eye<2;++eye) {
            const auto bounds=native_glass_reference_eye_scissor(viewport,eye_width,eye,scissor?&tracked[eye]:nullptr);
            out<<(eye?",[":"[")<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom<<']';
        }
        out<<"],\"reference_raster_mappings\":[";
        for (unsigned eye=0;eye<2;++eye) {
            const auto mapping=native_glass_reference_eye_mapping(viewport,eye_width,eye);
            out<<(eye?",[":"[")<<mapping[0]<<','<<mapping[1]<<",0,0]";
        }
        out<<']';
    }
    static sm3::Words raster_vertex(const sm3::Words& original,unsigned& constant) {
        const auto instructions=sm3::parse(original);
        const auto info=sm3::inspect(instructions);
        const auto temporaries=info.free(0,32),constants=info.free(2,256);
        unsigned position=0,positions=0,writes=0;
        for (const auto& declaration:info.declarations)
            if (declaration.type==6 && declaration.usage==0 && declaration.semantic==0) {
                position=declaration.number;++positions;
            }
        if (positions!=1 || temporaries.empty() || constants.empty())
            throw std::runtime_error("Original glass lacks an isolated raster-control register layout");
        constant=constants.front();
        const auto temporary=temporaries.front();
        sm3::Words result{original.front()};
        for (const auto& instruction:instructions) {
            auto tokens=instruction.tokens;
            if (!sm3::header(instruction.opcode) && tokens.size()>1 &&
                sm3::kind(tokens[1])==6 && sm3::index(tokens[1])==position) {
                tokens[1]=sm3::replace(tokens[1],0,temporary);++writes;
            }
            sm3::append(result,tokens);
        }
        if (!writes) throw std::runtime_error("Original glass has no position write to isolate");
        sm3::op(result,5,{sm3::destination(0,temporary,1),sm3::source(0,temporary,0),
                         sm3::source(2,constant,0)});
        sm3::op(result,4,{sm3::destination(0,temporary,1),sm3::source(0,temporary,0xff),
                         sm3::source(2,constant,0x55),sm3::source(0,temporary,0)});
        sm3::op(result,1,{sm3::destination(6,position),sm3::source(0,temporary)});
        result.push_back(65535);
        return result;
    }
    template<class T> static sm3::Words code(T& shader) {
        UINT bytes=0;
        gpu_check(shader.GetFunction(nullptr,&bytes),"Read native glass shader extent");
        if (bytes<8 || bytes%4 || bytes>1048576)
            throw std::runtime_error("Invalid original glass shader extent");
        sm3::Words result(bytes/4);
        const auto expected=bytes;
        gpu_check(shader.GetFunction(result.data(),&bytes),"Read original glass shader");
        if (bytes!=expected) throw std::runtime_error("Original glass shader extent changed");
        return result;
    }
    static std::vector<char> read(IDirect3DDevice9& device,IDirect3DSurface9& surface) {
        D3DSURFACE_DESC extent{};
        gpu_check(surface.GetDesc(&extent),"Inspect actual glass HDR target");
        if (extent.Format!=D3DFMT_A16B16G16R16F)
            throw std::runtime_error("Glass reference requires unchanged half-float scene color");
        Com<IDirect3DSurface9> staging;
        gpu_check(device.CreateOffscreenPlainSurface(extent.Width,extent.Height,extent.Format,
            D3DPOOL_SYSTEMMEM,staging.put(),nullptr),"Create glass HDR readback");
        gpu_check(device.GetRenderTargetData(&surface,staging.get()),"Read exact native glass pixels");
        D3DLOCKED_RECT lock{};
        gpu_check(staging->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize glass HDR pixels");
        std::vector<char> pixels(std::size_t(extent.Width)*extent.Height*8);
        for (UINT y=0;y<extent.Height;++y)
            std::memcpy(pixels.data()+std::size_t(y)*extent.Width*8,
                static_cast<const char*>(lock.pBits)+y*lock.Pitch,std::size_t(extent.Width)*8);
        gpu_check(staging->UnlockRect(),"Unlock glass HDR pixels");
        return pixels;
    }
    void save(const std::string& name,const void* data,std::size_t bytes) {
        std::ofstream file(folder_/name,std::ios::binary);
        file.write(static_cast<const char*>(data),std::streamsize(bytes));file.close();
        if (!file) throw std::runtime_error("Cannot retain original glass reference evidence");
    }
    void save_pixels(const std::string& prefix,const char* role,const std::vector<char>& pixels,
                     const std::vector<char>& before) {
        const auto name=prefix+"_"+role+".rgba16f";
        if (!sweep_ || std::strcmp(role,"before")==0) {
            save(name,pixels.data(),pixels.size());
            return;
        }
        std::ofstream file(folder_/(name+".delta"),std::ios::binary);
        write_native_pixel_delta(file,before,pixels);
        file.close();
        if (!file) throw std::runtime_error("Cannot complete lossless native glass evidence");
    }
    void temporal_sample(std::uint64_t frame,UINT64 gameplay,const D3DSURFACE_DESC& extent,
                         const sm3::Words& vertex,const sm3::Words& pixel,const sm3::Words& raster,
                         unsigned raster_constant,const std::vector<char>& reference,
                         const std::vector<char>& actual,const SubmissionResult& submission,
                         const std::vector<char>& before,const std::vector<char>& coverage,
                         const sm3::Words& coverage_program,const std::array<float,16>& local_to_world,
                         const std::array<float,12>& particle_originals,
                         const std::array<std::array<float,12>,2>& particle_eyes,
                         const std::array<float,80>& particle_context,
                         const sm3::Words& actual_vertex,const sm3::Words& actual_pixel,
                         const std::vector<float>& actual_views,const StereoShaderBinding& binding,
                         DWORD actual_samples,bool camera_view_dependent,
                         const D3DVIEWPORT9& viewport,bool scissor,const std::array<RECT,2>& tracked_scissors) {
        const auto retain_program=[&](const sm3::Words& program) {
            const auto hash=sha256_bytes(program.data(),program.size()*4);
            if (temporal_programs_.insert(hash).second)
                save("temporal-program-"+hash+".bin",program.data(),program.size()*4);
            return hash;
        };
        const auto vertex_hash=retain_program(vertex),pixel_hash=retain_program(pixel);
        const auto raster_hash=retain_program(raster);
        const auto prefix="temporal_"+std::to_string(temporal_samples_++);
        std::ostringstream difference(std::ios::binary|std::ios::out);
        write_native_pixel_xor(difference,reference,actual);
        const auto bytes=difference.str();
        save(prefix+".xor.delta",bytes.data(),bytes.size());
        const bool equal=reference==actual;
        const bool retain_mismatch=!equal && (!particle_ || (gameplay-320)%20==0);
        if (retain_mismatch) {
            save(prefix+"_actual.rgba16f",actual.data(),actual.size());
            save(prefix+"_raster.rgba16f",reference.data(),reference.size());
        }
        temporal_trace_<<"{\"frame\":"<<frame<<",\"gameplay_frame\":"<<gameplay
            <<",\"sample\":"<<temporal_samples_-1<<",\"width\":"<<extent.Width
            <<",\"height\":"<<extent.Height<<",\"decoded_xor_bytes\":"<<actual.size()
            <<",\"xor_image\":"<<std::quoted(prefix+".xor.delta")
            <<",\"xor_payload_sha256\":"<<std::quoted(sha256_bytes(bytes.data(),bytes.size()))
            <<",\"actual_sha256\":"<<std::quoted(sha256_bytes(actual.data(),actual.size()))
            <<",\"raster_sha256\":"<<std::quoted(sha256_bytes(reference.data(),reference.size()))
            <<",\"vertex_sha256\":"<<std::quoted(vertex_hash)
            <<",\"pixel_sha256\":"<<std::quoted(pixel_hash)
            <<",\"raster_vertex_sha256\":"<<std::quoted(raster_hash)
            <<",\"raster_constant\":"<<raster_constant
            <<",\"byte_exact\":"<<(equal?"true":"false")
            <<",\"original_retail_pixel_shader\":true,\"native_depth_and_stencil_read_only\":true"
            <<",\"diagnostic_reference_eye_draws\":2,\"production_geometry_submissions\":"
            <<submission.native_draw_attempts;
        reference_layout(temporal_trace_,viewport,extent.Width/2,scissor,tracked_scissors,material_sample_stride_);
        if (contribution_) {
            const auto retain_delta=[&](const char* suffix,const std::vector<char>& baseline,
                                        const std::vector<char>& pixels,bool xor_difference) {
                std::ostringstream output(std::ios::binary|std::ios::out);
                write_native_pixel_runs(output,baseline,pixels,xor_difference);
                const auto payload=output.str(),name=prefix+suffix;
                save(name,payload.data(),payload.size());
                return std::make_pair(name,sha256_bytes(payload.data(),payload.size()));
            };
            const auto contribution=retain_delta(".contribution.xor.delta",before,actual,true);
            const auto flat=retain_delta(".coverage.rgba16f.delta",
                std::vector<char>(coverage.size()),coverage,false);
            const auto matrix=prefix+".local-to-world.f32";
            save(matrix,local_to_world.data(),sizeof(local_to_world));
            temporal_trace_<<",\"material_contribution_fixture_version\":"<<(particle_?3:1)
                <<",\"before_sha256\":"<<std::quoted(sha256_bytes(before.data(),before.size()))
                <<",\"contribution_xor\":"<<std::quoted(contribution.first)
                <<",\"contribution_payload_sha256\":"<<std::quoted(contribution.second)
                <<",\"coverage_image\":"<<std::quoted(flat.first)
                <<",\"coverage_payload_sha256\":"<<std::quoted(flat.second)
                <<",\"coverage_program_sha256\":"<<std::quoted(retain_program(coverage_program))
                <<",\"local_to_world_file\":"<<std::quoted(matrix)
                <<",\"local_to_world_sha256\":"
                <<std::quoted(sha256_bytes(local_to_world.data(),sizeof(local_to_world)))
                <<",\"coverage_eye_draws\":2,\"coverage_color\":[0.25,0.5,0.75,1]"
                <<",\"coverage_native_cull_blend_depth_scissor_retained\":true"
                <<",\"coverage_target_cleared_without_clearing_depth\":true";
            if (particle_) {
                const auto uploaded_name=prefix+".particle-uploaded-views.f32";
                save(uploaded_name,actual_views.data(),actual_views.size()*sizeof(float));
                temporal_trace_<<",\"actual_vertex_sha256\":"<<std::quoted(retain_program(actual_vertex))
                    <<",\"actual_pixel_sha256\":"<<std::quoted(retain_program(actual_pixel))
                    <<",\"actual_depth_passing_samples\":"<<actual_samples
                    <<",\"particle_camera_view_dependent\":"
                    <<(camera_view_dependent?"true":"false")
                    <<",\"particle_uploaded_views\":"<<std::quoted(uploaded_name)
                    <<",\"particle_uploaded_views_sha256\":"
                    <<std::quoted(sha256_bytes(actual_views.data(),actual_views.size()*sizeof(float)))
                    <<",\"particle_view_texture_width\":"<<binding.view_texture_width
                    <<",\"particle_uploaded_uniforms\":[";
                for (std::size_t index=0;index<binding.tracked_uniforms.size();++index) {
                    const auto& uniform=binding.tracked_uniforms[index];
                    temporal_trace_<<(index?",":"")<<"{\"vertex\":"<<(uniform.vertex?"true":"false")
                        <<",\"register\":"<<uniform.original_register<<",\"row\":"<<uniform.texture_row
                        <<",\"rows\":"<<uniform.rows<<",\"kind\":"<<int(uniform.kind)<<'}';
                }
                temporal_trace_<<']';
                const auto context_name=prefix+".particle-camera-context.f32";
                save(context_name,particle_context.data(),sizeof(particle_context));
                const auto original_name=prefix+".particle-camera-originals.f32";
                save(original_name,particle_originals.data(),sizeof(particle_originals));
                temporal_trace_<<",\"particle_camera_context\":"<<std::quoted(context_name)
                    <<",\"particle_camera_context_sha256\":"
                    <<std::quoted(sha256_bytes(particle_context.data(),sizeof(particle_context)))
                    <<",\"particle_camera_originals\":"<<std::quoted(original_name)
                    <<",\"particle_camera_originals_sha256\":"
                    <<std::quoted(sha256_bytes(particle_originals.data(),sizeof(particle_originals)))
                    <<",\"particle_camera_reference_files\":[";
                for (unsigned eye=0;eye<2;++eye) {
                    const auto name=prefix+".particle-camera-eye-"+std::to_string(eye)+".f32";
                    save(name,particle_eyes[eye].data(),sizeof(particle_eyes[eye]));
                    temporal_trace_<<(eye?",":"")<<std::quoted(name);
                }
                temporal_trace_<<"],\"particle_camera_reference_sha256\":[";
                for (unsigned eye=0;eye<2;++eye)
                    temporal_trace_<<(eye?",":"")<<std::quoted(sha256_bytes(
                        particle_eyes[eye].data(),sizeof(particle_eyes[eye])));
                temporal_trace_<<"],\"native_particle_camera_reference\":true"
                    <<",\"reference_eye_seams_isolated\":true"
                    <<",\"mismatch_full_images_retained\":"<<(retain_mismatch?"true":"false");
            }
        }
        temporal_trace_<<"}\n";
        temporal_trace_.flush();
        if (!temporal_trace_) throw std::runtime_error("Cannot retain continuous native glass evidence");
    }
public:
    NativeGlassReferenceProbe(const std::filesystem::path& evidence,std::uint64_t frame,bool sweep=false,
                              bool temporal=false,bool reflective=false,bool contribution=false,bool particle=false)
        :frame_(frame),sweep_(sweep),temporal_(temporal),reflective_(reflective),
         contribution_(reflective || contribution || particle),particle_(particle) {
        if (temporal && !sweep)
            throw std::invalid_argument("Continuous native glass requires the original six-pose sweep");
        if (contribution_ && !temporal)
            throw std::invalid_argument("Material contribution requires continuous native glass");
        if (particle && reflective)
            throw std::invalid_argument("Particle and reflective references require separate fixtures");
        wchar_t combined[2]{};
        const auto combined_length=GetEnvironmentVariableW(L"OLVR_PROBE_MATERIAL_BODY_CAMERA",combined,2);
        if (combined_length) {
            if (combined_length!=1 || combined[0]!=L'1' || !temporal || !sweep)
                throw std::invalid_argument("Combined material cadence requires its complete explicit fixture");
            material_sample_stride_=20;
        }
        folder_=evidence/L"native-glass-reference";
        if (!frame || !std::filesystem::create_directory(folder_))
            throw std::runtime_error("Native glass reference requires a fixed frame and new directory");
        trace_.open(folder_/L"references.jsonl");
        if (!trace_) throw std::runtime_error("Cannot create original glass reference trace");
        trace_<<std::setprecision(9);
        if (temporal_) {
            temporal_trace_.open(folder_/L"temporal-references.jsonl");
            if (!temporal_trace_) throw std::runtime_error("Cannot create continuous native glass trace");
            temporal_trace_<<std::setprecision(9);
        }
    }
    bool matches(IDirect3DDevice9& device,std::uint64_t frame) {
        if (sweep_) {
            if (!world_frame_) {
                const auto module=GetModuleHandleW(L"OutlastVRHeadlessProbe.dll");
                const auto address=module?GetProcAddress(module,"OutlastVRProbe_GlassWorldFrameV2"):nullptr;
                if (!address) throw std::runtime_error("Missing versioned headless glass gameplay-frame source");
                static_assert(sizeof(world_frame_)==sizeof(address));
                std::memcpy(&world_frame_,&address,sizeof(world_frame_));
            }
            UINT64 world=0;
            if (!world_frame_(&world)) throw std::runtime_error("Headless glass gameplay-frame source refused");
            if (!native_glass_reference_sample(world,temporal_,material_sample_stride_)) return false;
        } else if (frame!=frame_) return false;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.GetPixelShader(pixel.put()),"Identify actual window glass shader");
        if (!pixel.get()) return false;
        const auto words=code(*pixel.get());
        return sha256_bytes(words.data(),words.size()*4)==(particle_?
            "aaf6160e56e1c3e86778a144f66760f0e24a21e25fc4498d120cb7180967601f":reflective_?
            "7427e7e3f1dc0ebfd5e7604d9450026354b027e940472aa10422c6ed8faa46e8":
            "b9f4a2c34e11ece75a7bca31c9be4c12bf2a9913c55dc45af4b0c639886a4ce8");
    }
    template<class NativeDraw,class StereoDraw>
    SubmissionResult compare(IDirect3DDevice9& device,std::uint64_t frame,const StereoViewConstants& eyes,
                             const StereoReconstructionContext& context,
                             NativeDraw native_draw,StereoDraw stereo_draw,D3D9StereoPipeline& pipeline) {
        UINT64 gameplay_frame=0;
        if (sweep_ && (!world_frame_ || !world_frame_(&gameplay_frame)))
            throw std::runtime_error("Cannot bind glass evidence to its actual gameplay frame");
        Com<IDirect3DSurface9> output,depth;
        gpu_check(device.GetRenderTarget(0,output.put()),"Retain actual glass output");
        gpu_check(device.GetDepthStencilSurface(depth.put()),"Retain actual native glass depth");
        OutlastAtlasTag tag{};
        if (!atlas_tag(*output.get(),tag) || tag.role!=OutlastTargetRole::scene_color)
            throw std::runtime_error("Original glass reference lost its actual scene atlas");
        D3DSURFACE_DESC extent{};
        gpu_check(output->GetDesc(&extent),"Inspect native glass atlas");
        if (extent.Width!=tag.logical_width*2 || extent.Height!=tag.logical_height)
            throw std::runtime_error("Original glass reference has an invalid atlas extent");
        DWORD zwrite=0,stencil=0;
        gpu_check(device.GetRenderState(D3DRS_ZWRITEENABLE,&zwrite),"Check read-only native glass depth");
        gpu_check(device.GetRenderState(D3DRS_STENCILENABLE,&stencil),"Check read-only native glass stencil");
        if (zwrite || stencil)
            throw std::runtime_error("Glass reference cannot modify the caller depth or stencil");
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.GetVertexShader(vertex.put()),"Retain original glass vertex program");
        gpu_check(device.GetPixelShader(pixel.put()),"Retain original glass pixel program");
        if (!vertex.get() || !pixel.get()) throw std::runtime_error("Original glass program missing");
        const auto vs=code(*vertex.get()),ps=code(*pixel.get());
        const auto info=sm3::inspect(sm3::parse(vs));
        if (!info.view_projection) throw std::runtime_error("Glass reference lacks the recovered native projection");
        std::array<float,12> pixel_parameters{};
        if (particle_)
            gpu_check(device.GetPixelShaderConstantF(0,pixel_parameters.data(),3),"Retain native particle pixel parameters");
        std::array<float,16> local_to_world{};
        std::array<float,12> particle_originals{};
        std::array<std::array<float,12>,2> particle_eyes{};
        std::array<float,80> particle_context{};
        std::array<unsigned,3> particle_registers{};
        if (particle_) {
            const std::array<const char*,3> names{{"CameraWorldPosition","CameraRight","CameraUp"}};
            for (unsigned index=0;index<names.size();++index) {
                unsigned found=0;
                for (const auto& constant:info.constants) if (constant.set==2 && constant.name==names[index]) {
                    if (constant.count!=1) throw std::runtime_error("Native particle camera register extent changed");
                    particle_registers[index]=constant.number;++found;
                    gpu_check(device.GetVertexShaderConstantF(constant.number,particle_originals.data()+index*4,1),
                        "Retain native particle camera parameter");
                }
                if (found!=1) throw std::runtime_error("Native particle camera parameter is missing or ambiguous");
            }
            const auto& frame_camera=context.original_camera;
            const std::array<Vec3,5> native_vectors{{frame_camera.right,frame_camera.up,frame_camera.backward,
                frame_camera.origin,frame_camera.pretranslation}};
            for (unsigned index=0;index<native_vectors.size();++index) {
                particle_context[index*3]=native_vectors[index].x;
                particle_context[index*3+1]=native_vectors[index].y;
                particle_context[index*3+2]=native_vectors[index].z;
            }
            particle_context[15]=frame_camera.units_per_meter;
            for (unsigned eye=0;eye<2;++eye)
                std::copy(eyes[eye].begin(),eyes[eye].end(),particle_context.begin()+16+eye*32);
            const float center[]{frame_camera.origin.x+frame_camera.pretranslation.x,
                frame_camera.origin.y+frame_camera.pretranslation.y,frame_camera.origin.z+frame_camera.pretranslation.z};
            for (unsigned eye=0;eye<2;++eye) {
                particle_eyes[eye]=particle_originals;
                for (unsigned axis=0;axis<3;++axis)
                    particle_eyes[eye][axis]+=eyes[eye][16+axis]-center[axis];
                const auto rotation=reconstruction_rotation(context,eyes[eye]);
                for (unsigned vector=1;pipeline.camera_directions_view_dependent() && vector<3;++vector)
                    for (unsigned row=0;row<3;++row) {
                    float value=0;
                    for (unsigned column=0;column<3;++column)
                        value+=rotation[row*4+column]*particle_originals[vector*4+column];
                    particle_eyes[eye][vector*4+row]=value;
                }
            }
        }
        if (contribution_) {
            unsigned matrices=0;
            for (const auto& constant:info.constants) if (constant.name=="LocalToWorld" && constant.set==2) {
                if (constant.count!=4) throw std::runtime_error("Reflective glass LocalToWorld layout changed");
                gpu_check(device.GetVertexShaderConstantF(constant.number,local_to_world.data(),4),
                    "Retain reflective material world transform");++matrices;
            }
            if (matrices!=1) throw std::runtime_error("Reflective glass requires one native LocalToWorld");
            for (const auto& expected:std::array<std::pair<D3DRENDERSTATETYPE,DWORD>,8>{{
                {D3DRS_ALPHABLENDENABLE,TRUE},{D3DRS_SRCBLEND,D3DBLEND_SRCALPHA},
                {D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA},{D3DRS_BLENDOP,D3DBLENDOP_ADD},
                {D3DRS_COLORWRITEENABLE,15},{D3DRS_ALPHATESTENABLE,FALSE},
                {D3DRS_SRGBWRITEENABLE,FALSE},{D3DRS_ZENABLE,D3DZB_TRUE}}}) {
                DWORD actual=0;
                gpu_check(device.GetRenderState(expected.first,&actual),"Validate reflective coverage render state");
                if (actual!=expected.second) throw std::runtime_error("Reflective coverage native state changed");
            }
        }
        const bool full_sample=!temporal_ || (gameplay_frame-320)%20==0;
        const auto before=(full_sample || contribution_)?read(device,*output.get()):std::vector<char>{};
        StateRestore state(device);
        D3DCAPS9 caps{};gpu_check(device.GetDeviceCaps(&caps),"Inspect original glass MRT slots");
        std::array<Com<IDirect3DSurface9>,4> targets;
        const auto slots=std::min<UINT>(caps.NumSimultaneousRTs,4);
        for (UINT slot=0;slot<slots;++slot) {
            const auto status=device.GetRenderTarget(slot,targets[slot].put());
            if (status!=D3DERR_NOTFOUND) gpu_check(status,"Retain original glass MRT");
        }
        const auto restore=[&] {
            gpu_check(device.SetRenderTarget(0,targets[0].get()),"Restore native glass target");
            for (UINT slot=1;slot<slots;++slot)
                gpu_check(device.SetRenderTarget(slot,targets[slot].get()),"Restore native glass MRT");
            gpu_check(device.SetDepthStencilSurface(depth.get()),"Restore actual native glass depth");
            gpu_check(state.apply(),"Restore original glass caller state");
        };
        Com<IDirect3DTexture9> reference;
        Com<IDirect3DSurface9> reference_surface;
        gpu_check(device.CreateTexture(extent.Width,extent.Height,1,D3DUSAGE_RENDERTARGET,
            extent.Format,D3DPOOL_DEFAULT,reference.put(),nullptr),"Create private original-glass reference");
        gpu_check(reference->GetSurfaceLevel(0,reference_surface.put()),"Get original-glass reference surface");
        std::array<Com<IDirect3DTexture9>,2> sources;
        D3DVIEWPORT9 viewport{};
        gpu_check(device.GetViewport(&viewport),"Read physical native glass viewport");
        if (viewport.Width%2 || viewport.X%2)
            throw std::runtime_error("Glass reference viewport is not an exact eye atlas");
        RECT scissor{};
        gpu_check(device.GetScissorRect(&scissor),"Read native glass scissor");
        DWORD scissor_enabled=0;
        gpu_check(device.GetRenderState(D3DRS_SCISSORTESTENABLE,&scissor_enabled),"Read native glass scissor policy");
        std::array<RECT,2> eye_scissors{scissor,scissor};
        if (scissor_enabled) {
            const RECT logical_viewport{LONG(viewport.X/2),LONG(viewport.Y),
                LONG((viewport.X+viewport.Width)/2),LONG(viewport.Y+viewport.Height)};
            const std::array<Matrix4,2> transforms{
                screen_input_delta(context,eyes[0]),screen_input_delta(context,eyes[1])};
            eye_scissors=stereo_world_scissors(context,transforms,scissor,logical_viewport);
        }
        std::vector<char> expected;
        std::vector<char> legacy_scissor_control;
        std::vector<char> raster_control;
        std::vector<char> coverage;
        sm3::Words raster_code;
        sm3::Words coverage_code;
        unsigned raster_constant=0;
        try {
            Com<IDirect3DVertexShader9> raster_shader;
            Com<IDirect3DPixelShader9> coverage_shader;
            if (sweep_) {
                raster_code=raster_vertex(vs,raster_constant);
                const std::vector<DWORD> driver_code(raster_code.begin(),raster_code.end());
                gpu_check(device.CreateVertexShader(driver_code.data(),raster_shader.put()),
                    "Create position-only native glass raster control");
            }
            if (contribution_) {
                coverage_code={0xffff0300};
                sm3::op(coverage_code,81,{sm3::destination(2,0),0x3e800000,0x3f000000,0x3f400000,0x3f800000});
                sm3::op(coverage_code,1,{sm3::destination(8,0),sm3::source(2,0)});
                coverage_code.push_back(65535);
                const std::vector<DWORD> driver_code(coverage_code.begin(),coverage_code.end());
                gpu_check(device.CreatePixelShader(driver_code.data(),coverage_shader.put()),
                    "Create explicit flat-color native-depth coverage control");
            }
            Com<IDirect3DBaseTexture9> bound;
            gpu_check(device.GetTexture(0,bound.put()),"Read native glass scene-color source");
            if (!bound.get() || bound->GetType()!=D3DRTYPE_TEXTURE)
                throw std::runtime_error("Native glass scene-color source is not a 2D texture");
            auto* texture=static_cast<IDirect3DTexture9*>(bound.get());
            OutlastAtlasTag source_tag{};
            D3DSURFACE_DESC source_extent{};
            gpu_check(texture->GetLevelDesc(0,&source_extent),"Inspect real glass source atlas");
            if (!atlas_tag(*texture,source_tag) || texture->GetLevelCount()!=1 ||
                source_extent.Width!=source_tag.logical_width*2 ||
                source_extent.Height!=source_tag.logical_height)
                throw std::runtime_error("Native glass scene-color source lacks its exact atlas identity");
            Com<IDirect3DSurface9> source;
            gpu_check(texture->GetSurfaceLevel(0,source.put()),"Get actual glass source surface");
            for (UINT eye=0;eye<2;++eye) {
                gpu_check(device.CreateTexture(source_tag.logical_width,source_tag.logical_height,1,
                    D3DUSAGE_RENDERTARGET,source_extent.Format,D3DPOOL_DEFAULT,sources[eye].put(),nullptr),
                    "Create unmodified-format original glass eye source");
                Com<IDirect3DSurface9> crop;
                gpu_check(sources[eye]->GetSurfaceLevel(0,crop.put()),"Get original glass eye source");
                const RECT region{LONG(eye*source_tag.logical_width),0,
                    LONG((eye+1)*source_tag.logical_width),LONG(source_tag.logical_height)};
                gpu_check(device.StretchRect(source.get(),&region,crop.get(),nullptr,D3DTEXF_NONE),
                    "Copy exact original glass source pixels without atlas shader rewriting");
            }
            gpu_check(device.StretchRect(output.get(),nullptr,reference_surface.get(),nullptr,D3DTEXF_NONE),
                "Seed identical original-glass caller pixels");
            for (UINT slot=1;slot<slots;++slot)
                gpu_check(device.SetRenderTarget(slot,nullptr),"Detach reference glass MRT");
            gpu_check(device.SetRenderTarget(0,reference_surface.get()),"Bind private original-glass reference");
            const auto draw_eyes=[&](bool tracked_scissor,bool atlas_raster=false,bool flat=false) {
                gpu_check(device.SetVertexShader(atlas_raster?raster_shader.get():vertex.get()),
                    "Select original or position-only control vertex program");
                gpu_check(device.SetPixelShader(flat?coverage_shader.get():pixel.get()),
                    "Select original material or explicit diagnostic coverage program");
                gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,
                    scissor_enabled || atlas_raster),"Isolate reference eye ownership without changing native depth");
                for (UINT eye=0;eye<2;++eye) {
                    auto view=viewport;
                    if (!atlas_raster) {view.Width/=2;view.X=view.X/2+eye*tag.logical_width;}
                    else {view.X=0;view.Width=extent.Width;}
                    gpu_check(device.SetViewport(&view),"Select isolated original-shader eye viewport");
                    if (atlas_raster) {
                        const auto mapping=native_glass_reference_eye_mapping(viewport,tag.logical_width,eye);
                        gpu_check(device.SetVertexShaderConstantF(raster_constant,mapping.data(),1),
                            "Map only native output position into the full atlas viewport");
                    }
                    if (scissor_enabled || atlas_raster) {
                        auto native=tracked_scissor?eye_scissors[eye]:scissor;
                        auto region=atlas_raster?native_glass_reference_eye_scissor(
                            viewport,tag.logical_width,eye,scissor_enabled?&native:nullptr):native;
                        if (!atlas_raster) {
                            region.left+=LONG(eye*tag.logical_width);
                            region.right+=LONG(eye*tag.logical_width);
                        }
                        gpu_check(device.SetScissorRect(&region),"Preserve per-eye original glass scissor");
                    }
                    std::array<float,16> columns{};
                    for (UINT row=0;row<4;++row) for (UINT column=0;column<4;++column)
                        columns[row*4+column]=eyes[eye][column*4+row];
                    gpu_check(device.SetVertexShaderConstantF(0,columns.data(),4),"Bind ordinary native eye projection");
                    for (const auto& constant:info.constants) {
                        if (constant.name=="CameraPositionVS" && constant.set==2) {
                            if (constant.number!=4 || constant.count!=1)
                                throw std::runtime_error("Original glass camera layout changed");
                            gpu_check(device.SetVertexShaderConstantF(4,eyes[eye].data()+16,1),
                                "Bind ordinary native eye camera");
                        }
                    }
                    if (particle_) for (unsigned index=0;index<particle_registers.size();++index)
                        gpu_check(device.SetVertexShaderConstantF(particle_registers[index],
                            particle_eyes[eye].data()+index*4,1),"Bind native per-eye particle camera parameter");
                    gpu_check(device.SetTexture(0,sources[eye].get()),"Bind isolated original glass source");
                    gpu_check(native_draw(),"Execute original retail glass program against actual native depth");
                }
            };
            if (full_sample) {
                draw_eyes(true);
                expected=read(device,*reference_surface.get());
                gpu_check(device.StretchRect(output.get(),nullptr,reference_surface.get(),nullptr,D3DTEXF_NONE),
                    "Seed explicit untracked-scissor negative control");
                draw_eyes(false);
                legacy_scissor_control=read(device,*reference_surface.get());
            }
            if (sweep_) {
                gpu_check(device.StretchRect(output.get(),nullptr,reference_surface.get(),nullptr,D3DTEXF_NONE),
                    "Seed isolated full-viewport original-pixel-shader control");
                draw_eyes(true,true);
                raster_control=read(device,*reference_surface.get());
            }
            if (contribution_) {
                gpu_check(device.ColorFill(reference_surface.get(),nullptr,0),
                    "Clear only the private geometry-coverage color target");
                draw_eyes(true,true,true);
                coverage=read(device,*reference_surface.get());
            }
        } catch (...) {restore();throw;}
        restore();
        Com<IDirect3DQuery9> actual_coverage;
        DWORD actual_samples=0;
        if (particle_) {
            std::array<float,12> restored{};
            gpu_check(device.GetPixelShaderConstantF(0,restored.data(),3),"Verify restored particle pixel parameters");
            if (restored!=pixel_parameters) throw std::runtime_error("Particle reference changed native pixel parameters");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_OCCLUSION,actual_coverage.put()),
                "Create explicit actual particle coverage observation");
            gpu_check(actual_coverage->Issue(D3DISSUE_BEGIN),"Begin actual particle coverage observation");
        }
        const auto result=stereo_draw();
        if (particle_) gpu_check(actual_coverage->Issue(D3DISSUE_END),"End actual particle coverage observation");
        if (!result.succeeded()) throw std::runtime_error("Actual single-pass glass draw failed");
        const auto after=read(device,*output.get());
        if (particle_) {
            const auto deadline=GetTickCount64()+2000;
            HRESULT status=S_FALSE;
            do {
                status=actual_coverage->GetData(&actual_samples,sizeof(actual_samples),D3DGETDATA_FLUSH);
                if (status==S_FALSE) Sleep(1);
            } while (status==S_FALSE && GetTickCount64()<deadline);
            if (status!=S_OK) throw std::runtime_error("Actual particle coverage observation did not complete");
        }
        sm3::Words actual_vertex,actual_pixel;
        std::vector<float> actual_views;
        const auto& binding=pipeline.diagnostic_binding();
        if (particle_) {
            actual_vertex=code(*binding.vertex);actual_pixel=code(*binding.pixel);
            auto* texture=pipeline.diagnostic_view_texture();
            if (!texture) throw std::runtime_error("Particle draw did not upload a physical eye texture");
            D3DSURFACE_DESC description{};
            gpu_check(texture->GetLevelDesc(0,&description),"Inspect uploaded particle eye texture");
            if (description.Width!=binding.view_texture_width || description.Height!=2 ||
                description.Format!=D3DFMT_A32B32G32R32F)
                throw std::runtime_error("Particle eye texture layout differs from its shader");
            actual_views.resize(description.Width*8);
            D3DLOCKED_RECT uploaded{};
            gpu_check(texture->LockRect(0,&uploaded,nullptr,D3DLOCK_READONLY),"Read physical particle eye texture");
            for (unsigned eye=0;eye<2;++eye)
                std::memcpy(actual_views.data()+eye*description.Width*4,
                    static_cast<const char*>(uploaded.pBits)+eye*uploaded.Pitch,description.Width*4*sizeof(float));
            gpu_check(texture->UnlockRect(0),"Unlock physical particle eye texture");
        }
        if (temporal_) temporal_sample(frame,gameplay_frame,extent,vs,ps,raster_code,raster_constant,
            raster_control,after,result,before,coverage,coverage_code,local_to_world,
            particle_originals,particle_eyes,particle_context,actual_vertex,actual_pixel,actual_views,binding,actual_samples,
            pipeline.camera_directions_view_dependent(),viewport,scissor_enabled!=0,eye_scissors);
        if (!full_sample) return result;
        const auto prefix="sample_"+std::to_string(samples_++);
        for (const auto& item:std::array<std::pair<const char*,const std::vector<char>*>,3>{{
                {"before",&before},{"after",&after},{"reference",&expected}}})
            save_pixels(prefix,item.first,*item.second,before);
        save(prefix+"_vertex.bin",vs.data(),vs.size()*4);
        save(prefix+"_pixel.bin",ps.data(),ps.size()*4);
        save_pixels(prefix,"legacy_scissor_control",legacy_scissor_control,before);
        if (sweep_) {
            save_pixels(prefix,"raster_control",raster_control,before);
            save(prefix+"_raster_vertex.bin",raster_code.data(),raster_code.size()*4);
        }
        trace_<<"{\"frame\":"<<frame<<",\"sample\":"<<samples_-1
            <<",\"gameplay_frame\":"<<gameplay_frame
            <<",\"width\":"<<extent.Width<<",\"height\":"<<extent.Height
            <<",\"prefix\":"<<std::quoted(prefix)
            <<",\"pixel_storage\":"<<std::quoted(sweep_?"rle-native-pixel-delta-v1":"raw-rgba16f")
            <<",\"before_sha256\":"<<std::quoted(sha256_bytes(before.data(),before.size()))
            <<",\"after_sha256\":"<<std::quoted(sha256_bytes(after.data(),after.size()))
            <<",\"reference_sha256\":"<<std::quoted(sha256_bytes(expected.data(),expected.size()))
            <<",\"legacy_scissor_control_sha256\":"
            <<std::quoted(sha256_bytes(legacy_scissor_control.data(),legacy_scissor_control.size()))
            <<",\"vertex_sha256\":"<<std::quoted(sha256_bytes(vs.data(),vs.size()*4))
            <<",\"pixel_sha256\":"<<std::quoted(sha256_bytes(ps.data(),ps.size()*4))
            <<",\"original_retail_shaders\":true,\"native_depth_retained\":true"
            <<",\"tracked_native_world_scissor\":true"
            <<",\"native_depth_and_stencil_read_only\":true,\"diagnostic_reference_draws\":2"
            <<",\"diagnostic_only_control_draws\":"<<((sweep_?4:2)+(contribution_?2:0))
            <<",\"position_only_raster_control\":"<<(sweep_?"true":"false");
        if (sweep_) {
            trace_<<",\"raster_control_sha256\":"<<std::quoted(sha256_bytes(raster_control.data(),raster_control.size()))
                <<",\"raster_vertex_sha256\":"<<std::quoted(sha256_bytes(raster_code.data(),raster_code.size()*4))
                <<",\"raster_constant\":"<<raster_constant;
            reference_layout(trace_,viewport,tag.logical_width,scissor_enabled!=0,eye_scissors,material_sample_stride_);
        }
        trace_<<",\"scissor_enabled\":"<<scissor_enabled
            <<",\"scissor\":["<<scissor.left<<','<<scissor.top<<','<<scissor.right<<','<<scissor.bottom<<']'
            <<",\"native_viewport\":["<<viewport.X<<','<<viewport.Y<<','<<viewport.Width<<','<<viewport.Height
            <<','<<viewport.MinZ<<','<<viewport.MaxZ<<"]}\n";
        trace_.flush();
        if (!trace_) throw std::runtime_error("Cannot retain original glass comparison provenance");
        return result;
    }
};

}
