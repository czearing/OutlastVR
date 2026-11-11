#include "isolated_d3d9.h"
#include "world_panel.h"
#ifdef OLVR_ISOLATED_9ON12
#include "isolated_d3d12_counter.h"
#endif
#include <iostream>

using namespace olvr;
int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-headlocked";
        const bool mono=argc==2 && std::string(argv[1])=="--fault-mono";
        const bool canted=argc==2 && std::string(argv[1])=="--canted";
        if (argc!=1 && !fault && !mono && !canted) throw std::invalid_argument("Unknown spatial panel control");
        constexpr UINT width=128,height=96,source_width=160,source_height=90;
        IsolatedDevice isolated(width*2,height);
        auto& device=*isolated.device.get();
        Com<IDirect3DTexture9> source;
        gpu_check(device.CreateTexture(source_width*2,source_height,1,0,D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED,source.put(),nullptr),"Create panel image fixture");
        D3DLOCKED_RECT lock{};
        gpu_check(source->LockRect(0,&lock,nullptr,0),"Lock panel image fixture");
        for (UINT y=0;y<source_height;++y) for (UINT x=0;x<source_width*2;++x) {
            const UINT r=UINT(std::lround(13+220*(x+.5)/source_width));
            const UINT g=UINT(std::lround(13+220*(y+.5)/source_height));
            reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=
                x<source_width?(0xff000020|(r<<16)|(g<<8)):0xffff00ff;
        }
        gpu_check(source->UnlockRect(0),"Unlock panel image fixture");
        Com<IDirect3DSurface9> output,readback;
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,output.put()),"Get panel output");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create panel readback");
        D3D9WorldPanel renderer(device);
        WorldPanel panel;
        const std::array<EyeFov,2> fovs=canted?
            std::array<EyeFov,2>{{{-.95f,.55f,.72f,-.69f},{-.53f,.91f,.78f,-.65f}}}:
            std::array<EyeFov,2>{{{-.8f,.8f,.8f,-.8f},{-.8f,.8f,.8f,-.8f}}};
        double half_height=1e9;
        for (unsigned eye=0;eye<2;++eye) {
            const double cant=canted?(eye?.06:-.06):0,c=std::cos(cant),s=std::sin(cant);
            const auto& f=fovs[eye];
            const double planes[][3]{
                {c+s*std::tan(double(f.left)),0,-s+c*std::tan(double(f.left))},
                {-c-s*std::tan(double(f.right)),0,s-c*std::tan(double(f.right))},
                {s*std::tan(double(f.down)),1,c*std::tan(double(f.down))},
                {-s*std::tan(double(f.up)),-1,-c*std::tan(double(f.up))}};
            for (const auto& n:planes) {
                const double available=n[0]*(eye?-.032:.032)-2*n[2];
                half_height=std::min(half_height,available/((16./9)*std::abs(n[0])+std::abs(n[1])));
            }
        }
        UINT tested=0,outside=0;
#ifdef OLVR_ISOLATED_9ON12
        IsolatedD3D12DrawCounter counter(isolated.translation_device());
#endif
        for (unsigned frame=0;frame<5;++frame) {
            const float yaw=frame==1?.25f:0,pitch=frame==2?-.18f:0,roll=frame==4?.3f:0;
            const Quaternion q{
                std::sin(pitch/2)*std::cos(yaw/2)*std::cos(roll/2)+std::cos(pitch/2)*std::sin(yaw/2)*std::sin(roll/2),
                std::cos(pitch/2)*std::sin(yaw/2)*std::cos(roll/2)-std::sin(pitch/2)*std::cos(yaw/2)*std::sin(roll/2),
                std::cos(pitch/2)*std::cos(yaw/2)*std::sin(roll/2)-std::sin(pitch/2)*std::sin(yaw/2)*std::cos(roll/2),
                std::cos(pitch/2)*std::cos(yaw/2)*std::cos(roll/2)+std::sin(pitch/2)*std::sin(yaw/2)*std::sin(roll/2)};
            const Vec3 center=frame==3?Vec3{.2f,.1f,.25f}:Vec3{};
            std::array<EyePose,2> eyes{};
            for (unsigned eye=0;eye<2;++eye) {
                const float cant=canted?(eye?.06f:-.06f):0;
                eyes[eye]={add(center,rotate(q,{eye?.032f:-.032f,0,0})),
                    multiply(q,{0,std::sin(cant/2),0,std::cos(cant/2)})};
            }
            if (fault) panel.reset();
            auto matrices=panel.locate(eyes,fovs,16.f/9);
            if (mono) matrices[1]=matrices[0];
#ifdef OLVR_ISOLATED_9ON12
            counter.reset();
#endif
            renderer.draw(*source.get(),*output.get(),width,height,matrices);
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read spatial panel pixels");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Synchronize spatial panel pixels");
            bool correct=true;
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
                const auto& eye=eyes[x/width];
                const auto& f=fovs[x/width];
                const double rx=std::tan(double(f.left))+
                    (std::tan(double(f.right))-std::tan(double(f.left)))*(x%width)/width;
                const double ry=std::tan(double(f.up))+
                    (std::tan(double(f.down))-std::tan(double(f.up)))*y/height;
                const auto rotation=eye.orientation;
                const double qx=rotation.x,qy=rotation.y,qz=rotation.z,qw=rotation.w;
                const double dx=(1-2*qy*qy-2*qz*qz)*rx+(2*qx*qy-2*qz*qw)*ry-(2*qx*qz+2*qy*qw);
                const double dy=(2*qx*qy+2*qz*qw)*rx+(1-2*qx*qx-2*qz*qz)*ry-(2*qy*qz-2*qx*qw);
                const double dz=(2*qx*qz-2*qy*qw)*rx+(2*qy*qz+2*qx*qw)*ry-(1-2*qx*qx-2*qy*qy);
                const double t=(-2-eye.position.z)/dz;
                const double u=((eye.position.x+t*dx)/(half_height*16./9)+1)/2;
                const double v=(1-(eye.position.y+t*dy)/half_height)/2;
                const auto color=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                if (u>.05 && u<.95 && v>.05 && v<.95 && t>0) {
                    const int r=int(std::lround(13+220*u)),g=int(std::lround(13+220*v));
                    if (correct && (std::abs(int((color>>16)&255)-r)>2 ||
                        std::abs(int((color>>8)&255)-g)>2 || (color&255)!=32))
                        std::cerr<<"Panel mismatch frame="<<frame<<" x="<<x<<" y="<<y
                            <<" actual="<<std::hex<<color<<std::dec<<" expected="<<r<<','<<g<<",32\n";
                    correct &= std::abs(int((color>>16)&255)-r)<=2 &&
                        std::abs(int((color>>8)&255)-g)<=2 && (color&255)==32;
                    ++tested;
                } else if (u<-.05 || u>1.05 || v<-.05 || v>1.05 || t<0) {
                    if (correct && color!=0xff000000)
                        std::cerr<<"Panel outside mismatch frame="<<frame<<" x="<<x<<" y="<<y
                            <<" uv="<<u<<','<<v<<" actual="<<std::hex<<color<<std::dec<<'\n';
                    correct &= color==0xff000000;
                    ++outside;
                }
            }
            gpu_check(readback->UnlockRect(),"Unlock spatial panel pixels");
            if (!correct) throw std::runtime_error("World panel moved with the head or sampled the wrong source eye");
