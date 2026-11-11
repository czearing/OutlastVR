#include "native_camcorder_probe.h"
#include "isolated_d3d9.h"
#include <chrono>
#include <iostream>
#include <thread>

using namespace olvr;
namespace {
unsigned checks=0,constructed=0,attached=0,detached=0,views=0,projections=0,closed=0;
BYTE capture_class_token=0,target_class_token=0;
std::vector<void*> pages;
void* last_component=nullptr;
void require(bool value,const char* message) {
    ++checks;if (!value) throw std::runtime_error(message);
}
template<class T> void write(void* object,std::size_t offset,T value) {
    std::memcpy(static_cast<BYTE*>(object)+offset,&value,sizeof(value));
}
void* page() {
    auto* result=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!result) throw std::runtime_error("Cannot allocate isolated native object fixture");
    pages.push_back(result);return result;
}
void WINAPI attach(void* owner,void* component) {
    require(!skeleton_field<void*>(component,0x78),"Capture was attached twice");
    require(!skeleton_field<UINT>(component,0xe4),"Engine matrix updates would overwrite the tracked lens");
    write(component,0x78,owner);++attached;
    if (skeleton_field<UINT>(component,0x88)&2) {
        require(skeleton_field<void*>(component,0x9c),"Postprocessing was enabled without a native chain");
        write(component,0xc8,INT(2));
    }
}
void WINAPI detach(void* owner,void* component) {
    require(skeleton_field<void*>(component,0x78)==owner,"Detached an unowned native capture");
    write(component,0x78,static_cast<void*>(nullptr));++detached;
}
void* WINAPI construct(void* cls,void* owner,UINT64 name,UINT64 flags,void*,void*,void*,void*) {
    require(owner && !name && flags==0x400000000000,"Native capture factory ownership/flags changed");
    require(cls==&capture_class_token || cls==&target_class_token,"Unknown native capture class");
    auto* object=page();++constructed;
    if (cls==&capture_class_token) last_component=object;
    return object;
}
void WINAPI initialize(void* target,UINT width,UINT height,BYTE format,UINT extra) {
    require(width==512 && height==288 && format==2 && !extra,"Native mono target profile changed");
    auto* resource=page();
    write(target,0xf8,resource);write(resource,0x24,std::uintptr_t(0x1000+constructed));
}
void WINAPI projection(void* component) {
    require(skeleton_field<UINT>(component,0xe4)==1,"Native lens projection was not initialized");
    require(skeleton_field<float>(component,0xd8)==70,"Native optical FOV changed");
    ++projections;
}
void WINAPI view(void*,const Vec3* position,const INT* rotation) {
    require(finite(*position) && rotation[1]==1234,"Tracked native lens pose was not passed through");
    ++views;
}
void protect(DWORD mode) {
    for (auto* object:pages) {
        DWORD previous=0;
        if (!VirtualProtect(object,4096,mode,&previous))
            throw std::runtime_error("Cannot protect isolated native object fixture");
    }
}
}
int main(int argc,char** argv) {
    try {
        const bool sample_readiness=argc==3 && std::string(argv[1])=="--gpu-sample-readiness";
        const bool gpu=sample_readiness || (argc==2 && std::string(argv[1])=="--gpu-readiness");
        if ((argc!=1 && !gpu) || argc>3) throw std::invalid_argument("Unknown native lifecycle control");
        for (UINT flags=0;flags<256;++flags)
            require(NativeCamcorderProbe::gameplay_effect_visible(flags)==((flags&2)!=0),
                "Native gameplay effect visibility did not follow the stock show-in-game bit");
        require(!NativeCamcorderProbe::gameplay_effect_visible(4) &&
            NativeCamcorderProbe::gameplay_effect_visible(3) &&
            NativeCamcorderProbe::gameplay_effect_visible(7) &&
            NativeCamcorderProbe::gameplay_effect_visible(11),
            "Actual native disabled glitch/deform or enabled camera/AO visibility changed");
        void* errors=reinterpret_cast<void*>(1),*engine=reinterpret_cast<void*>(2);
        NativeCamcorderProbeBindings bindings{};
        bindings.capture_class=[]()->void* {return &capture_class_token;};
        bindings.target_class=[]()->void* {return &target_class_token;};
        bindings.construct=&construct;bindings.initialize=&initialize;
        bindings.attach=&attach;bindings.detach=&detach;bindings.set_view=&view;
        bindings.projection=&projection;bindings.errors=&errors;bindings.engine=&engine;
        bindings.activate_night_vision=[](void*,INT){};
        bindings.local_player=[](void*)->void* {return nullptr;};
        bindings.show_flags=[](void* component,UINT64* result)->UINT64* {
            result[0]=(skeleton_field<UINT>(component,0x88)&2)?0x40:0;return result;
        };
        NativeCamcorderProbe capture(bindings,{});
        BYTE first_owner=0,second_owner=0;
        const Vec3 position{1,2,3};const std::array<INT,3> rotation{{0,1234,0}};
        capture.update_pose(&first_owner,position,rotation,false);
        const auto first_handle=capture.pending_texture_handle();
        require(capture.needs_image() && !capture.texture_handle(),"Unrendered target was published as a lens image");
        capture.acknowledge_image(first_handle+1);
        require(!capture.texture_handle(),"An unrelated output published a stale lens image");
        capture.acknowledge_image(first_handle);
        capture.advance_frame(450);
        require(capture.frame()==450 && !capture.sampled() && capture.texture_handle()==first_handle,
            "Image readback gating stalled the native lifecycle/frame clock");
        require(first_handle && constructed==2 && attached==1 && views==1 && projections==1,
            "First independent native capture was not created exactly once");
        capture.update_pose(&first_owner,position,rotation,false);
        require(constructed==2 && attached==1 && views==2,"A pose update rebuilt/reattached the camera");
        capture.update_pose(&first_owner,position,rotation,true);
        require(!capture.texture_handle(),"NV mode change reused a stale capture");
        capture.acknowledge_image(capture.pending_texture_handle());
        require(constructed==2 && attached==2 && detached==1 &&
            skeleton_field<BYTE>(last_component,0x90)==1,"NV mode did not refresh only its native capture proxy");
        capture.hide();capture.hide();
        require(!capture.texture_handle() && detached==2,"Holstering did not detach idempotently");
        capture.update_pose(&first_owner,position,rotation,true);
        require(!capture.texture_handle(),"Re-grab reused the previous capture before its native render");
        capture.acknowledge_image(capture.pending_texture_handle());
        require(capture.texture_handle()==first_handle && constructed==2 && attached==3,
            "Reacquisition failed to reuse the valid native target");
        protect(PAGE_NOACCESS);
        bool render_safe=false;
        std::thread render([&] {
            render_safe=capture.texture_handle()==first_handle && !capture.texture();
            capture.release_device_resources();
            render_safe=render_safe && !capture.pending_texture_handle() && !capture.texture();
        });
        render.join();protect(PAGE_READWRITE);
        require(render_safe,"Render-side lookup depended on native object memory");
        capture.update_pose(&first_owner,position,rotation,true);
        require(capture.needs_image() && !capture.texture_handle(),
            "Device reset did not require a freshly rendered native lens");
        capture.acknowledge_image(capture.pending_texture_handle());
        capture.release();capture.release();
        require(!capture.texture_handle() && detached==3,"Pre-GC teardown was not idempotent");
        protect(PAGE_NOACCESS);
        require(!capture.texture_handle() && !capture.texture(),"Post-GC publication retained an invalid native resource");
        capture.release();protect(PAGE_READWRITE);
        capture.update_pose(&first_owner,position,rotation,false);
        capture.acknowledge_image(capture.pending_texture_handle());
        require(constructed==4 && projections==2 && capture.texture_handle()!=first_handle,
            "Post-GC capture reused collected native objects");
        capture.update_pose(&second_owner,position,rotation,false);
        require(constructed==6 && projections==3 && detached==4 &&
            skeleton_field<void*>(last_component,0x78)==&second_owner,"Owner change retained the previous hero's capture");
        capture.hide();capture.release();
        require(detached==5 && !capture.texture_handle(),"Release after holstering detached an invalid component");
        unsigned rejected=0;
        try {capture.create(nullptr);} catch (const std::invalid_argument&) {++rejected;}
        auto incomplete=bindings;incomplete.detach=nullptr;
        try {NativeCamcorderProbe bad(incomplete,{});} catch (const std::invalid_argument&) {++rejected;}
        require(rejected==2,"Incomplete native ownership/lifetime binding was admitted");
        bindings.close_movie=[](void* movie) {
            ++closed;
            auto* native=skeleton_field<void*>(movie,0x60);
            if (native) write(native,0x54,INT(0));
        };
        bindings.set_scale=[](void* movie,BYTE scale) {
            auto* native=skeleton_field<void*>(movie,0x60);
            write(skeleton_field<void*>(native,0x3c),0xe8,UINT(scale));
        };
        bindings.get_scale=[](void* view)->UINT {return skeleton_field<UINT>(view,0xe8);};
        NativeCamcorderProbe hud(bindings,{});
        auto* movie=page();auto* native=page();auto* original=page();auto* movie_view=page();
        write(native,0x3c,movie_view);
        write(movie,0x60,native);write(movie,0x118,original);write(movie,0x16d,BYTE(2));
        write(native,0x70,movie);write(native,0x54,INT(1));
        const auto before_hud=constructed;
        hud.bind_hud(movie);
        auto* target=skeleton_field<void*>(movie,0x118);
        require(constructed==before_hud+1 && target!=original && closed==1 &&
            skeleton_field<BYTE>(movie,0x16d)==1 &&
            skeleton_field<std::array<float,4>>(target,0x140)==std::array<float,4>{0,0,0,0},
            "Stock movie did not receive its own native transparent alpha target");
        write(native,0x78,target);
        hud.refresh_hud();hud.bind_hud(movie);
        require(skeleton_field<UINT>(movie_view,0xe8)==2,"Native HUD retained unscaled overlapping movie layout");
        write(movie_view,0xe8,UINT(0));hud.refresh_hud();
        require(skeleton_field<UINT>(movie_view,0xe8)==2,"Native HUD resize lost its render-target scale mode");
        require(constructed==before_hud+1 && closed==1 && hud.hud_movie()==movie &&
            hud.needs_hud_image() && !hud.hud_texture_handle() && !hud.hud_texture(),
            "HUD target was rebuilt or an unrendered native HUD image was published");
        if (gpu) {
            IsolatedDevice isolated(32,32);
            Com<IDirect3DTexture9> first,second;
            Com<IDirect3DSurface9> first_surface,second_surface;
            auto create=[&](Com<IDirect3DTexture9>& texture,Com<IDirect3DSurface9>& surface) {
                gpu_check(isolated.device->CreateTexture(16,16,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT,texture.put(),nullptr),"Create actual GPU HUD readiness fixture");
                gpu_check(texture->GetSurfaceLevel(0,surface.put()),"Get actual GPU HUD readiness surface");
            };
            create(first,first_surface);create(second,second_surface);
            auto* resource=skeleton_field<void*>(target,0xf8);
            const auto initial=skeleton_field<std::uintptr_t>(resource,0x24),replacement=initial+0x1000;
            hud.remember_texture(initial,*first.get());
            hud.surface_written(*second_surface.get());
            require(hud.needs_hud_image() && !hud.hud_texture_handle(),
                "An unrelated actual GPU surface published an unrendered HUD");
            hud.surface_written(*first_surface.get());
            require(!hud.needs_hud_image() && hud.hud_texture_handle()==initial && hud.hud_texture()==first.get(),
                "Actual completed HUD surface was not published");
            write(resource,0x24,replacement);hud.refresh_hud();
            hud.remember_texture(replacement,*second.get());
            require(hud.needs_hud_image() && !hud.hud_texture_handle() && !hud.hud_texture(),
                "A replacement native HUD resource inherited the previous image's readiness");
            hud.surface_written(*first_surface.get());
            require(hud.needs_hud_image(),"Stale GPU surface completion acknowledged a replacement HUD");
            hud.surface_written(*second_surface.get());
            require(hud.hud_texture_handle()==replacement && hud.hud_texture()==second.get(),
                "Replacement native HUD did not become ready after its own GPU completion");
            hud.release_device_resources();hud.refresh_hud();
            require(hud.needs_hud_image() && !hud.hud_texture_handle(),
                "Device reset reused previously completed native HUD pixels");
        }
        write(native,0x78,original);
        bool wrong_target=false;
        try {hud.refresh_hud();} catch (const std::runtime_error&) {wrong_target=true;}
        require(wrong_target,"HUD publication trusted a script property without checking actual native GFx binding");
        write(native,0x78,target);
        protect(PAGE_NOACCESS);
        bool hud_render_safe=false;
        std::thread hud_render([&] {
            hud_render_safe=!hud.hud_texture() && !hud.hud_texture_handle();
            hud.release_device_resources();
        });
        hud_render.join();protect(PAGE_READWRITE);
        require(hud_render_safe,"HUD render-side lookup read inaccessible native movie memory");
        hud.release();hud.release();
        require(closed==2 && skeleton_field<void*>(movie,0x118)==original &&
            skeleton_field<BYTE>(movie,0x16d)==2 && skeleton_field<UINT>(movie_view,0xe8)==0 && !hud.hud_movie(),
            "Pre-GC HUD teardown did not restore the original native movie target and alpha mode exactly once");
        if (sample_readiness) {
            const std::filesystem::path folder(argv[2]);
            if (!std::filesystem::create_directory(folder))
                throw std::invalid_argument("HUD sample fixture requires a new evidence directory");
            NativeCamcorderProbe diagnostic(bindings,folder);
            diagnostic.update_pose(&first_owner,position,rotation,false);
            diagnostic.bind_hud(movie);
            auto* diagnostic_target=skeleton_field<void*>(movie,0x118);
            write(native,0x78,diagnostic_target);diagnostic.refresh_hud();
            auto* resource=skeleton_field<void*>(diagnostic_target,0xf8);
            const auto hud_handle=skeleton_field<std::uintptr_t>(resource,0x24);
            IsolatedDevice isolated(32,32);
            Com<IDirect3DTexture9> lens_image,hud_image;
            Com<IDirect3DSurface9> lens_surface,hud_surface;
            const auto create=[&](Com<IDirect3DTexture9>& texture,Com<IDirect3DSurface9>& surface) {
                gpu_check(isolated.device->CreateTexture(512,288,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT,texture.put(),nullptr),"Create actual HUD sample readiness texture");
                gpu_check(texture->GetSurfaceLevel(0,surface.put()),"Get actual HUD sample readiness surface");
                gpu_check(isolated.device->ColorFill(surface.get(),nullptr,0xff123456),
                    "Initialize actual HUD sample readiness pixels");
            };
            create(lens_image,lens_surface);create(hud_image,hud_surface);
            diagnostic.remember_texture(diagnostic.pending_texture_handle(),*lens_image.get());
            diagnostic.remember_texture(hud_handle,*hud_image.get());
            diagnostic.surface_written(*lens_surface.get());
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            diagnostic.update_pose(&first_owner,position,rotation,false);
            require(diagnostic.diagnostic_ready() && !diagnostic.hud_timed_out(),
                "Delayed HUD fixture did not retain the original native readiness interval");
            diagnostic.sample(*isolated.device.get(),400);
            require(!diagnostic.sampled() && !std::filesystem::exists(folder/"native-lens-0.ppm"),
                "Diagnostic sampled a lens before its real native HUD image completed");
            diagnostic.surface_written(*hud_surface.get());
            diagnostic.sample(*isolated.device.get(),401);
            require(diagnostic.sampled()==1 && std::filesystem::exists(folder/"native-lens-0.ppm") &&
                std::filesystem::file_size(folder/"native-hud-0.bgra")==512*288*4,
                "Delayed actual GPU HUD completion did not release the pending native sample");
            write(resource,0x24,hud_handle+0x2000);diagnostic.refresh_hud();
            std::this_thread::sleep_for(std::chrono::milliseconds(4600));
            diagnostic.update_pose(&first_owner,position,rotation,false);
            bool timed_out=false;
            try {diagnostic.sample(*isolated.device.get(),700);}
            catch (const std::runtime_error& error) {
                timed_out=std::string(error.what())=="Native camera sample has no completed stock GFx HUD image";
            }
            require(timed_out && diagnostic.hud_timed_out() && diagnostic.sampled()==1 &&
                !std::filesystem::exists(folder/"native-lens-1.ppm"),
                "HUD sample readiness silently accepted or wrote evidence for a missing replacement image");
            diagnostic.release();
        }
        auto* native_engine=page();auto* effects=page();auto* normal_chain=page();auto* nv_chain=page();
        write(native_engine,0xa74,effects);write(effects,0x328,normal_chain);write(effects,0x338,nv_chain);
        for (auto* chain:{normal_chain,nv_chain}) {
            write(chain,0x60,effects);write(chain,0x68,INT(2));
        }
        engine=native_engine;bindings.native_postprocess=true;
        bindings.activate_night_vision=+[](void*,INT) {throw std::runtime_error("Capture changed fullscreen NV");};
        NativeCamcorderProbe postprocess(bindings,{});
        postprocess.update_pose(&first_owner,position,rotation,false);
        require(postprocess.postprocess_chain()==reinterpret_cast<std::uintptr_t>(normal_chain) &&
            postprocess.native_postprocess() && skeleton_field<UINT>(last_component,0x88)==7,
            "Normal lens did not bind the actual camera-color chain with independent postprocessing");
        const auto prior_attached=attached,prior_detached=detached;
        postprocess.update_pose(&first_owner,position,rotation,true);
        require(postprocess.postprocess_chain()==reinterpret_cast<std::uintptr_t>(nv_chain) &&
            attached==prior_attached+1 && detached==prior_detached+1 &&
            skeleton_field<BYTE>(last_component,0x90)==0 &&
            skeleton_field<BYTE>(effects,0x3b8)==0,
            "Native NV did not retain lit rendering and rebuild only the capture proxy without fullscreen effects");
        postprocess.release();
        require(!postprocess.postprocess_chain() && !postprocess.component_address(),
            "Collected capture retained its native postprocess provenance");
        for (auto* object:pages)
            if (!VirtualFree(object,0,MEM_RELEASE)) throw std::runtime_error("Cannot release isolated native fixture");
        std::cout<<"PASS: native camcorder lifecycle checks="<<checks
            <<"; hide/reacquire; mode changes; pre-GC release; new generation; owner changes;"
              " native HUD alpha target/restoration; wrong actual GFx target rejection;"
              " inaccessible UObject render-thread control.\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
