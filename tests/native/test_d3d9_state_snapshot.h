#pragma once
#include "d3d9_resource.h"
#include <vector>
#include <cstring>
#include <string>

namespace olvr {
class TestD3D9StateSnapshot {
    std::vector<unsigned char> bytes_;
    template<class T> void append(const T& value) {
        const auto* begin=reinterpret_cast<const unsigned char*>(&value);
        bytes_.insert(bytes_.end(),begin,begin+sizeof(value));
    }
    template<class T> void pointer(const Com<T>& value) {
        append(reinterpret_cast<std::uintptr_t>(value.get()));
    }
    template<class T> void resource(const Com<T>& value) {
        pointer(value);
        ULONG references=0;
        if (value.get()) {
            value->AddRef();
            references=value->Release();
        }
        append(references);
    }
public:
    explicit TestD3D9StateSnapshot(IDirect3DDevice9& device) {
        D3DCAPS9 caps{};gpu_check(device.GetDeviceCaps(&caps),"Snapshot native capabilities");
        std::vector<float> vertex(caps.MaxVertexShaderConst*4);
        std::array<float,224*4> pixel{};
        gpu_check(device.GetVertexShaderConstantF(0,vertex.data(),caps.MaxVertexShaderConst),"Snapshot every vertex float constant");
        gpu_check(device.GetPixelShaderConstantF(0,pixel.data(),224),"Snapshot every pixel float constant");
        for (auto value:vertex) append(value);
        append(pixel);
        std::array<BOOL,16> vertex_bool{},pixel_bool{};
        std::array<int,64> vertex_int{},pixel_int{};
        gpu_check(device.GetVertexShaderConstantB(0,vertex_bool.data(),16),"Snapshot vertex boolean constants");
        gpu_check(device.GetPixelShaderConstantB(0,pixel_bool.data(),16),"Snapshot pixel boolean constants");
        gpu_check(device.GetVertexShaderConstantI(0,vertex_int.data(),16),"Snapshot vertex integer constants");
        gpu_check(device.GetPixelShaderConstantI(0,pixel_int.data(),16),"Snapshot pixel integer constants");
        append(vertex_bool);append(pixel_bool);append(vertex_int);append(pixel_int);
        Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
        Com<IDirect3DVertexDeclaration9> declaration;Com<IDirect3DIndexBuffer9> indices;
        gpu_check(device.GetVertexShader(vs.put()),"Snapshot caller vertex shader");
        gpu_check(device.GetPixelShader(ps.put()),"Snapshot caller pixel shader");
        gpu_check(device.GetVertexDeclaration(declaration.put()),"Snapshot caller declaration");
        gpu_check(device.GetIndices(indices.put()),"Snapshot caller indices");
        pointer(vs);pointer(ps);pointer(declaration);resource(indices);
        DWORD fvf=0;gpu_check(device.GetFVF(&fvf),"Snapshot caller FVF");append(fvf);
        for (UINT stream=0;stream<caps.MaxStreams;++stream) {
            Com<IDirect3DVertexBuffer9> source;UINT offset=0,stride=0,frequency=0;
            gpu_check(device.GetStreamSource(stream,source.put(),&offset,&stride),"Snapshot caller stream");
            gpu_check(device.GetStreamSourceFreq(stream,&frequency),"Snapshot caller stream frequency");
            // No vertex layout is addressable when the stream has no buffer.
            if (!source.get()) offset=stride=0;
            resource(source);append(offset);append(stride);append(frequency);
        }
        for (UINT slot=0;slot<20;++slot) {
            const UINT sampler=slot<16?slot:D3DVERTEXTEXTURESAMPLER0+slot-16;
            Com<IDirect3DBaseTexture9> texture;
            gpu_check(device.GetTexture(sampler,texture.put()),"Snapshot caller sampler texture");
            resource(texture);
            for (UINT state=D3DSAMP_ADDRESSU;state<=D3DSAMP_DMAPOFFSET;++state) {
                DWORD value=0;
                gpu_check(device.GetSamplerState(sampler,D3DSAMPLERSTATETYPE(state),&value),"Snapshot caller sampler state");
                append(value);
            }
        }
        for (auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_ALPHABLENDENABLE,
             D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAREF,
             D3DRS_ALPHAFUNC,D3DRS_CULLMODE,D3DRS_COLORWRITEENABLE,D3DRS_SRGBWRITEENABLE,
             D3DRS_STENCILENABLE,D3DRS_STENCILFUNC,D3DRS_STENCILREF,D3DRS_STENCILMASK,
             D3DRS_STENCILWRITEMASK,D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_STENCILPASS,
             D3DRS_FILLMODE,D3DRS_MULTISAMPLEANTIALIAS,D3DRS_DEPTHBIAS,D3DRS_SLOPESCALEDEPTHBIAS,
             D3DRS_CLIPPLANEENABLE,D3DRS_FOGENABLE,D3DRS_SPECULARENABLE,D3DRS_SEPARATEALPHABLENDENABLE,
             D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,D3DRS_SCISSORTESTENABLE}) {
            DWORD value=0;gpu_check(device.GetRenderState(state,&value),"Snapshot caller render state");append(value);
        }
        D3DVIEWPORT9 viewport{};RECT scissor{};
        gpu_check(device.GetViewport(&viewport),"Snapshot caller viewport");
        gpu_check(device.GetScissorRect(&scissor),"Snapshot caller scissor");
        append(viewport);append(scissor);
        for (auto state:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) {
            D3DMATRIX matrix{};gpu_check(device.GetTransform(state,&matrix),"Snapshot caller transform");append(matrix);
        }
    }
    bool operator==(const TestD3D9StateSnapshot& other) const {return bytes_==other.bytes_;}
    std::string difference(const TestD3D9StateSnapshot& other) const {
        if (bytes_.size()!=other.bytes_.size()) return "snapshot extent changed";
        for (std::size_t i=0;i<bytes_.size();++i)
            if (bytes_[i]!=other.bytes_[i]) return "byte "+std::to_string(i)+": "+
                std::to_string(bytes_[i])+" -> "+std::to_string(other.bytes_[i]);
        return "none";
    }
};
}
