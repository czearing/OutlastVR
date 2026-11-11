#include "isolated_d3d9.h"
#include "outlast_replay_compactor.h"
#include "d3d9_shader_variants.h"
#include "MinHook.h"
#include "outlast_native_image.h"
#ifdef OLVR_ISOLATED_9ON12
#include "isolated_d3d12_counter.h"
#endif
#include <iostream>

using namespace olvr;
using NativeIndexed=void(WINAPI*)(void*,void*,UINT,INT,UINT,UINT,UINT,UINT);
using DeviceDraw=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using Cooperative=HRESULT(WINAPI*)(IDirect3DDevice9*);
static DeviceDraw original_draw=nullptr;
static Cooperative original_cooperative=nullptr;
static OutlastReplayCompactor* active=nullptr;
static D3D9StereoPipeline* pipeline=nullptr;
static StereoViewConstants views{};
static bool internal=false,guard_failure=false;
static UINT physical_draws=0,native_calls=0,cooperative_calls=0;
static std::string interception_error;

static void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void hook_check(MH_STATUS status,const char* operation) {
    if (status!=MH_OK) throw std::runtime_error(std::string(operation)+": "+MH_StatusToString(status));
}
static HRESULT WINAPI draw_hook(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,
                                UINT minimum,UINT vertices,UINT start,UINT primitives) {
    if (!active || internal) {
        ++physical_draws;
        return original_draw(device,type,base,minimum,vertices,start,primitives);
    }
    ++native_calls;
    internal=true;
    try {
        const auto result=active->consume({UINT(type),base,minimum,vertices,start,primitives},*pipeline,views);
        internal=false;
        return result.operation_status;
    } catch (const std::exception& error) {
        internal=false;interception_error=error.what();
        return D3DERR_INVALIDCALL;
    }
}
static HRESULT WINAPI cooperative_hook(IDirect3DDevice9* device) {
    ++cooperative_calls;
    return guard_failure?D3DERR_DEVICELOST:original_cooperative(device);
}

class NativeFixture {
    unsigned char* page_=nullptr;
public:
    explicit NativeFixture(const std::filesystem::path& path) {
        const auto bytes=outlast_proof_body(path,0xbf5b60,627,
            "ff6939df22849635bde7285b764f3d7f5c6fa5f53f5a0d991dcbcd98049ff263");
        require(bytes.size()==627 && bytes[0x4c]==0x39 && bytes[0x4d]==0x35 &&
            bytes[0x25e]==0xc3,"Native indexed fixture differs from its REA body layout");
        page_=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        require(page_!=nullptr,"Cannot allocate isolated native indexed function");
        std::memcpy(page_,bytes.data(),bytes.size());
        // Only the cooperative-guard RIP displacement is relocated. The two
        // external logging branches are unreachable for the three valid types.
        const std::int32_t displacement=4096-0x52;
        std::memcpy(page_+0x4e,&displacement,sizeof(displacement));
        DWORD old=0;
        if (!VirtualProtect(page_,4096,PAGE_EXECUTE_READ,&old) ||
            !FlushInstructionCache(GetCurrentProcess(),page_,bytes.size())) {
            VirtualFree(page_,0,MEM_RELEASE);page_=nullptr;
            throw std::runtime_error("Cannot publish the isolated native indexed function");
        }
    }
    ~NativeFixture() {
        if (page_ && !VirtualFree(page_,0,MEM_RELEASE)) {
            std::cerr<<"FAIL: native indexed fixture cleanup\n";std::terminate();
        }
    }
    NativeIndexed entry() const {return reinterpret_cast<NativeIndexed>(page_);}
    void set_guard(UINT enabled) {std::memcpy(page_+4096,&enabled,sizeof(enabled));}
};

