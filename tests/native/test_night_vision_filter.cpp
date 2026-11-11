#include "isolated_d3d9.h"
#include "d3d9_night_vision.h"
#include "isolated_d3d12_counter.h"
#include <cmath>
#include <iostream>

using namespace olvr;
int main() {
    try {
        constexpr UINT width=128,height=32;
        IsolatedDevice isolated(width,height);
        auto& device=*isolated.device.get();
        Com<IDirect3DSurface9> target,upload,readback;
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,target.put()),"Get filter fixture");
        gpu_check(device.CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            upload.put(),nullptr),"Create native filter upload");
        gpu_check(device.CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create native filter readback");
        D3D9NightVision filter(device);
        IsolatedD3D12DrawCounter counter(isolated.translation_device(),true,isolated.translation_queue());
        unsigned checked=0;
        for (unsigned repeat=0;repeat<5;++repeat) {
            const bool night_vision=repeat!=3;
            const float vignette=repeat>=3?.8f:0.f;
            D3DLOCKED_RECT lock{};
            gpu_check(upload->LockRect(&lock,nullptr,0),"Write different-eye native filter inputs");
            auto input=[&](UINT x,UINT y) {
                const DWORD value=32+(x*3+y*7+repeat*19)%180;
                return 0xff000000u|(x<width/2?value<<16:value);
            };
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x)
                reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=input(x,y);
            gpu_check(upload->UnlockRect(),"Finish native filter inputs");
            gpu_check(device.UpdateSurface(upload.get(),nullptr,target.get(),nullptr),"Upload exact filter pixels");
            const D3DVIEWPORT9 viewport{3,4,50,20,.1f,.9f};
            const RECT scissor{7,8,20,22};
            gpu_check(device.SetViewport(&viewport),"Set caller viewport");
            gpu_check(device.SetScissorRect(&scissor),"Set caller scissor");
            gpu_check(device.SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE),"Set caller scissor state");
            const DWORD wrapping=repeat==0?0:repeat==1?D3DWRAP_U:repeat==2?D3DWRAP_V:D3DWRAP_U|D3DWRAP_V;
            gpu_check(device.SetRenderState(D3DRS_WRAP0,wrapping),"Set caller texture wrapping");
            counter.reset();
            filter.draw(*target.get(),night_vision,vignette);
            gpu_check(device.GetRenderTargetData(target.get(),readback.get()),"Read real GPU filter result");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize real GPU filter pixels");
            bool equal=true;
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
                const DWORD value=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                const auto original=input(x,y);
                const double luminance=(.2126*((original>>16)&255)+.0722*(original&255))/255.;
                const double green=std::pow(std::min(1.,luminance*1.6),.65)*255.;
                const double eye_x=(double(x%(width/2))+.5)/(width/2)*2-1;
                const double eye_y=(double(y)+.5)/height*2-1;
                const double radius=std::sqrt(eye_x*eye_x+eye_y*eye_y);
                const double edge=std::clamp((radius-.55)/.45,0.,1.);
                const double visibility=1-vignette*edge*edge*(3-2*edge);
                for (unsigned channel=0;channel<3;++channel) {
                    const auto factor=channel==0?.22:channel==1?1.:.12;
                    const double expected=(night_vision?green*factor:
                        double((original>>(channel*8))&255))*visibility;
                    if (std::abs(int((value>>(channel*8))&255)-expected)>2.) equal=false;
                }
                if ((value>>24)!=255) equal=false;
                ++checked;
            }
            gpu_check(readback->UnlockRect(),"Finish real GPU filter readback");
            if (!equal) throw std::runtime_error("Filter changed eye ownership, alpha or independent luminance oracle");
            D3DVIEWPORT9 after{};RECT after_scissor{};DWORD enabled=0,after_wrapping=0;
            gpu_check(device.GetViewport(&after),"Read restored viewport");
            gpu_check(device.GetScissorRect(&after_scissor),"Read restored scissor");
            gpu_check(device.GetRenderState(D3DRS_SCISSORTESTENABLE,&enabled),"Read restored scissor state");
            gpu_check(device.GetRenderState(D3DRS_WRAP0,&after_wrapping),"Read restored wrapping state");
            if (std::memcmp(&after,&viewport,sizeof(after)) ||
                std::memcmp(&after_scissor,&scissor,sizeof(scissor)) || enabled!=TRUE ||
                after_wrapping!=wrapping)
                throw std::runtime_error("Filter leaked graphics state into the native renderer");
            const auto draws=counter.events();
            if (draws.size()!=1 || draws[0].instances!=1) {
                for (const auto& event:draws)
                    std::cerr<<"GPU event indexed="<<event.indexed<<"; elements="<<event.elements
                        <<"; instances="<<event.instances<<'\n';
                throw std::runtime_error("Full-atlas filter replayed eye geometry or submitted duplicate draws");
            }
        }
        std::cout<<"PASS: "<<checked<<" actual GPU pixels; both eyes preserved; one atlas filter draw; caller state restored\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
