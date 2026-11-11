#include "isolated_d3d9.h"
#include "world_panel.h"
#include "outlast_camcorder.h"
#ifdef OLVR_ISOLATED_9ON12
#include "isolated_d3d12_counter.h"
#endif
#include <iostream>

using namespace olvr;
int main(int argc,char** argv) {
    try {
        const bool hud=argc==2 && std::string(argv[1])=="--hud";
        const bool native_hud=argc==2 && (std::string(argv[1])=="--native-hud" ||
            std::string(argv[1])=="--fault-missing-native-hud");
        const bool night_vision=hud || (argc==2 && std::string(argv[1])=="--night-vision");
        const std::string fault=argc==2 && !night_vision && std::string(argv[1])!="--native-hud"?argv[1]:"";
        if (argc>2 || (!fault.empty() && fault!="--fault-atlas-source" &&
            fault!="--fault-missing-occlusion" && fault!="--fault-headlocked" &&
            fault!="--fault-missing-native-hud"))
            throw std::invalid_argument("Unknown physical viewfinder control");
        constexpr UINT width=256,height=192,source_width=128,source_height=72;
        constexpr DWORD background=0xff111f31;
        IsolatedDevice isolated(width*2,height);
        auto& device=*isolated.device.get();
        Com<IDirect3DTexture9> source;
        gpu_check(device.CreateTexture(source_width,source_height,1,0,D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED,source.put(),nullptr),"Create independent lens image fixture");
        D3DLOCKED_RECT lock{};
        gpu_check(source->LockRect(0,&lock,nullptr,0),"Lock independent lens image");
        for (UINT y=0;y<source_height;++y) for (UINT x=0;x<source_width;++x) {
            const DWORD red=DWORD(std::lround(30+180*(x+.5)/source_width));
            const DWORD green=DWORD(std::lround(40+170*(y+.5)/source_height));
            reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=
                0xff00000b|(red<<16)|(green<<8);
        }
        gpu_check(source->UnlockRect(0),"Unlock independent lens image");
        Com<IDirect3DTexture9> movie;
        if (native_hud) {
            gpu_check(device.CreateTexture(source_width,source_height,1,0,D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,movie.put(),nullptr),"Create independent premultiplied native HUD fixture");
            gpu_check(movie->LockRect(0,&lock,nullptr,0),"Lock native HUD fixture");
            for (UINT y=0;y<source_height;++y) for (UINT x=0;x<source_width;++x)
                reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=0x80401808;
            gpu_check(movie->UnlockRect(0),"Unlock native HUD fixture");
        }
        Com<IDirect3DSurface9> output,depth,readback;
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,output.put()),"Get physical screen output");
        gpu_check(device.CreateDepthStencilSurface(width*2,height,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,
            0,TRUE,depth.put(),nullptr),"Create native-shaped stereo world depth");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
            readback.put(),nullptr),"Create physical screen readback");
        D3D9WorldPanel renderer(device);
        const std::array<EyeFov,2> fovs{{{-.65f,.6f,.6f,-.6f},{-.6f,.65f,.6f,-.6f}}};
        UINT checked=0,outside=0,blocked=0,battery_pixels=0,recording_pixels=0;
#ifdef OLVR_ISOLATED_9ON12
        IsolatedD3D12DrawCounter counter(isolated.translation_device());
