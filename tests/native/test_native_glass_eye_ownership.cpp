#include "isolated_d3d9.h"
#include "native_glass_reference_probe.h"
#include <d3dcompiler.h>
#include <iostream>

using namespace olvr;

int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-unbounded-reference";
        if (argc>2 || (argc==2 && !fault))
            throw std::invalid_argument("Unknown original-glass eye ownership control");
        unsigned continuous=0,combined=0,legacy=0;
        for (unsigned world=0;world<=900;++world) {
            continuous+=native_glass_reference_sample(world,true);
            combined+=native_glass_reference_sample(world,true,20);
            legacy+=native_glass_reference_sample(world,false);
        }
        if (continuous!=101 || combined!=6 || legacy!=6 ||
            native_glass_reference_sample(319,true,20) || native_glass_reference_sample(421,true,20))
            throw std::runtime_error("Combined material cadence changed its declared native worlds");
        bool invalid_stride=false;
        try {native_glass_reference_sample(320,true,2);}
        catch (const std::invalid_argument&) {invalid_stride=true;}
        if (!invalid_stride) throw std::runtime_error("Undeclared material cadence was admitted");
        constexpr UINT width=64,height=32;
        IsolatedDevice isolated(width*2,height);
        auto& device=*isolated.device.get();
        const auto compile=[](const char* text,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto result=D3DCompile(text,std::strlen(text),"glass-eye-ownership",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(result)) throw std::runtime_error(errors.get()?std::string(
                static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                "Cannot compile glass eye ownership fixture");
        };
        Com<ID3DBlob> vertex_code,raster_code,pixel_code;
        compile("float4 main(float4 p:POSITION):POSITION{return p;}","vs_3_0",vertex_code);
        compile("float4 Mapping:register(c0);float4 main(float4 p:POSITION):POSITION{"
            "p.x=p.x*Mapping.x+p.w*Mapping.y;return p;}","vs_3_0",raster_code);
        compile("float4 main():COLOR{return float4(.25,.5,.75,1);}","ps_3_0",pixel_code);
        Com<IDirect3DVertexShader9> vertex,raster;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vertex_code->GetBufferPointer()),
            vertex.put()),"Create original fixture vertex");
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(raster_code->GetBufferPointer()),
            raster.put()),"Create full-atlas raster fixture vertex");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(pixel_code->GetBufferPointer()),
            pixel.put()),"Create original fixture pixel");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind unchanged reference material");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create eye ownership declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind eye ownership declaration");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable fixture winding cull");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable fixture depth");
        Com<IDirect3DSurface9> output,readback;
        gpu_check(device.GetRenderTarget(0,output.put()),"Get eye ownership output");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create eye ownership readback");
        const auto read=[&] {
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read eye ownership pixels");
            D3DLOCKED_RECT lock{};
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize eye ownership pixels");
            std::vector<DWORD> result(width*2*height);
            for (UINT y=0;y<height;++y)
                std::memcpy(result.data()+y*width*2,static_cast<const char*>(lock.pBits)+y*lock.Pitch,width*2*4);
            gpu_check(readback->UnlockRect(),"Unlock eye ownership pixels");
            return result;
        };
        unsigned checks=0,rejected_legacy=0;
        for (unsigned sample=0;sample<4;++sample) for (unsigned clipped=0;clipped<2;++clipped) {
            const D3DVIEWPORT9 viewport=sample==3?D3DVIEWPORT9{16,4,96,24,0,1}:
                D3DVIEWPORT9{0,0,width*2,height,0,1};
            const RECT native{LONG(viewport.X/2+4),LONG(viewport.Y+2),
                LONG((viewport.X+viewport.Width)/2-4),LONG(viewport.Y+viewport.Height-2)};
            std::vector<DWORD> ordinary;
            for (unsigned path=0;path<3;++path) {
                gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE),"Clear complete ownership target");
                const D3DVIEWPORT9 full{0,0,width*2,height,0,1};
                gpu_check(device.SetViewport(&full),"Set complete ownership clear viewport");
                gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear ownership target");
                gpu_check(device.SetVertexShader(path?raster.get():vertex.get()),"Select original reference raster");
                gpu_check(device.BeginScene(),"Begin eye ownership fixture");
                for (unsigned eye=0;eye<2;++eye) {
                    const auto x=sample==0?1.25f:sample==1?-.75f:sample==2?-2.25f:.5f;
                    const float quad[][4]{{x,-.75f,.5f,1},{x+1,-.75f,.5f,1},{x+1,.75f,.5f,1},
                        {x,-.75f,.5f,1},{x+1,.75f,.5f,1},{x,.75f,.5f,1}};
                    auto view=viewport;
                    if (!path) {view.X=view.X/2+eye*width;view.Width/=2;}
                    else {view.X=0;view.Width=width*2;}
                    gpu_check(device.SetViewport(&view),"Select ordinary or full-atlas viewport");
                    const auto mapping=native_glass_reference_eye_mapping(viewport,width,eye);
                    gpu_check(device.SetVertexShaderConstantF(0,mapping.data(),1),"Map native clip X into one eye");
                    const bool bounded=path==1 && !fault;
                    const auto region=path?native_glass_reference_eye_scissor(
                        viewport,width,eye,clipped?&native:nullptr):
                        RECT{native.left+LONG(eye*width),native.top,native.right+LONG(eye*width),native.bottom};
                    gpu_check(device.SetScissorRect(&region),"Set original reference eye clip");
                    gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,clipped || bounded),
                        "Select bounded or legacy-unbounded reference");
                    gpu_check(device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,quad,sizeof(quad[0])),
                        "Execute isolated original-material eye reference");
                }
                gpu_check(device.EndScene(),"End eye ownership fixture");
                const auto image=read();
                if (!path) ordinary=image;
                else if (path==1) {
                    if (image!=ordinary)
                        throw std::runtime_error("Full-atlas reference crossed the original eye's clip boundary: sample="+
                            std::to_string(sample)+" clipped="+std::to_string(clipped));
                    checks+=UINT(image.size());
                } else if (!clipped && image!=ordinary) ++rejected_legacy;
            }
        }
        if (rejected_legacy<3)
            throw std::runtime_error("Legacy cross-eye reference contamination was not reproduced");
        bool invalid=false;
        try {native_glass_reference_eye_scissor({0,0,127,height,0,1},width,0);}
        catch (const std::invalid_argument&) {invalid=true;}
        if (!invalid) throw std::runtime_error("Invalid reference atlas extent was admitted");
        std::cout<<"PASS: "<<checks<<" independently rasterized reference pixels; "
            <<rejected_legacy<<" legacy cross-eye contaminations reproduced; "
            <<"both eyes, outside/crossing/in-view geometry, native scissor and offset viewport; "
            <<"101 continuous and six explicitly cadenced native material frames\n";
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