#ifdef OLVR_ISOLATED_9ON12
            if (counter.indexed_calls!=1 || counter.plain_calls || counter.instances!=2)
                throw std::runtime_error("World panel was not one physical draw with two eye instances");
#endif
        }
        if (fault || tested<10000 || outside<10000)
            throw std::runtime_error("Spatial panel control or pixel coverage failed");
        panel.reset();
        const std::array<EyePose,2> pointer_eyes{{{{-.032f,0,0},{0,0,0,1}},
            {{.032f,0,0},{0,0,0,1}}}};
        const TrackingFrame identity{{0,0,0},{1,0,0},{0,1,0},{0,0,1},1,{0,0,0}};
        StereoViewConstants pointer_views{};
        for (unsigned eye=0;eye<2;++eye)
            pointer_views[eye]=eye_constants(identity,pointer_eyes[eye],fovs[eye],.01f,1000);
        for (bool hit:{false,true}) {
            renderer.draw(*source.get(),*output.get(),width,height,panel.locate(pointer_eyes,fovs,16.f/9));
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read pre-pointer menu");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Lock pre-pointer menu");
            std::vector<DWORD> original_pixels(width*2*height);
            for (UINT y=0;y<height;++y)
                std::memcpy(original_pixels.data()+y*width*2,
                    static_cast<const char*>(lock.pBits)+y*lock.Pitch,width*2*sizeof(DWORD));
            gpu_check(readback->UnlockRect(),"Unlock pre-pointer menu");
            const float caller_color[]{.3f,.2f,.1f,.5f};
            gpu_check(device.SetPixelShaderConstantF(2,caller_color,1),"Seed caller pixel constants");
#ifdef OLVR_ISOLATED_9ON12
            counter.reset();
#endif
            renderer.draw_pointer(*output.get(),width,height,pointer_views,
                {.15f,-.15f,-.25f},{.1f,.3f,-2},hit);
            float restored_color[4]{};
            gpu_check(device.GetPixelShaderConstantF(2,restored_color,1),"Read restored caller pixel constants");
            if (std::memcmp(restored_color,caller_color,sizeof(caller_color))!=0)
                throw std::runtime_error("Menu ray corrupted caller shader constants");
            gpu_check(device.GetRenderTargetData(output.get(),readback.get()),"Read menu ray pixels");
            gpu_check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"Lock menu ray pixels");
#ifdef OLVR_ISOLATED_9ON12
            if (counter.indexed_calls!=1 || counter.plain_calls || counter.instances!=2)
                throw std::runtime_error("Controller ray replayed geometry instead of one stereo draw");
#endif
            std::array<UINT,2> ray_pixels{};
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
                const auto color=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x];
                if (color==original_pixels[y*width*2+x]) continue;
                const int red=hit?26:140,green=hit?255:179;
                if (std::abs(int((color>>16)&255)-red)>1 ||
                    std::abs(int((color>>8)&255)-green)>1 || (color&255)!=255)
                    throw std::runtime_error("Controller ray cleared or damaged the native menu image");
                ++ray_pixels[x/width];
            }
            gpu_check(readback->UnlockRect(),"Unlock menu ray pixels");
            for (const auto count:ray_pixels)
                if (count<2 || count>width*height/20)
                    throw std::runtime_error("Menu ray is absent from an eye or covers too much of the panel");
        }
        std::cout<<"PASS: fixed two-meter world panel, mono source, yaw/pitch/roll/translation; pixels="
            <<tested<<"; surrounding_pixels="<<outside
            <<"; two_eye_visible_controller_rays; menu_pixels_and_caller_constants_preserved"
            <<"; one_physical_draw_two_eye_instances_per_panel_or_ray\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