#endif
        for (unsigned frame=0;frame<6;++frame) {
            const float yaw=frame==1?.15f:frame==2?-.2f:0;
            const Quaternion head_rotation{0,std::sin(yaw/2),0,std::cos(yaw/2)};
            const Vec3 head=frame==2?Vec3{.025f,.015f,0}:Vec3{};
            std::array<EyePose,2> eyes{};
            for (unsigned eye=0;eye<2;++eye)
                eyes[eye]={add(head,rotate(head_rotation,{eye?.032f:-.032f,0,0})),head_rotation};
            const float screen_yaw=frame==3?.35f:0;
            const EyePose screen{{frame==3?-.025f:.015f,0,-.35f},
                {0,std::sin(screen_yaw/2),0,std::cos(screen_yaw/2)}};
            auto drawn=screen;
            if (fault=="--fault-headlocked")
                drawn.position=add(head,rotate(head_rotation,screen.position));
            const auto matrices=camcorder_screen_constants(drawn,eyes,fovs);
            gpu_check(device.SetDepthStencilSurface(depth.get()),"Bind physical world-depth fixture");
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,
                background,1,0),"Initialize unrelated world and depth");
            const bool occlude=frame>=4;
            const D3DRECT left_eye{0,0,LONG(width),LONG(height)};
            if (occlude && fault!="--fault-missing-occlusion")
                gpu_check(device.Clear(1,&left_eye,D3DCLEAR_ZBUFFER,0,.1f,0),"Place nearer world occluder in left eye");
            const float sentinel[]{.17f+float(frame)*.01f,.31f,.49f,.73f};
            const DWORD caller_depth_write=frame%2?FALSE:TRUE;
            gpu_check(device.SetPixelShaderConstantF(0,sentinel,1),"Install caller shader-constant sentinel");
            gpu_check(device.SetRenderState(D3DRS_ZWRITEENABLE,caller_depth_write),"Install changed caller depth-write sentinel");
            gpu_check(device.SetTexture(1,source.get()),"Install caller second-sampler sentinel");
#ifdef OLVR_ISOLATED_9ON12
            counter.reset();
