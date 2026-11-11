#include "native_camcorder_probe.h"
#include <iostream>

using namespace olvr;

#define FAKE_RESOURCE_METHODS \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override {return E_NOINTERFACE;} \
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references;} \
    ULONG STDMETHODCALLTYPE Release() override {return --references;} \
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override {return E_NOTIMPL;} \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override {return E_NOTIMPL;} \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override {return E_NOTIMPL;} \
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override {return E_NOTIMPL;} \
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override {return 0;} \
    DWORD STDMETHODCALLTYPE GetPriority() override {return 0;} \
    void STDMETHODCALLTYPE PreLoad() override {}

struct Surface final:IDirect3DSurface9 {
    ULONG references=1;
    FAKE_RESOURCE_METHODS
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {return D3DRTYPE_SURFACE;}
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID,void**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT*,const RECT*,DWORD) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE UnlockRect() override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDC(HDC*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override {return E_NOTIMPL;}
};

struct Texture final:IDirect3DTexture9 {
    ULONG references=1;
    Surface& surface;
    explicit Texture(Surface& value):surface(value) {}
    FAKE_RESOURCE_METHODS
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {return D3DRTYPE_TEXTURE;}
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override {return 0;}
    DWORD STDMETHODCALLTYPE GetLOD() override {return 0;}
    DWORD STDMETHODCALLTYPE GetLevelCount() override {return 1;}
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override {return E_NOTIMPL;}
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override {return D3DTEXF_NONE;}
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {}
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT,D3DSURFACE_DESC*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level,IDirect3DSurface9** out) override {
        if (level || !out) return D3DERR_INVALIDCALL;
        surface.AddRef();*out=&surface;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT,D3DLOCKED_RECT*,const RECT*,DWORD) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override {return E_NOTIMPL;}
};
#undef FAKE_RESOURCE_METHODS

using Block=std::array<unsigned char,0x400>;
static Block lens_target{},component{},hud_target{},lens_resource{},hud_resource{};
static int capture_type=0,target_type=0;
static unsigned target_count=0,checks=0;

template<class T> static void put(void* object,std::size_t offset,T value) {
    std::memcpy(static_cast<unsigned char*>(object)+offset,&value,sizeof(value));
}
static void expect(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static void* WINAPI capture_class() {return &capture_type;}
static void* WINAPI target_class() {return &target_type;}
static void* WINAPI construct(void* type,void*,UINT64,UINT64,void*,void*,void*,void*) {
    if (type==&capture_type) return component.data();
    if (type!=&target_type || target_count>=2) throw std::runtime_error("Unexpected native fixture construction");
    return target_count++?hud_target.data():lens_target.data();
}
static void WINAPI initialize(void* target,UINT width,UINT height,BYTE,UINT) {
    expect(width==512 && height==288,"Native texture fixture extent changed");
    put(target,0xf8,target==lens_target.data()?lens_resource.data():hud_resource.data());
}
static void WINAPI attach(void* owner,void* value) {put(value,0x78,owner);}
static void WINAPI detach(void*,void* value) {put(value,0x78,static_cast<void*>(nullptr));}
static void WINAPI set_view(void*,const Vec3*,const INT*) {}
static void WINAPI projection(void*) {}
static void WINAPI activate(void*,INT) {}
static void* WINAPI local_player(void* owner) {return owner;}
static UINT64* WINAPI show_flags(void*,UINT64* flags) {flags[0]=flags[1]=0;return flags;}
static void close_movie(void*) {}
static void WINAPI set_scale(void*,BYTE) {}
static UINT WINAPI get_scale(void*) {return 2;}

int main() {
    try {
        Surface lens_surface,hud_surface,next_lens_surface,reused_surface,next_hud_surface;
        Texture lens(lens_surface),hud(hud_surface),next_lens(next_lens_surface),reused(reused_surface);
        Texture next_hud(next_hud_surface);
        Block hero{},movie{},native_movie{},view{};
        void* errors=hero.data();
        void* engine=hero.data();
        NativeCamcorderProbeBindings bindings;
        bindings.capture_class=&capture_class;bindings.target_class=&target_class;
        bindings.construct=&construct;bindings.initialize=&initialize;
        bindings.attach=&attach;bindings.detach=&detach;bindings.set_view=&set_view;
        bindings.projection=&projection;bindings.activate_night_vision=&activate;
        bindings.local_player=&local_player;bindings.show_flags=&show_flags;
        bindings.close_movie=&close_movie;bindings.set_scale=&set_scale;bindings.get_scale=&get_scale;
        bindings.errors=&errors;bindings.engine=&engine;
        NativeCamcorderProbe probe(bindings,{});
        const auto update=[&] {probe.update_pose(hero.data(),Vec3{},std::array<INT,3>{},false);};
        put(lens_resource.data(),0x24,std::uintptr_t(1));
        update();
        probe.remember_texture(1,lens);
        probe.acknowledge_image(1);
        expect(probe.texture()==&lens,"Initial native lens texture missing");
        probe.bind_hud(movie.data());
        put(movie.data(),0x60,static_cast<void*>(native_movie.data()));
        put(native_movie.data(),0x70,static_cast<void*>(movie.data()));
        put(native_movie.data(),0x78,static_cast<void*>(hud_target.data()));
        put(native_movie.data(),0x3c,static_cast<void*>(view.data()));
        update();
        probe.remember_texture(2,hud);
        expect(hud.references==2,"Pending native HUD association was not retained");
        expect(probe.texture()==&lens,"Pending HUD allocation changed the active lens");
        put(hud_resource.data(),0x24,std::uintptr_t(2));
        update();
        probe.surface_written(hud_surface);
        expect(probe.hud_texture_handle()==2 && probe.hud_texture()==&hud,
               "Native HUD allocated before game-thread publication was irreversibly evicted");
        probe.remember_texture(3,next_lens);
        expect(probe.texture()==&lens,"Pending lens allocation changed the published lens");
        put(lens_resource.data(),0x24,std::uintptr_t(3));
        update();
        probe.acknowledge_image(3);
        expect(probe.texture()==&next_lens,"Late native lens publication lost its existing GPU association");
        expect(lens.references==1,"Superseded published lens texture was not retired");
        expect(probe.hud_texture()==&hud,"Lens replacement invalidated the independent HUD");
        probe.remember_texture(3,reused);
        expect(next_lens.references==1,"Reused RHI handle retained its obsolete D3D texture");
        expect(probe.texture()==&reused,"Reused native RHI handle still resolves to stale image");
        probe.remember_texture(4,next_hud);
        expect(probe.texture()==&reused,"Pending HUD replacement changed the active native lens");
        put(hud_resource.data(),0x24,std::uintptr_t(4));
        update();
        probe.surface_written(next_hud_surface);
        expect(probe.hud_texture()==&next_hud,"Delayed HUD replacement lost its GPU association");
        expect(probe.texture()==&reused && hud.references==1,
               "Superseded completed HUD texture was not retired");
        probe.release_device_resources();
        expect(hud.references==1 && reused.references==1 && next_hud.references==1,
               "Native device reset leaked retained lens/HUD resources");
        expect(!probe.texture() && !probe.hud_texture(),"Device reset exposed a retired native image");
        probe.release();
        std::cout<<"PASS: native_camera_resource_checks="<<checks
                 <<"; delayed_game_thread_HUD_and_lens_publication; superseded_resource_retirement;"
                    " RHI_handle_reuse; exact_device_reset_release; no_GPU_or_native_engine_execution\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
