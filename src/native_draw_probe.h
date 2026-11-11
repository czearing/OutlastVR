#pragma once
#include "outlast_target_roles.h"
#include "sha256.h"
#include "sm3_runtime.h"
#include "native_draw_selection.h"
#include <cmath>
#include <iomanip>
#include <type_traits>
#include <unordered_map>

namespace olvr {

enum class NativeDrawReadback {images,constants_only};

class NativeDrawProbe {
    std::filesystem::path folder_;
    std::ofstream trace_;
    NativeDrawSelection selection_;
    bool retain_hdr_=false;
    NativeDrawReadback readback_policy_=NativeDrawReadback::images;
    std::uint64_t serial_=0;
    Com<IDirect3DSurface9> readback_;
    D3DSURFACE_DESC cached_{};
    std::unordered_map<std::string,std::vector<sm3::Constant>> shaders_;
    template<class T> std::string shader(IDirect3DDevice9& device,T& object) {
        UINT bytes=0;
        gpu_check(object.GetFunction(nullptr,&bytes),"Read diagnostic shader extent");
        if (bytes<8 || bytes%4 || bytes>1048576)
            throw std::runtime_error("Invalid diagnostic shader extent");
        sm3::Words words(bytes/4);
        const auto expected=bytes;
        gpu_check(object.GetFunction(words.data(),&bytes),"Read diagnostic shader");
        if (bytes!=expected) throw std::runtime_error("Diagnostic shader extent changed");
        const auto hash=sha256_bytes(words.data(),bytes);
        if (!shaders_.count(hash)) {
            const auto constants=sm3::inspect(sm3::parse(words)).constants;
            shaders_.emplace(hash,constants);
            std::ofstream output(folder_/(hash+".bin"),std::ios::binary);
            output.write(reinterpret_cast<const char*>(words.data()),bytes);output.close();
            if (!output) throw std::runtime_error("Cannot retain diagnostic shader");
            trace_<<"{\"shader\":"<<std::quoted(hash)<<",\"constants\":[";
            bool separator=false;
            for (const auto& constant:constants) {
                trace_<<(separator?",":"")<<"{\"name\":"<<std::quoted(constant.name)
                    <<",\"register\":"<<constant.number<<",\"count\":"<<constant.count;
                if (constant.set==2 && constant.count<=4 &&
                    (constant.name=="ViewProjectionMatrix" || constant.name=="CameraWorldPos" ||
                     constant.name=="CameraPositionVS" || constant.name=="DynamicVelocityParameters" ||
                     constant.name=="MotionBlurMaskScaleAndBias")) {
                    float values[16]{};
                    if constexpr (std::is_same_v<T,IDirect3DVertexShader9>)
                        gpu_check(device.GetVertexShaderConstantF(constant.number,values,constant.count),
                            "Read diagnostic vertex view uniform");
                    else
                        gpu_check(device.GetPixelShaderConstantF(constant.number,values,constant.count),
                            "Read diagnostic pixel view uniform");
                    trace_<<",\"values\":[";
                    for (unsigned i=0;i<constant.count*4;++i) {
                        trace_<<(i?",":"");
                        if (std::isfinite(values[i])) trace_<<values[i];else trace_<<"null";
                    }
                    trace_<<"]";
                }
                trace_<<"}";
                separator=true;
            }
            trace_<<"]}\n";
        }
        return hash;
    }
    void uniforms(IDirect3DDevice9& device,const std::string& hash,bool vertex) {
        trace_<<"[";
        bool separator=false;
        for (const auto& constant:shaders_.at(hash)) {
            if (constant.set!=2) continue;
            const unsigned limit=vertex?256:224;
            if (!constant.count || constant.number>=limit || constant.count>limit-constant.number)
                throw std::runtime_error("Diagnostic uniform exceeds the native SM3 constant bank");
            std::vector<float> values(constant.count*4);
            if (vertex)
                gpu_check(device.GetVertexShaderConstantF(constant.number,values.data(),constant.count),
                    "Read per-draw native vertex material uniforms");
            else
                gpu_check(device.GetPixelShaderConstantF(constant.number,values.data(),constant.count),
                    "Read per-draw native pixel material uniforms");
            trace_<<(separator?",":"")<<"{\"name\":"<<std::quoted(constant.name)
                <<",\"register\":"<<constant.number<<",\"count\":"<<constant.count<<",\"values\":[";
            bool nonfinite=false;
            for (unsigned i=0;i<values.size();++i) {
                trace_<<(i?",":"");
                if (std::isfinite(values[i])) trace_<<values[i];else {trace_<<"null";nonfinite=true;}
            }
            trace_<<"],\"nonfinite\":"<<(nonfinite?"true":"false");
            if (nonfinite) {
                trace_<<",\"raw_bits\":[";
                for (unsigned i=0;i<values.size();++i) {
                    std::uint32_t bits=0;std::memcpy(&bits,&values[i],sizeof(bits));
                    trace_<<(i?",":"")<<bits;
                }
                trace_<<"]";
            }
            trace_<<"}";separator=true;
        }
        trace_<<"]";
    }
    void textures(IDirect3DDevice9& device) {
        trace_<<"[";
        for (DWORD slot=0;slot<16;++slot) {
            Com<IDirect3DBaseTexture9> texture;
            gpu_check(device.GetTexture(slot,texture.put()),"Read diagnostic material texture");
            trace_<<(slot?",":"")<<"{\"slot\":"<<slot<<",\"resource\":";
            if (!texture.get()) {trace_<<"null}";continue;}
            trace_<<reinterpret_cast<std::uintptr_t>(texture.get());
            UINT width=0,height=0,depth=1;
            DWORD usage=0;D3DFORMAT format{};D3DPOOL pool{};
            const auto type=texture->GetType();
            if (type==D3DRTYPE_TEXTURE || type==D3DRTYPE_CUBETEXTURE) {
                D3DSURFACE_DESC description{};
                if (type==D3DRTYPE_TEXTURE) {
                    Com<IDirect3DTexture9> typed;
                    gpu_check(texture->QueryInterface(__uuidof(IDirect3DTexture9),reinterpret_cast<void**>(typed.put())),
                        "Inspect diagnostic 2D texture");
                    gpu_check(typed->GetLevelDesc(0,&description),"Read diagnostic 2D texture extent");
                } else {
                    Com<IDirect3DCubeTexture9> typed;
                    gpu_check(texture->QueryInterface(__uuidof(IDirect3DCubeTexture9),reinterpret_cast<void**>(typed.put())),
                        "Inspect diagnostic cube texture");
                    gpu_check(typed->GetLevelDesc(0,&description),"Read diagnostic cube texture extent");
                }
                width=description.Width;height=description.Height;format=description.Format;
                usage=description.Usage;pool=description.Pool;
            } else if (type==D3DRTYPE_VOLUMETEXTURE) {
                Com<IDirect3DVolumeTexture9> typed;D3DVOLUME_DESC description{};
                gpu_check(texture->QueryInterface(__uuidof(IDirect3DVolumeTexture9),reinterpret_cast<void**>(typed.put())),
                    "Inspect diagnostic volume texture");
                gpu_check(typed->GetLevelDesc(0,&description),"Read diagnostic volume texture extent");
                width=description.Width;height=description.Height;depth=description.Depth;
                format=description.Format;usage=description.Usage;pool=description.Pool;
            } else throw std::runtime_error("Unsupported native diagnostic texture type");
            trace_<<",\"type\":"<<DWORD(type)<<",\"width\":"<<width<<",\"height\":"<<height
                <<",\"depth\":"<<depth<<",\"format\":"<<DWORD(format)<<",\"usage\":"<<usage
                <<",\"pool\":"<<DWORD(pool)<<",\"levels\":"<<texture->GetLevelCount()<<",\"sampler\":{";
            const std::pair<const char*,D3DSAMPLERSTATETYPE> states[]{
                {"address_u",D3DSAMP_ADDRESSU},{"address_v",D3DSAMP_ADDRESSV},{"address_w",D3DSAMP_ADDRESSW},
                {"mag_filter",D3DSAMP_MAGFILTER},{"min_filter",D3DSAMP_MINFILTER},{"mip_filter",D3DSAMP_MIPFILTER},
                {"lod_bias_bits",D3DSAMP_MIPMAPLODBIAS},{"max_mip",D3DSAMP_MAXMIPLEVEL},
                {"max_anisotropy",D3DSAMP_MAXANISOTROPY},{"srgb",D3DSAMP_SRGBTEXTURE},
                {"border_color",D3DSAMP_BORDERCOLOR}};
            bool separator=false;
            for (const auto& state:states) {
                DWORD value=0;gpu_check(device.GetSamplerState(slot,state.second,&value),
                    "Read diagnostic material sampler");
                trace_<<(separator?",":"")<<std::quoted(state.first)<<":"<<value;separator=true;
            }
            trace_<<"}}";
        }
        trace_<<"]";
    }
    static float half(std::uint16_t bits) {
        const unsigned exponent=(bits>>10)&31,mantissa=bits&1023;
        const auto value=exponent==31?std::numeric_limits<float>::infinity():
            std::ldexp(float(exponent?mantissa+1024:mantissa),exponent?int(exponent)-25:-24);
        return bits&32768?-value:value;
    }
public:
    NativeDrawProbe(const std::filesystem::path& evidence,std::uint64_t frame,
                    const wchar_t* directory=L"draw-probe",bool retain_hdr=false,
                    NativeDrawReadback readback=NativeDrawReadback::images)
        :selection_{frame,frame==0},retain_hdr_(retain_hdr),readback_policy_(readback) {
        if (retain_hdr_ && readback_policy_==NativeDrawReadback::constants_only)
            throw std::invalid_argument("HDR diagnostic evidence requires actual pixel readback");
        folder_=evidence/directory;
        if (!std::filesystem::create_directory(folder_))
            throw std::runtime_error("Diagnostic draw directory must be new");
        trace_.open(folder_/L"draws.jsonl");
        if (!trace_) throw std::runtime_error("Cannot create native draw diagnostic trace");
        trace_<<std::setprecision(9);
    }
    std::uint64_t selected_frame() const {return selection_.frame;}
    void retain_bound_shaders(IDirect3DDevice9& device,std::uint64_t frame=0) {
        if (frame && !selection_.selects(frame,false)) return;
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.GetVertexShader(vertex.put()),"Read refused native vertex shader");
        gpu_check(device.GetPixelShader(pixel.put()),"Read refused native pixel shader");
        if (!frame && (!vertex.get() || !pixel.get()))
            throw std::runtime_error("Refused native pair lacks its source shader");
        const auto vs=vertex.get()?shader(device,*vertex.get()):std::string{};
        const auto ps=pixel.get()?shader(device,*pixel.get()):std::string{};
        if (frame) {
            trace_<<"{\"native_frame\":"<<frame<<",\"native_mono_vertex\":";
            if (vertex.get()) trace_<<std::quoted(vs);else trace_<<"null";
            trace_<<",\"native_mono_pixel\":";
            if (pixel.get()) trace_<<std::quoted(ps);else trace_<<"null";
            trace_<<"}\n";
        }
        trace_.flush();
        if (!trace_) throw std::runtime_error("Cannot preserve refused native shader state");
    }
    void capture(IDirect3DDevice9& device,std::uint64_t frame,bool contact_pose_ready=false) {
        if (selection_.predicate_driven && !selection_.frame) {
            if (!contact_pose_ready) return;
            Com<IDirect3DVertexShader9> vertex;
            gpu_check(device.GetVertexShader(vertex.put()),"Identify contact-frame skeletal draw");
            if (!vertex.get()) return;
            const auto hash=shader(device,*vertex.get());
            const auto& constants=shaders_.at(hash);
            if (std::none_of(constants.begin(),constants.end(),[](const auto& value) {
                return value.name=="BoneMatrices" && value.count>=75;
            })) return;
        }
        if (!selection_.selects(frame,contact_pose_ready)) return;
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        Com<IDirect3DSurface9> target;
        gpu_check(device.GetVertexShader(vertex.put()),"Read diagnostic vertex shader");
        gpu_check(device.GetPixelShader(pixel.put()),"Read diagnostic pixel shader");
        gpu_check(device.GetRenderTarget(0,target.put()),"Read diagnostic color target");
        if (!vertex.get() || !pixel.get() || !target.get())
            throw std::runtime_error("Native draw diagnostic lacks its source state");
        const auto vs=shader(device,*vertex.get()),ps=shader(device,*pixel.get());
        D3DSURFACE_DESC description{};
        gpu_check(target->GetDesc(&description),"Read diagnostic target extent");
        OutlastAtlasTag tag{};
        if (!atlas_tag(*target.get(),tag)) throw std::runtime_error("Diagnostic draw lost its atlas tag");
        DWORD alpha=0,source=0,destination=0,write=0,depth=0;
        gpu_check(device.GetRenderState(D3DRS_ALPHABLENDENABLE,&alpha),"Read diagnostic alpha");
        gpu_check(device.GetRenderState(D3DRS_SRCBLEND,&source),"Read diagnostic source blend");
        gpu_check(device.GetRenderState(D3DRS_DESTBLEND,&destination),"Read diagnostic destination blend");
        gpu_check(device.GetRenderState(D3DRS_ZWRITEENABLE,&write),"Read diagnostic depth write");
        gpu_check(device.GetRenderState(D3DRS_ZENABLE,&depth),"Read diagnostic depth state");
        const auto serial=++serial_;
        const bool color=tag.role==OutlastTargetRole::scene_color || tag.role==OutlastTargetRole::post;
        const unsigned stride=description.Format==D3DFMT_A16B16G16R16F?8:
            description.Format==D3DFMT_A8R8G8B8 || description.Format==D3DFMT_X8R8G8B8?4:0;
        const bool image=readback_policy_==NativeDrawReadback::images && color && stride;
        std::uint64_t nonfinite=0;
        const bool hdr=retain_hdr_ && color && stride==8;
        std::string hdr_hash;
        if (image) {
            if (!readback_.get() || description.Width!=cached_.Width ||
                description.Height!=cached_.Height || description.Format!=cached_.Format) {
                readback_.reset();
                gpu_check(device.CreateOffscreenPlainSurface(description.Width,description.Height,
                    description.Format,D3DPOOL_SYSTEMMEM,readback_.put(),nullptr),"Create diagnostic readback");
                cached_=description;
            }
            gpu_check(device.GetRenderTargetData(target.get(),readback_.get()),"Read diagnostic draw pixels");
            D3DLOCKED_RECT locked{};
            gpu_check(readback_->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Lock diagnostic draw pixels");
            std::vector<char> image(512*256*3);
            std::vector<std::uint16_t> hdr_image(hdr?512*256*4:0);
            for (unsigned y=0;y<256;++y) for (unsigned x=0;x<512;++x) {
                const auto* value=static_cast<const unsigned char*>(locked.pBits)+
                    std::size_t(y)*description.Height/256*locked.Pitch+
                    std::size_t(x)*description.Width/512*stride;
                if (hdr) std::memcpy(hdr_image.data()+(y*512+x)*4,value,8);
                for (unsigned component=0;component<3;++component) {
                    float channel=value[2-component]/255.f;
                    if (stride==8) {
                        std::uint16_t bits=0;std::memcpy(&bits,value+component*2,2);channel=half(bits);
                    }
                    if (!std::isfinite(channel)) {++nonfinite;channel=0;}
                    image[(y*512+x)*3+component]=char(std::clamp(channel,0.f,1.f)*255);
                }
            }
            gpu_check(readback_->UnlockRect(),"Unlock diagnostic draw pixels");
            std::ofstream output(folder_/("draw_"+std::to_string(serial)+".ppm"),std::ios::binary);
            output<<"P6\n512 256\n255\n";output.write(image.data(),image.size());output.close();
            if (!output) throw std::runtime_error("Cannot retain diagnostic draw image");
            if (hdr) {
                const auto bytes=hdr_image.size()*sizeof(hdr_image[0]);
                hdr_hash=sha256_bytes(hdr_image.data(),bytes);
                std::ofstream raw(folder_/("draw_"+std::to_string(serial)+".rgba16f"),std::ios::binary);
                raw.write(reinterpret_cast<const char*>(hdr_image.data()),bytes);raw.close();
                if (!raw) throw std::runtime_error("Cannot retain diagnostic HDR draw samples");
            }
        }
        trace_<<"{\"native_frame\":"<<frame<<",\"draw\":"<<serial<<",\"role\":"<<DWORD(tag.role)
            <<",\"format\":"<<DWORD(description.Format)<<",\"image\":"<<(image?"true":"false")
            <<",\"pixel_readback_omitted\":"<<(readback_policy_==NativeDrawReadback::constants_only?"true":"false")
            <<",\"vertex\":"<<std::quoted(vs)<<",\"pixel\":"<<std::quoted(ps)
            <<",\"alpha\":"<<alpha<<",\"src\":"<<source<<",\"dst\":"<<destination
            <<",\"zwrite\":"<<write<<",\"ztest\":"<<depth<<",\"nonfinite_sample_channels\":";
        if (image) trace_<<nonfinite;else trace_<<"null";
        trace_<<",\"hdr_image\":"<<(hdr?"true":"false");
        if (hdr) trace_<<",\"hdr_format\":\"RGBA16F_LE\",\"hdr_width\":512,\"hdr_height\":256"
            <<",\"hdr_sha256\":"<<std::quoted(hdr_hash)
            <<",\"sample_source_width\":"<<description.Width
            <<",\"sample_source_height\":"<<description.Height;
        trace_<<",\"material_state\":{";
        const std::pair<const char*,D3DRENDERSTATETYPE> states[]{
            {"alpha_test",D3DRS_ALPHATESTENABLE},{"alpha_ref",D3DRS_ALPHAREF},{"alpha_func",D3DRS_ALPHAFUNC},
            {"cull",D3DRS_CULLMODE},{"depth_func",D3DRS_ZFUNC},{"depth_bias_bits",D3DRS_DEPTHBIAS},
            {"slope_depth_bias_bits",D3DRS_SLOPESCALEDEPTHBIAS},{"srgb_write",D3DRS_SRGBWRITEENABLE},
            {"color_write",D3DRS_COLORWRITEENABLE},{"blend_op",D3DRS_BLENDOP}};
        bool separator=false;
        for (const auto& state:states) {
            DWORD value=0;gpu_check(device.GetRenderState(state.second,&value),"Read diagnostic raster material state");
            trace_<<(separator?",":"")<<std::quoted(state.first)<<":"<<value;separator=true;
        }
        trace_<<"},\"vertex_constants\":";uniforms(device,vs,true);
        trace_<<",\"pixel_constants\":";uniforms(device,ps,false);
        trace_<<",\"textures\":";textures(device);
        trace_<<"}\n";
        trace_.flush();
        if (!trace_) throw std::runtime_error("Cannot retain diagnostic draw state");
    }
};

}