class DeviceHooks {
    void* draw_=nullptr;
    void* cooperative_=nullptr;
    bool initialized_=false;
public:
    explicit DeviceHooks(IDirect3DDevice9& device) {
        void** table=nullptr;std::memcpy(&table,&device,sizeof(table));
        draw_=table[82];cooperative_=table[3];
        hook_check(MH_Initialize(),"Initialize isolated native draw interception");initialized_=true;
        try {
            hook_check(MH_CreateHook(draw_,reinterpret_cast<void*>(&draw_hook),
                reinterpret_cast<void**>(&original_draw)),"Create real indexed-device interception");
            hook_check(MH_CreateHook(cooperative_,reinterpret_cast<void*>(&cooperative_hook),
                reinterpret_cast<void**>(&original_cooperative)),"Create isolated cooperative guard control");
            hook_check(MH_QueueEnableHook(draw_),"Queue isolated draw interception");
            hook_check(MH_QueueEnableHook(cooperative_),"Queue isolated cooperative interception");
            hook_check(MH_ApplyQueued(),"Activate real isolated device interceptions");
        } catch (...) {
            const auto status=MH_Uninitialize();initialized_=false;
            if (status!=MH_OK) std::cerr<<"FAIL: isolated interception initialization cleanup\n";
            throw;
        }
    }
    ~DeviceHooks() {
        if (initialized_ && MH_Uninitialize()!=MH_OK) {
            std::cerr<<"FAIL: isolated native interception teardown\n";std::terminate();
        }
    }
};

template<class T,std::size_t N>
static void write(std::array<unsigned char,N>& buffer,std::size_t offset,T value) {
    require(offset<=N && sizeof(T)<=N-offset,"Fixture field exceeds native object extent");
    std::memcpy(buffer.data()+offset,&value,sizeof(value));
}

