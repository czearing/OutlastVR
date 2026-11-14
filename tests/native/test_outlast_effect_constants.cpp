#include "isolated_d3d9.h"
#include "d3d9_shader_variants.h"
#include <iostream>
using namespace olvr;
int main() {
    try {
        auto require=[](bool pass,const char* message) {
            if (!pass) throw std::runtime_error(message);
        };
        auto compile=[](const char* text,const char* profile) {
            Com<ID3DBlob> code,errors;
            const auto result=D3DCompile(text,std::strlen(text),"live-effects-test",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(result)) throw std::runtime_error(errors.get()?
                static_cast<const char*>(errors->GetBufferPointer()):"Cannot compile live-effects test");
            sm3::Words words(code->GetBufferSize()/4);
            std::memcpy(words.data(),code->GetBufferPointer(),code->GetBufferSize());return words;
        };
        const auto vertex=compile(R"(
            float4x4 ViewProjectionMatrix:register(c0);
            float4 main(float2 p:POSITION):POSITION {
                return mul(float4(p,0,1),ViewProjectionMatrix);
            })","vs_3_0");
        const auto pixel=compile(R"(
            float4 BloomScaleAndThreshold:register(c10);
            float4 BloomTintAndScreenBlendThreshold:register(c11);
            float4 GrainParams:register(c12);
            float4 ImageAdjustments1:register(c13);
            float Scene_ImageGrainScale:register(c14);
            float4 main():COLOR {
                return float4(BloomScaleAndThreshold.x+BloomTintAndScreenBlendThreshold.z,
                    GrainParams.y+ImageAdjustments1.w+Scene_ImageGrainScale,
                    GrainParams.w+.1*(BloomTintAndScreenBlendThreshold.w+
                        BloomScaleAndThreshold.z+ImageAdjustments1.x),
                    BloomScaleAndThreshold.y+.1);
            })","ps_3_0");
        IsolatedDevice isolated(64,32);
        auto& device=*isolated.device.get();
        Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
        gpu_check(device.CreateVertexShader(reinterpret_cast<const DWORD*>(vertex.data()),vs.put()),
            "Create live-effect test vertex shader");
        gpu_check(device.CreatePixelShader(reinterpret_cast<const DWORD*>(pixel.data()),ps.put()),
            "Create native-named live-effect test pixel shader");
        gpu_check(device.SetVertexShader(vs.get()),"Bind live-effect vertex shader");
        gpu_check(device.SetPixelShader(ps.get()),"Bind live-effect pixel shader");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create live-effect declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind live-effect declaration");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable live-effect test depth");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable live-effect test culling");
        const float matrix[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        gpu_check(device.SetVertexShaderConstantF(0,matrix,4),"Bind live-effect test projection");
        StereoViewConstants eyes{};
        for (auto& eye:eyes) std::copy_n(matrix,16,eye.begin());
        const float values[]{.1f,.2f,.3f,.4f,.1f,.2f,.3f,.4f,.1f,.2f,.3f,.4f,
            .1f,.2f,.3f,.4f,.07f,.08f,.09f,.1f};
        gpu_check(device.SetPixelShaderConstantF(10,values,5),"Bind independent authored effect channels");
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        Com<IDirect3DSurface9> output,readback;
        gpu_check(device.GetRenderTarget(0,output.put()),"Get live-effects output");
        gpu_check(device.CreateOffscreenPlainSurface(64,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create live-effects pixel readback");
        D3D9ShaderVariants variants(device);
        unsigned checked=0;
        for (const auto policy:{std::pair{true,true},std::pair{false,true},std::pair{true,false},
            std::pair{false,false},std::pair{true,true}}) {
            variants.effects(policy.first,policy.second);
            for (const bool stereo:{true,false}) {
                gpu_check(device.BeginScene(),"Begin live-effects test draw");
                if (stereo)
                    variants.current(0,false,false).draw_primitive_up(D3DPT_TRIANGLELIST,2,
                        quad,sizeof(quad),sizeof(quad[0]),eyes);
                else gpu_check(variants.draw_mono([&] {
                    return device.DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,quad,sizeof(quad[0]));
                }),"Draw native mono live-effects policy");
                gpu_check(device.EndScene(),"End live-effects test draw");
                float restored[20]{};
                gpu_check(device.GetPixelShaderConstantF(10,restored,5),"Read restored authored settings");
                require(std::memcmp(values,restored,sizeof(values))==0,
                    "Live effect toggle leaked into another native effect or caller");
                gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read live-effect GPU pixels");
                D3DLOCKED_RECT lock{};
                gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Lock live-effects GPU pixels");
                bool correct=true;
                for (unsigned y=0;y<32;++y) for (unsigned x=0;x<64;++x) {
                    const auto color=reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch)[x];
                    correct&=std::abs(int((color>>16)&255)-(policy.first?.4f:0)*255)<2 &&
                        std::abs(int((color>>8)&255)-(policy.second?.67f:.07f)*255)<2 &&
                        std::abs(int(color&255)-.48f*255)<2 &&
                        std::abs(int(color>>24)-.3f*255)<2;
                    ++checked;
                }
                gpu_check(readback->UnlockRect(),"Unlock live-effects GPU pixels");
                require(correct,"Live bloom/grain toggle changed the wrong native shader channels");
            }
        }
        std::cout<<"PASS: live_native_bloom_and_grain; checked_GPU_pixels="<<checked
            <<"; both_eyes_and_mono_capture; repeated_cache_changes; "
            "DOF_limits_bloom_threshold_script_time_and_other_channels_preserved; caller_state_restored\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;
    }
}
