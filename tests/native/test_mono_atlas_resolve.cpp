#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include <iostream>

using namespace olvr;

int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-unmapped";
        const bool fault_destination=argc==2 && std::string(argv[1])=="--fault-destination";
        if (argc>2 || (argc==2 && !fault && !fault_destination))
            throw std::invalid_argument("Unknown mono resolve control");
        constexpr UINT width=64,height=16;
        IsolatedDevice isolated(width,height);
        auto& device=*isolated.device.get();
        auto compile=[&](const std::string& source,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto result=D3DCompile(source.data(),source.size(),"mono-atlas-resolve",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(result)) throw std::runtime_error(errors.get()?std::string(
                static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Shader compilation failed");
        };
        Com<ID3DBlob> vertex_code;
        compile(R"(
            float4 UV:register(c0);
            struct Output {float4 position:POSITION;float2 uv:TEXCOORD0;};
            Output main(float2 position:POSITION) {
                Output output;output.position=float4(position,0,1);
                output.uv=(position*float2(.5,-.5)+.5)*UV.xy+UV.zw;return output;
            })","vs_3_0",vertex_code);
        Com<IDirect3DVertexShader9> vertex;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vertex_code->GetBufferPointer()),vertex.put()),
            "Create mono reference vertex");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind unchanged mono vertex");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create mono declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind mono declaration");
        for (auto setting:std::initializer_list<std::pair<D3DRENDERSTATETYPE,DWORD>>{
            {D3DRS_ZENABLE,FALSE},{D3DRS_CULLMODE,D3DCULL_NONE},{D3DRS_SCISSORTESTENABLE,FALSE}})
            gpu_check(device.SetRenderState(setting.first,setting.second),"Set mono reference raster state");
        std::array<Com<IDirect3DTexture9>,3> textures;
        for (unsigned index=0;index<textures.size();++index) {
            gpu_check(device.CreateTexture(index?width*2:width,height,1,0,D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,textures[index].put(),nullptr),"Create independent native/atlas/material source");
            D3DLOCKED_RECT lock{};
            gpu_check(textures[index]->LockRect(0,&lock,nullptr,0),"Lock mono source");
            for (UINT y=0;y<height;++y) for (UINT x=0;x<(index?width*2:width);++x) {
                const DWORD red=32+x%width*2+y;
                const DWORD pixel=index==2?0xff000000|((48+x/2)<<16):
                    index==1 && x>=width?0xff00ff00:0xff000000|(red<<16);
                reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=pixel;
            }
            gpu_check(textures[index]->UnlockRect(0),"Unlock mono source");
        }
        tag_atlas_texture(*textures[1].get(),OutlastTargetRole::scene_color,width,height);
        gpu_check(device.SetTexture(1,textures[2].get()),"Bind untagged material of the same physical size");
        for (UINT sampler=0;sampler<2;++sampler)
            for (auto setting:std::initializer_list<std::pair<D3DSAMPLERSTATETYPE,DWORD>>{
                {D3DSAMP_MINFILTER,D3DTEXF_POINT},{D3DSAMP_MAGFILTER,D3DTEXF_POINT},
                {D3DSAMP_MIPFILTER,D3DTEXF_NONE},{D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP},
                {D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP}})
                gpu_check(device.SetSamplerState(sampler,setting.first,setting.second),"Set mono source sampling");
        Com<IDirect3DSurface9> target,readback;
        gpu_check(device.GetRenderTarget(0,target.put()),"Read mono target");
        gpu_check(device.CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create mono readback");
        Com<IDirect3DTexture9> atlas_output;
        Com<IDirect3DSurface9> atlas_target,atlas_readback;
        gpu_check(device.CreateTexture(width*2,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,atlas_output.put(),nullptr),"Create independently tagged mono destination");
        tag_atlas_texture(*atlas_output.get(),OutlastTargetRole::scene_color,width,height);
        gpu_check(atlas_output->GetSurfaceLevel(0,atlas_target.put()),"Read mono atlas destination");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,atlas_readback.put(),nullptr),"Create complete destination atlas readback");
        auto pixels=[&] {
            gpu_check(device.GetRenderTargetData(target.get(),readback.get()),"Read complete mono output");
            D3DLOCKED_RECT lock{};
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Lock mono output");
            std::vector<DWORD> result(width*height);
            for (UINT y=0;y<height;++y) std::memcpy(result.data()+y*width,
                static_cast<const char*>(lock.pBits)+y*lock.Pitch,width*4);
            gpu_check(readback->UnlockRect(),"Unlock mono output");return result;
        };
        D3D9ShaderVariants variants(device);
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        const std::array<const char*,4> samples{{
            "tex2D(Scene,coordinate)","tex2Dproj(Scene,float4(coordinate*2,0,2))",
            "tex2Dgrad(Scene,coordinate,float2(1./64,0),float2(0,1./16))",
            "tex2Dlod(Scene,float4(coordinate,0,0))"}};
        unsigned checked=0,cases=0;
        for (const auto* sample:samples) {
            Com<ID3DBlob> code;
            compile(std::string(R"(
                sampler Scene:register(s0);sampler Material:register(s1);
                float4 Params:register(c0);
                float4 main(float2 uv:TEXCOORD0,float2 pixel:VPOS):COLOR {
                    float2 coordinate=lerp(uv,pixel*Params.xy,Params.z)+float2(Params.w,0);
                    float4 scene=)")+sample+R"(;
                    return float4(scene.r,tex2D(Material,uv).r,scene.g,1);
                })","ps_3_0",code);
            Com<IDirect3DPixelShader9> shader;
            gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),shader.put()),
                "Create original mono sample program");
            for (unsigned subview=0;subview<2;++subview) for (unsigned vpos=0;vpos<2;++vpos)
                for (int border=-1;border<=1;++border) {
                    const D3DVIEWPORT9 full{0,0,width,height,0,1};
                    const D3DVIEWPORT9 viewport=subview?D3DVIEWPORT9{3,2,40,12,0,1}:full;
                    const float uv[]{subview?40.f/width:1,subview?12.f/height:1,.5f/width,.5f/height};
                    const float params[]{1.f/width,1.f/height,float(vpos),float(border)};
                    gpu_check(device.SetVertexShaderConstantF(0,uv,1),"Set original native subview UVs");
                    gpu_check(device.SetPixelShader(shader.get()),"Bind original mono sample program");
                    gpu_check(device.SetPixelShaderConstantF(0,params,1),"Set native VPOS/border controls");
                    std::vector<DWORD> reference;
                    for (unsigned atlas=0;atlas<2;++atlas) {
                        gpu_check(device.SetViewport(&full),"Clear full mono target");
                        gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff091123,1,0),"Clear mono target");
                        gpu_check(device.SetViewport(&viewport),"Set original native viewport");
                        gpu_check(device.SetTexture(0,textures[atlas].get()),"Select native reference or atlas source");
                        gpu_check(device.BeginScene(),"Begin mono resolve");
                        const auto draw=[&] {return device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,quad,sizeof(quad[0]));};
                        gpu_check(atlas && !fault?variants.draw_mono(draw):draw(),"Draw one complete native mono view");
                        gpu_check(device.EndScene(),"End mono resolve");
                        Com<IDirect3DPixelShader9> restored;
                        gpu_check(device.GetPixelShader(restored.put()),"Read restored native mono shader");
                        float restored_params[4]{};
                        gpu_check(device.GetPixelShaderConstantF(0,restored_params,1),"Read restored native mono uniforms");
                        D3DVIEWPORT9 restored_viewport{};
                        gpu_check(device.GetViewport(&restored_viewport),"Read restored native mono viewport");
                        if (restored.get()!=shader.get() || std::memcmp(params,restored_params,sizeof(params)) ||
                            std::memcmp(&viewport,&restored_viewport,sizeof(viewport)))
                            throw std::runtime_error("Mono atlas correction changed native caller state");
                        const auto actual=pixels();
                        if (!atlas) reference=actual;
                        else for (unsigned i=0;i<actual.size();++i) {
                            if (actual[i]!=reference[i])
                                throw std::runtime_error("Whole mono view differs from independent non-atlased native reference");
                            ++checked;
                        }
                    }
                    ++cases;
                    for (unsigned atlas=0;atlas<2;++atlas) {
                        gpu_check(device.SetRenderTarget(0,atlas_target.get()),"Bind native mono atlas destination");
                        gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff091123,1,0),"Seed both atlas halves independently");
                        auto physical=viewport;physical.X*=2;physical.Width*=2;
                        gpu_check(device.SetViewport(&physical),"Reproduce the native physical atlas viewport");
                        gpu_check(device.SetTexture(0,textures[atlas].get()),"Select mono or atlas input independently of destination");
                        gpu_check(device.BeginScene(),"Begin owned mono atlas draw");
                        gpu_check(variants.draw_mono([&] {
                            return device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,quad,sizeof(quad[0]));
                        },!fault_destination),"Draw native mono geometry into one logical atlas view");
                        gpu_check(device.EndScene(),"End owned mono atlas draw");
                        D3DVIEWPORT9 restored_viewport{};
                        gpu_check(device.GetViewport(&restored_viewport),"Read restored physical atlas viewport");
                        Com<IDirect3DPixelShader9> restored;
                        Com<IDirect3DBaseTexture9> restored_texture;
                        float restored_params[4]{};
                        gpu_check(device.GetPixelShader(restored.put()),"Read restored atlas destination shader");
                        gpu_check(device.GetTexture(0,restored_texture.put()),"Read restored atlas destination input");
                        gpu_check(device.GetPixelShaderConstantF(0,restored_params,1),"Read restored atlas destination uniforms");
                        if (std::memcmp(&physical,&restored_viewport,sizeof(physical)) ||
                            restored.get()!=shader.get() || restored_texture.get()!=textures[atlas].get() ||
                            std::memcmp(params,restored_params,sizeof(params)))
                            throw std::runtime_error("Owned mono atlas draw changed native caller state");
                        gpu_check(device.GetRenderTargetData(atlas_target.get(),atlas_readback.get()),
                            "Read both actual mono destination atlas halves");
                        D3DLOCKED_RECT lock{};
                        gpu_check(atlas_readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Lock complete mono destination");
                        bool different=false;
                        for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
                            const auto actual=reinterpret_cast<const DWORD*>(
                                static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                            const auto expected=x<width?reference[y*width+x]:DWORD(0xff091123);
                            different|=actual!=expected;++checked;
                        }
                        gpu_check(atlas_readback->UnlockRect(),"Unlock complete mono destination");
                        if (different)
                            throw std::runtime_error("Native mono atlas destination differs from its complete original-shader reference or overwrote the other eye");
                        ++cases;
                        gpu_check(device.SetRenderTarget(0,target.get()),"Restore independent non-atlased reference target");
                    }
                }
        }
        std::cout<<"PASS: "<<cases<<" mono resolves, "<<checked
            <<" full-frame reference pixels; UV, VPOS, subviews, borders, projected/gradient/LOD samples, mono atlas destinations, material isolation and native state.\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