int wmain(int argc,wchar_t** argv) {
    try {
        const bool fault=argc==3 && std::wstring(argv[2])==L"--fault-replay";
        require(argc==2 || fault,"Specify the private native indexed fixture [--fault-replay]");
        constexpr UINT width=128,height=16,max_objects=7;
        NativeFixture native(argv[1]);
        IsolatedDevice isolated(width*2,height);
        auto& device=*isolated.device.get();
        DeviceHooks hooks(device);
        const char* vs=R"(
            struct Output {float4 position:POSITION;float2 color:TEXCOORD0;};
            Output main(float2 position:POSITION,float4 object:TEXCOORD1) {
                Output output;output.position=float4(position.x*object.y+object.x,position.y,0,1);
                output.color=object.zw;return output;
            })";
        const char* ps="float4 main(float2 color:TEXCOORD0):COLOR {return float4(color,.25,1);}";
        auto compile=[](const char* source,const char* profile,Com<ID3DBlob>& code) {
            Com<ID3DBlob> errors;
            const auto status=D3DCompile(source,std::strlen(source),"native-replay",nullptr,nullptr,
                "main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,code.put(),errors.put());
            if (FAILED(status)) throw std::runtime_error(errors.get()?
                std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):
                "Native replay fixture shader compile failed");
        };
        Com<ID3DBlob> vertex_code,pixel_code;
        compile(vs,"vs_3_0",vertex_code);compile(ps,"ps_3_0",pixel_code);
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        gpu_check(device.CreateVertexShader(static_cast<const DWORD*>(vertex_code->GetBufferPointer()),
            vertex.put()),"Create replay vertex shader");
        gpu_check(device.CreatePixelShader(static_cast<const DWORD*>(pixel_code->GetBufferPointer()),
            pixel.put()),"Create replay pixel shader");
        gpu_check(device.SetVertexShader(vertex.get()),"Bind replay vertex shader");
        gpu_check(device.SetPixelShader(pixel.get()),"Bind replay pixel shader");
        const D3DVERTEXELEMENT9 elements[]{
            {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {3,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},D3DDECL_END()};
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.CreateVertexDeclaration(elements,declaration.put()),"Create replay declaration");
        gpu_check(device.SetVertexDeclaration(declaration.get()),"Bind replay declaration");
        Com<IDirect3DVertexBuffer9> geometry,records;
        const float quad[][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
        gpu_check(device.CreateVertexBuffer(sizeof(quad),0,0,D3DPOOL_MANAGED,geometry.put(),nullptr),"Create replay geometry");
        void* mapped=nullptr;
        gpu_check(geometry->Lock(0,0,&mapped,0),"Lock replay geometry");
        std::memcpy(mapped,quad,sizeof(quad));gpu_check(geometry->Unlock(),"Unlock replay geometry");
        gpu_check(device.SetStreamSource(0,geometry.get(),0,sizeof(quad[0])),"Bind replay geometry");
        gpu_check(device.CreateVertexBuffer(max_objects*32,0,0,D3DPOOL_MANAGED,records.put(),nullptr),"Create padded object records");
        Com<IDirect3DIndexBuffer9> indices;
        const WORD triangles[]{0,1,2,0,2,3};
        gpu_check(device.CreateIndexBuffer(sizeof(triangles),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,
            indices.put(),nullptr),"Create replay indices");
        gpu_check(indices->Lock(0,0,&mapped,0),"Lock replay indices");
        std::memcpy(mapped,triangles,sizeof(triangles));gpu_check(indices->Unlock(),"Unlock replay indices");
        gpu_check(device.SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable replay culling");
        gpu_check(device.SetRenderState(D3DRS_ZENABLE,FALSE),"Disable replay depth");
        D3D9ShaderVariants variants(device);
        pipeline=&variants.current();
        Com<IDirect3DSurface9> backbuffer,readback;
        gpu_check(device.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),"Get replay output");
        gpu_check(device.CreateOffscreenPlainSurface(width*2,height,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,readback.put(),nullptr),"Create replay readback");
#ifdef OLVR_ISOLATED_9ON12
        IsolatedD3D12DrawCounter counter(isolated.translation_device());
#endif
        UINT pixels=0;
#ifdef OLVR_ISOLATED_9ON12
        UINT gpu_draws=0,gpu_instances=0;
#endif
        for (unsigned frame=0;frame<6;++frame) {
            const UINT objects=frame+2;
            std::array<unsigned char,0x3cc> rhi{};
            std::array<unsigned char,32> stats{};
            write(rhi,0x34,&device);write(rhi,0x10,stats.data());write(rhi,0x18,UINT(1));
            write(rhi,0x284,objects);write(rhi,0x288,UINT(1<<3));
            write(rhi,0x28c+3*0x14,reinterpret_cast<std::uintptr_t>(records.get())+8);
            write(rhi,0x294+3*0x14,UINT(32));write(rhi,0x298+3*0x14,UINT(0));
            gpu_check(records->Lock(0,0,&mapped,0),"Lock changing replay object data");
            std::memset(mapped,0xcd,max_objects*32);
            for (unsigned object=0;object<objects;++object) {
                const float data[]{-1+(2.f*object+1)/objects,.6f/objects,
                    (30.f+object*25+frame*3)/255,(40.f+frame*13+object*17)/255};
                std::memcpy(static_cast<char*>(mapped)+object*32,data,sizeof(data));
            }
            gpu_check(records->Unlock(),"Publish changing replay object records");
            gpu_check(device.SetStreamSource(3,records.get(),0,0),"Bind original constant-per-object stream");
            const Draw expected{D3DPT_TRIANGLELIST,0,0,4,0,2};
            OutlastReplayCompactor scope(rhi.data(),rhi.size(),device,expected);
            physical_draws=native_calls=0;interception_error.clear();
#ifdef OLVR_ISOLATED_9ON12
            counter.reset();
#endif
            gpu_check(device.Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"Clear replay frame");
            gpu_check(device.BeginScene(),"Begin actual native replay frame");
            active=&scope;
            native.entry()(rhi.data(),reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(indices.get())+8),
                0,0,0,4,0,2);
            active=nullptr;
            require(interception_error.empty(),interception_error.c_str());
            scope.finish();
            if (fault && !pipeline->draw_indexed(expected,views).succeeded())
                throw std::runtime_error("Replay negative did not submit its deliberate additional draw");
            gpu_check(device.EndScene(),"End native replay frame");
            gpu_check(device.GetRenderTargetData(backbuffer.get(),readback.get()),"Synchronize native replay pixels");
            require(native_calls==objects && physical_draws==1,
                "Native replay emitted more than one physical geometry draw");
            UINT draws=0,primitives=0;
            std::memcpy(&draws,stats.data()+12,4);std::memcpy(&primitives,stats.data()+16,4);
            require(draws==1 && primitives==2,"Actual native RHI statistics were bypassed or doubled");
            UINT frequency=0;
            gpu_check(device.GetStreamSourceFreq(0,&frequency),"Check native geometry frequency");
            require(frequency==1,"Compaction leaked indexed frequency");
            gpu_check(device.GetStreamSourceFreq(3,&frequency),"Check native object frequency");
            require(frequency==1,"Compaction leaked object frequency");
            Com<IDirect3DVertexBuffer9> source;
            UINT offset=0,stride=1;
            gpu_check(device.GetStreamSource(3,source.put(),&offset,&stride),"Check final original replay binding");
            require(source.get()==records.get() && offset==(objects-1)*32 && stride==0,
                "Compaction changed original native replay postconditions");
            D3DLOCKED_RECT locked{};
            gpu_check(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"Read native replay pixels");
            bool correct=true;
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width*2;++x) {
                const double position=2.*(x%width)/width-1;
                int selected=-1;bool boundary=false;
                for (unsigned object=0;object<objects;++object) {
                    const double distance=std::abs(position-(-1+(2.*object+1)/objects));
                    if (std::abs(distance-.6/objects)<.012) boundary=true;
                    if (distance<.6/objects) selected=int(object);
                }
                if (boundary) continue;
                const DWORD expected_pixel=selected<0?0xff000000:
                    0xff000040|DWORD(30+selected*25+frame*3)<<16|DWORD(40+frame*13+selected*17)<<8;
                const auto actual_pixel=reinterpret_cast<const DWORD*>(
                    static_cast<const char*>(locked.pBits)+y*locked.Pitch)[x];
                for (unsigned shift:{0u,8u,16u})
                    if (std::abs(int((actual_pixel>>shift)&255)-int((expected_pixel>>shift)&255))>1) {
                        if (correct) std::cerr<<"Native replay pixel mismatch: frame="<<frame
                            <<"; x="<<x<<"; y="<<y<<"; actual="<<actual_pixel<<"; expected="<<expected_pixel<<'\n';
                        correct=false;
                    }
                ++pixels;
            }
            gpu_check(readback->UnlockRect(),"Unlock native replay measurement");
            require(correct,"Compacted native object/eye pixel ordering differs from the independent raster oracle");
#ifdef OLVR_ISOLATED_9ON12
            if (counter.indexed_calls!=1 || counter.plain_calls!=0 || counter.instances!=objects*2)
                std::cerr<<"Native GPU replay mismatch: frame="<<frame<<"; objects="<<objects
                    <<"; indexed="<<counter.indexed_calls<<"; plain="<<counter.plain_calls
                    <<"; instances="<<counter.instances<<'\n';
            require(counter.indexed_calls==1 && counter.plain_calls==0 && counter.instances==objects*2,
                "Underlying GPU replay did not contain exactly one draw with 2N instances");
            gpu_draws+=counter.indexed_calls;gpu_instances+=counter.instances;
#endif
        }
        std::array<unsigned char,0x3cc> rhi{};
        std::array<unsigned char,32> stats{};
        auto prepare=[&] {
            rhi.fill(0);stats.fill(0);
            write(rhi,0x34,&device);write(rhi,0x10,stats.data());write(rhi,0x18,UINT(1));
            write(rhi,0x284,UINT(3));write(rhi,0x288,UINT(1<<3));
            write(rhi,0x28c+3*0x14,reinterpret_cast<std::uintptr_t>(records.get())+8);
            write(rhi,0x294+3*0x14,UINT(32));write(rhi,0x298+3*0x14,UINT(0));
            gpu_check(device.SetStreamSource(3,records.get(),0,0),"Reset isolated native object source");
            physical_draws=native_calls=0;interception_error.clear();
        };
        const auto handle=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(indices.get())+8);
        const Draw expected{D3DPT_TRIANGLELIST,0,0,4,0,2};
        prepare();
        {
            OutlastReplayCompactor scope(rhi.data(),rhi.size(),device,expected);
            native.set_guard(1);guard_failure=true;cooperative_calls=0;active=&scope;
            native.entry()(rhi.data(),handle,0,0,0,4,0,2);
            active=nullptr;guard_failure=false;native.set_guard(0);
            scope.finish(true);
            require(cooperative_calls==1 && native_calls==0 && physical_draws==0,
                "Original cooperative guard did not prevent all physical/native draws");
            UINT count=0,draws=0,primitives=0;
            std::memcpy(&count,rhi.data()+0x284,4);
            std::memcpy(&draws,stats.data()+12,4);std::memcpy(&primitives,stats.data()+16,4);
            require(count==3 && draws==1 && primitives==2,
                "Compaction changed native guarded-return statistics or instance state");
        }
        prepare();
        native.entry()(rhi.data(),handle,0,0,0,4,0,0);
        UINT count=0,mask=0,draws=0,primitives=1;
        std::memcpy(&count,rhi.data()+0x284,4);std::memcpy(&mask,rhi.data()+0x288,4);
        std::memcpy(&draws,stats.data()+12,4);std::memcpy(&primitives,stats.data()+16,4);
        require(physical_draws==0 && count==1 && mask==0 && draws==1 && primitives==0,
            "Original zero-primitive native cleanup/statistics were changed");
        prepare();
        const auto before_null=rhi;
        native.entry()(rhi.data(),nullptr,0,0,0,4,0,2);
        require(rhi==before_null && physical_draws==0,
            "Original null-index native early return was changed");
        UINT refused=0;
        for (unsigned malformed=0;malformed<7;++malformed) {
            prepare();
            if (malformed==0) write(rhi,0x284,UINT(0));
            if (malformed==1) write(rhi,0x288,UINT(1));
            if (malformed==2) write(rhi,0x288,UINT(1<<17));
            if (malformed==3) write(rhi,0x298+3*0x14,UINT(16));
            if (malformed==4) write(rhi,0x294+3*0x14,UINT(512));
            if (malformed==5) write(rhi,0x28c+3*0x14,std::uintptr_t(0));
            if (malformed==6) gpu_check(device.SetStreamSource(3,records.get(),32,0),"Offset negative binding");
            const auto before=rhi;
            try {OutlastReplayCompactor reject(rhi.data(),rhi.size(),device,expected);}
            catch (const std::invalid_argument&) {++refused;}
            require(rhi==before && physical_draws==0,"Invalid replay validation mutated native state or emitted geometry");
        }
        require(refused==7,"Malformed software replay records were not all rejected");
        for (const UINT primitive:{1u,2u}) {
            prepare();
            const Draw mapped{primitive==1?UINT(D3DPT_TRIANGLESTRIP):UINT(D3DPT_LINELIST),0,0,4,0,2};
            OutlastReplayCompactor scope(rhi.data(),rhi.size(),device,mapped);
            gpu_check(device.BeginScene(),"Begin native alternate-topology proof");
            active=&scope;native.entry()(rhi.data(),handle,primitive,0,0,4,0,2);active=nullptr;
            gpu_check(device.EndScene(),"End native alternate-topology proof");
            require(interception_error.empty(),interception_error.c_str());
            scope.finish();
            require(native_calls==3 && physical_draws==1,"Native topology did not preserve compacted physical draw count");
        }
        std::cout<<"PASS: actual MCP-recovered 627-byte indexed RHI; frames=6; checked_pixels="<<pixels
            <<"; objects=2..7; one physical draw with 2N instances; native statistics/cache cleanup/stream postconditions preserved"
            <<"; guard/null/zero-primitive cases preserved; malformed_records_rejected="<<refused
            <<"; all three native topologies"
#ifdef OLVR_ISOLATED_9ON12
            <<"; native_d3d12_draws="<<gpu_draws<<"; native_d3d12_instances="<<gpu_instances
#endif
            <<"; no game launched\n";
        return 0;
    } catch (const std::exception& error) {
        active=nullptr;internal=false;
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