#endif
            if (fault=="--fault-atlas-source")
                renderer.draw(*source.get(),*output.get(),width,height,matrices);
            else renderer.draw_screen(*source.get(),*output.get(),*depth.get(),width,height,matrices,
                night_vision,hud?float(frame)*.2f:native_hud?.8f:-1,
                fault=="--fault-missing-native-hud"?nullptr:movie.get());
            float restored[4]{};
            gpu_check(device.GetPixelShaderConstantF(0,restored,1),"Read restored caller constants");
            Com<IDirect3DSurface9> restored_depth;
            gpu_check(device.GetDepthStencilSurface(restored_depth.put()),"Read restored caller depth");
            DWORD depth_write=0;
            gpu_check(device.GetRenderState(D3DRS_ZWRITEENABLE,&depth_write),"Read restored caller depth writes");
            Com<IDirect3DBaseTexture9> restored_movie;
            gpu_check(device.GetTexture(1,restored_movie.put()),"Read restored caller second sampler");
            if (std::memcmp(sentinel,restored,sizeof(sentinel)) || restored_depth.get()!=depth.get() ||
                depth_write!=caller_depth_write || restored_movie.get()!=source.get())
                throw std::runtime_error("Physical screen did not restore changed caller graphics state");
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read physical viewfinder pixels");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize physical viewfinder pixels");
            bool correct=true;
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
                const UINT eye=x/width;
                const double u_eye=double(x%width)/width,v_eye=double(y)/height;
                const double tx=std::tan(double(fovs[eye].left))*(1-u_eye)+std::tan(double(fovs[eye].right))*u_eye;
                const double ty=std::tan(double(fovs[eye].up))*(1-v_eye)+std::tan(double(fovs[eye].down))*v_eye;
                const double dx=std::cos(yaw)*tx-std::sin(yaw),dy=ty;
                const double dz=-std::sin(yaw)*tx-std::cos(yaw);
                const auto origin=eyes[eye].position;
                const double nx=std::sin(screen_yaw),nz=std::cos(screen_yaw);
                const double t=((screen.position.x-origin.x)*nx+(screen.position.z-origin.z)*nz)/(dx*nx+dz*nz);
                const double hit_x=origin.x+t*dx-screen.position.x;
                const double hit_y=origin.y+t*dy-screen.position.y;
                const double hit_z=origin.z+t*dz-screen.position.z;
                const double u=.5+(std::cos(screen_yaw)*hit_x-std::sin(screen_yaw)*hit_z)/.09;
                const double v=.5-hit_y/.050625;
                const DWORD color=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                const double inset=hud || native_hud?.02:.08;
                if (t>0 && u>inset && u<1-inset && v>inset && v<1-inset) {
                    if (occlude && eye==0) {
                        correct &= color==background;++blocked;
                    } else {
                        double red=30+180*u,green=40+170*v,blue=11;
                        if (night_vision) {
                            const double luminance=(red*.2126+green*.7152+blue*.0722)/255;
                            green=255*std::pow(std::min(1.,luminance*1.6),.65)*
                                (1-.7*((u-.5)*(u-.5)+(v-.5)*(v-.5)));
                            red=green*.12;blue=green*.22;
                        }
                        if (hud) {
                            const double bx=std::abs(u-.88),by=std::abs(v-.075);
                            const bool outline=bx<.075 && by<.028 && (bx>.069 || by>.022);
                            const bool terminal=u>.955 && u<.964 && v>.063 && v<.087;
                            const bool charge=u>.814 && u<.814+.132*float(frame)*.2f && v>.06 && v<.09;
                            if (outline || terminal || charge) {
                                red=green=blue=255;++battery_pixels;
                            }
                            const double rx=u-.05,ry=(v-.075)*1.77777778;
                            if (rx*rx+ry*ry<.00012) {
                                red=255;green=.08*255;blue=.04*255;++recording_pixels;
                            }
                        }
                        if (native_hud) {
                            red=64+red*(127./255);green=24+green*(127./255);blue=8+blue*(127./255);
                        }
                        const bool pixel_matches=std::abs(int((color>>16)&255)-int(std::lround(red)))<=3 &&
                            std::abs(int((color>>8)&255)-int(std::lround(green)))<=3 &&
                            std::abs(int(color&255)-int(std::lround(blue)))<=3;
                        if (!pixel_matches && correct)
                            std::cerr<<"First screen mismatch: frame="<<frame<<"; xy="<<x<<','<<y
                                <<"; uv="<<u<<','<<v<<"; actual="<<((color>>16)&255)<<','
                                <<((color>>8)&255)<<','<<(color&255)<<"; expected="<<red<<','<<green<<','<<blue<<'\n';
                        correct &= pixel_matches;
                        ++checked;
                    }
                } else if (t<=0 || u<-.08 || u>1.08 || v<-.08 || v>1.08) {
                    correct &= color==background;++outside;
                }
            }
            gpu_check(readback->UnlockRect(),"Unlock physical screen readback");
            if (!correct) throw std::runtime_error("Physical screen changed the world, sampled a headset eye, lost lens pose or ignored world occlusion");
#ifdef OLVR_ISOLATED_9ON12
            if (counter.indexed_calls!=1 || counter.plain_calls || counter.instances!=2)
                throw std::runtime_error("Physical viewfinder was not one GPU draw with two eye instances");
#endif
        }
        if (checked<1500 || outside<400000 || blocked<300 || !fault.empty() ||
            (hud && (battery_pixels<10 || recording_pixels<2)))
            throw std::runtime_error("Physical viewfinder coverage/control gate failed");
        if (renderer.state_allocations()!=1 || renderer.state_captures()!=6)
            throw std::runtime_error("Panel caller state was not recaptured for every draw using one allocation");
        std::cout<<"PASS: physical_viewfinder_pixels="<<checked<<"; unchanged_world_pixels="<<outside
            <<"; depth_occluded_pixels="<<blocked
            <<"; battery_pixels="<<battery_pixels<<"; recording_pixels="<<recording_pixels
            <<"; native_hud_alpha_fixture="<<native_hud
            <<"; independent_mono_source; tracked_screen_not_HMD; per_eye_parallax; caller_state_restored;"
            <<" physical_draws=6; eye_instances=12; caller_state_allocations=1; caller_state_captures=6;"
            <<" synthetic_lens_fixture_not_native_capture\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
