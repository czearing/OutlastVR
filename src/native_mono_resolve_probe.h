#pragma once
#include "d3d9_shader_variants.h"
#include "sha256.h"
#include "native_ppm.h"
#include <filesystem>
#include <fstream>

namespace olvr {
class NativeMonoResolveProbe {
    std::filesystem::path folder_;
    std::ofstream evidence_;
    std::ofstream raw_sources_;
    UINT raw_frame_=UINT_MAX;
    UINT samples_=0,last_frame_=UINT_MAX;
    static std::vector<DWORD> read(IDirect3DDevice9& device,IDirect3DSurface9& source,
                                   UINT width=512,UINT height=288) {
        D3DSURFACE_DESC description{};
        gpu_check(source.GetDesc(&description),"Inspect native mono proof output");
        if (description.Width!=width || description.Height!=height || description.Format!=D3DFMT_A8R8G8B8)
            throw std::runtime_error("Native mono reference requires the admitted complete lens target");
        Com<IDirect3DSurface9> staging;
        gpu_check(device.CreateOffscreenPlainSurface(width,height,description.Format,D3DPOOL_SYSTEMMEM,
            staging.put(),nullptr),"Create native mono reference readback");
        gpu_check(device.GetRenderTargetData(&source,staging.get()),"Read native mono reference pixels");
        D3DLOCKED_RECT lock{};
        gpu_check(staging->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize complete native mono reference");
        std::vector<DWORD> pixels(std::size_t(width)*height);
        for (UINT y=0;y<height;++y) std::memcpy(pixels.data()+std::size_t(y)*width,
            static_cast<const char*>(lock.pBits)+y*lock.Pitch,width*4);
        gpu_check(staging->UnlockRect(),"Unlock native mono reference");return pixels;
    }
public:
    explicit NativeMonoResolveProbe(const std::filesystem::path& folder):folder_(folder) {
        evidence_.open(folder_/"native-mono-resolve.jsonl");
        raw_sources_.open(folder_/"native-raw-optics.jsonl");
        if (!evidence_ || !raw_sources_) throw std::runtime_error("Cannot retain actual native mono reference evidence");
    }
    bool due(UINT frame) const {return samples_<3 && frame>=400+samples_*300;}
    bool ready(UINT frame) const {return last_frame_==frame;}
    void sampled(UINT count) {samples_=count;}
    bool needs_raw_source(UINT frame) const {return due(frame) && raw_frame_!=frame;}
    void raw_source(IDirect3DDevice9& device,IDirect3DTexture9& texture,UINT frame,const RECT& region,
                    const std::string& shader,const std::vector<DWORD>& program,UINT sampler,
                    std::uintptr_t rhi,std::uintptr_t view) {
        const auto program_bytes=program.size()*sizeof(DWORD);
        if (program_bytes<8 || program_bytes>1048576 || sha256_bytes(program.data(),program_bytes)!=shader)
            throw std::runtime_error("Raw optics shader bytes differ from their actual bound native program");
        D3DSURFACE_DESC extent{};
        gpu_check(texture.GetLevelDesc(0,&extent),"Inspect actual first HDR postprocess source");
        if (region.left<0 || region.top<0 || region.right-region.left!=512 ||
            region.bottom-region.top!=288 || UINT(region.right)>extent.Width ||
            UINT(region.bottom)>extent.Height || extent.Format!=D3DFMT_A16B16G16R16F)
            throw std::runtime_error("Raw native optics require the actual complete half-float scene view");
        Com<IDirect3DSurface9> source,cropped,staging;
        gpu_check(texture.GetSurfaceLevel(0,source.put()),"Read first HDR postprocess source surface");
        gpu_check(device.CreateRenderTarget(512,288,extent.Format,D3DMULTISAMPLE_NONE,0,FALSE,
            cropped.put(),nullptr),"Create unchanged-format pre-Uber optics snapshot");
        gpu_check(device.StretchRect(source.get(),&region,cropped.get(),nullptr,D3DTEXF_NONE),
            "Retain exact pre-Uber scene pixels without tone mapping or UV distortion");
        gpu_check(device.CreateOffscreenPlainSurface(512,288,extent.Format,D3DPOOL_SYSTEMMEM,
            staging.put(),nullptr),"Create half-float native optics readback");
        gpu_check(device.GetRenderTargetData(cropped.get(),staging.get()),"Read exact pre-Uber native optics");
        std::vector<BYTE> pixels(512*288*8);
        D3DLOCKED_RECT locked{};
        gpu_check(staging->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Synchronize raw native optics");
        for (UINT y=0;y<288;++y)
            std::memcpy(pixels.data()+y*512*8,static_cast<const char*>(locked.pBits)+y*locked.Pitch,512*8);
        gpu_check(staging->UnlockRect(),"Release raw native optics readback");
        const auto name="native-raw-optics-"+std::to_string(frame)+".rgba16f";
        std::ofstream image(folder_/name,std::ios::binary);
        image.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()));
        image.close();
        if (!image) throw std::runtime_error("Cannot retain complete raw native optics pixels");
        const auto program_name="native-raw-optics-program-"+std::to_string(frame)+".bin";
        if (std::filesystem::exists(folder_/program_name))
            throw std::runtime_error("Refusing to overwrite native raw optics shader evidence");
        std::ofstream code(folder_/program_name,std::ios::binary);
        code.write(reinterpret_cast<const char*>(program.data()),std::streamsize(program_bytes));
        code.close();
        if (!code) throw std::runtime_error("Cannot retain actual first HDR native shader bytes");
        raw_sources_<<"{\"raw_optics_schema_version\":2,\"frame\":"<<frame<<",\"image\":\""<<name
            <<"\",\"width\":512,\"height\":288,\"format\":"<<UINT(extent.Format)
            <<",\"source_texture\":"<<reinterpret_cast<std::uintptr_t>(&texture)
            <<",\"source_rhi_texture\":"<<rhi<<",\"native_view\":"<<view
            <<",\"image_sha256\":\""<<sha256_bytes(pixels.data(),pixels.size())<<'"'
            <<",\"first_hdr_postprocess_shader\":\""<<shader<<"\",\"sampler\":"<<sampler
            <<",\"first_hdr_program\":\""<<program_name<<"\",\"first_hdr_program_bytes\":"<<program_bytes
            <<",\"source_extent\":["<<extent.Width<<','<<extent.Height<<"],\"source_rectangle\":["
            <<region.left<<','<<region.top<<','<<region.right<<','<<region.bottom<<"]}\n";
        raw_sources_.flush();
        if (!raw_sources_) throw std::runtime_error("Cannot retain raw native optics provenance");
        raw_frame_=frame;
    }
    void postprocess_output(IDirect3DDevice9& device,IDirect3DSurface9& source,UINT frame,INT mode,
                            const RECT& region) {
        if (region.left<0 || region.top<0 || region.right-region.left!=512 || region.bottom-region.top!=288)
            throw std::runtime_error("Native camera postprocess snapshot has an unexpected native view rectangle");
        D3DSURFACE_DESC extent{};
        gpu_check(source.GetDesc(&extent),"Read actual postprocess snapshot extent");
        if (UINT(region.right)>extent.Width || UINT(region.bottom)>extent.Height ||
            extent.Format!=D3DFMT_A8R8G8B8)
            throw std::runtime_error("Native camera postprocess snapshot exceeds its admitted color target");
        Com<IDirect3DSurface9> cropped;
        gpu_check(device.CreateRenderTarget(512,288,extent.Format,D3DMULTISAMPLE_NONE,0,FALSE,
            cropped.put(),nullptr),"Create exact native postprocess snapshot");
        gpu_check(device.StretchRect(&source,&region,cropped.get(),nullptr,D3DTEXF_NONE),
            "Retain actual native postprocess viewport pixels");
        const auto pixels=read(device,*cropped.get());
        std::ofstream image(folder_/("native-postprocess-output-"+std::to_string(frame)+
            "-mode-"+std::to_string(mode)+".ppm"),std::ios::binary);
        image<<"P6\n512 288\n255\n";
        write_native_ppm_rgb(image,pixels.data(),pixels.size(),512,288);
        image.close();
        if (!image) throw std::runtime_error("Cannot retain actual native postprocess output pixels");
        const auto save=[&](const std::string& name,const std::vector<DWORD>& values,UINT width,UINT height) {
            std::ofstream output(folder_/name,std::ios::binary);
            output<<"P6\n"<<width<<' '<<height<<"\n255\n";
            write_native_ppm_rgb(output,values.data(),values.size(),width,height);
            output.close();
            if (!output) throw std::runtime_error("Cannot retain native postprocess diagnostic pixels");
        };
        gpu_check(device.StretchRect(&source,nullptr,cropped.get(),nullptr,D3DTEXF_POINT),
            "Inspect complete native postprocess target independently of its view rectangle");
        save("native-postprocess-atlas-"+std::to_string(frame)+".ppm",read(device,*cropped.get()),512,288);
        Com<IDirect3DBaseTexture9> lut;
        gpu_check(device.GetTexture(3,lut.put()),"Read actual native color-grading LUT binding");
        if (!lut.get() || lut->GetType()!=D3DRTYPE_TEXTURE)
            throw std::runtime_error("Actual native color-grading LUT is not a 2D texture");
        Com<IDirect3DSurface9> lut_surface;
        gpu_check(static_cast<IDirect3DTexture9*>(lut.get())->GetSurfaceLevel(0,lut_surface.put()),
            "Read actual native color-grading LUT surface");
        save("native-postprocess-lut-"+std::to_string(frame)+".ppm",read(device,*lut_surface.get(),256,16),256,16);
    }
    void screen_before(IDirect3DDevice9& device,IDirect3DSurface9& source,UINT frame,UINT width,UINT height,
                       UINT capture_width,UINT capture_height) {
        if ((frame+1)%30 && !ready(frame)) return;
        Com<IDirect3DSurface9> scaled;
        if (width!=capture_width || height!=capture_height) {
            gpu_check(device.CreateRenderTarget(capture_width*2,capture_height,D3DFMT_A8R8G8B8,
                D3DMULTISAMPLE_NONE,0,FALSE,scaled.put(),nullptr),"Create exact XR pre-screen reference extent");
            for (UINT eye=0;eye<2;++eye) {
                const RECT from{LONG(eye*width),0,LONG((eye+1)*width),LONG(height)};
                const RECT to{LONG(eye*capture_width),0,LONG((eye+1)*capture_width),LONG(capture_height)};
                gpu_check(device.StretchRect(&source,&from,scaled.get(),&to,D3DTEXF_LINEAR),
                    "Match the actual XR per-eye submission resampling");
            }
        }
        const auto pixels=read(device,scaled.get()?*scaled.get():source,capture_width*2,capture_height);
        for (UINT eye=0;eye<2;++eye) {
            std::ofstream image(folder_/("native-screen-before-"+std::to_string(frame)+
                "-eye-"+std::to_string(eye)+".ppm"),std::ios::binary);
            image<<"P6\n"<<capture_width<<' '<<capture_height<<"\n255\n";
            write_native_ppm_rgb(image,pixels.data()+eye*capture_width,
                pixels.size()-eye*capture_width,capture_width,capture_height,capture_width*2);
            image.close();
            if (!image) throw std::runtime_error("Cannot retain same-frame pre-screen native image");
        }
    }
    template<class NativeDraw,class CorrectedDraw>
    HRESULT compare(IDirect3DDevice9& device,IDirect3DSurface9& output,UINT frame,
                    unsigned samplers,NativeDraw native_draw,CorrectedDraw corrected_draw) {
        if (!samplers || !due(frame)) throw std::logic_error("Native reference requires an actual atlas resolve");
        D3DSURFACE_DESC description{};
        gpu_check(output.GetDesc(&description),"Inspect actual native mono resolve");
        Com<IDirect3DTexture9> reference;
        Com<IDirect3DSurface9> reference_surface;
        gpu_check(device.CreateTexture(description.Width,description.Height,1,D3DUSAGE_RENDERTARGET,
            description.Format,D3DPOOL_DEFAULT,reference.put(),nullptr),"Create independent original-shader mono reference");
        gpu_check(reference->GetSurfaceLevel(0,reference_surface.put()),"Get native mono reference target");
        StateRestore state(device);
        D3DCAPS9 caps{};gpu_check(device.GetDeviceCaps(&caps),"Inspect native reference MRT slots");
        const auto count=std::min<UINT>(caps.NumSimultaneousRTs,4);
        std::array<Com<IDirect3DSurface9>,4> targets;
        Com<IDirect3DSurface9> depth;
        for (UINT slot=0;slot<count;++slot) {
            const auto status=device.GetRenderTarget(slot,targets[slot].put());
            if (status!=D3DERR_NOTFOUND) gpu_check(status,"Retain native reference caller target");
        }
        const auto depth_status=device.GetDepthStencilSurface(depth.put());
        if (depth_status!=D3DERR_NOTFOUND) gpu_check(depth_status,"Retain native reference caller depth");
        const auto restore=[&] {
            gpu_check(device.SetRenderTarget(0,targets[0].get()),"Restore actual native mono target");
            for (UINT slot=1;slot<count;++slot)
                gpu_check(device.SetRenderTarget(slot,targets[slot].get()),"Restore native mono MRT");
            gpu_check(device.SetDepthStencilSurface(depth.get()),"Restore native mono depth");
            gpu_check(state.apply(),"Restore unmodified native mono caller");
        };
        std::array<Com<IDirect3DTexture9>,16> aliases;
        try {
            for (unsigned slot=0;slot<16;++slot) if (samplers&(1u<<slot)) {
                Com<IDirect3DBaseTexture9> source;
                gpu_check(device.GetTexture(slot,source.put()),"Read native reference atlas source");
                if (!source.get() || source->GetType()!=D3DRTYPE_TEXTURE)
                    throw std::runtime_error("Native reference source is not an admitted 2D atlas");
                auto* texture=static_cast<IDirect3DTexture9*>(source.get());
                D3DSURFACE_DESC extent{};
                gpu_check(texture->GetLevelDesc(0,&extent),"Inspect native reference source extent");
                if (texture->GetLevelCount()!=1 || extent.Width<2 || extent.Width%2)
                    throw std::runtime_error("Native reference source is not the admitted single-level eye atlas");
                gpu_check(device.CreateTexture(extent.Width/2,extent.Height,1,D3DUSAGE_RENDERTARGET,
                    extent.Format,D3DPOOL_DEFAULT,aliases[slot].put(),nullptr),"Create non-atlased native reference source");
                Com<IDirect3DSurface9> original,alias;
                gpu_check(texture->GetSurfaceLevel(0,original.put()),"Get rendered native source atlas");
                gpu_check(aliases[slot]->GetSurfaceLevel(0,alias.put()),"Get independent native mono source");
                const RECT left{0,0,LONG(extent.Width/2),LONG(extent.Height)};
                gpu_check(device.StretchRect(original.get(),&left,alias.get(),nullptr,D3DTEXF_NONE),
                    "Preserve every original-format source pixel for the native reference");
            }
            gpu_check(device.StretchRect(&output,nullptr,reference_surface.get(),nullptr,D3DTEXF_NONE),
                "Seed identical native reference caller contents");
            D3DVIEWPORT9 viewport{};
            gpu_check(device.GetViewport(&viewport),"Read native reference viewport");
            gpu_check(device.SetDepthStencilSurface(nullptr),"Detach native reference depth");
            for (UINT slot=1;slot<count;++slot)
                gpu_check(device.SetRenderTarget(slot,nullptr),"Detach native reference MRT");
            gpu_check(device.SetRenderTarget(0,reference_surface.get()),"Bind original-shader native mono reference");
            gpu_check(device.SetViewport(&viewport),"Preserve the full original native mono viewport");
            for (unsigned slot=0;slot<16;++slot) if (aliases[slot].get())
                gpu_check(device.SetTexture(slot,aliases[slot].get()),"Bind non-atlased original native source");
            gpu_check(native_draw(),"Execute the unmodified retail shader and native draw against mono sources");
        } catch (...) {restore();throw;}
        restore();
        const auto result=corrected_draw();
        gpu_check(result,"Execute corrected actual native atlas resolve");
        const auto expected=read(device,*reference_surface.get()),actual=read(device,output);
        for (std::size_t i=0;i<actual.size();++i)
            if (actual[i]!=expected[i]) throw std::runtime_error("Actual native lens resolve differs from the original-shader mono reference");
        const auto name="native-mono-reference-"+std::to_string(frame)+".ppm";
        std::ofstream image(folder_/name,std::ios::binary);
        image<<"P6\n512 288\n255\n";
        write_native_ppm_rgb(image,expected.data(),expected.size(),512,288);
        image.close();
        if (!image) throw std::runtime_error("Cannot retain complete native mono reference image");
        evidence_<<"{\"frame\":"<<frame<<",\"sample\":"<<samples_<<",\"atlas_samplers\":"<<samplers
            <<",\"checked_pixels\":"<<actual.size()<<",\"different_pixels\":0,\"original_retail_shader\":true"
            <<",\"reference_image\":\""<<name<<"\"}\n";evidence_.flush();
        if (!evidence_) throw std::runtime_error("Cannot retain whole-native-image comparison");
        last_frame_=frame;return result;
    }
};
}
