#include <windows.h>
#include "MinHook.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using Target = int (*)(int);
static Target trampoline=nullptr;
static unsigned intercepted=0;

static int replacement(int value) {
    ++intercepted;
    return trampoline(value)+100;
}

static void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void check(MH_STATUS status,const char* operation) {
    if (status!=MH_OK)
        throw std::runtime_error(std::string(operation)+": "+MH_StatusToString(status));
}

int main(int argc,char** argv) {
    void* page=nullptr;
    bool initialized=false;
    try {
        const bool fault_noop=argc==2 && std::string(argv[1])=="--fault-noop";
        require(argc==1 || fault_noop,"Unknown detour control");
        page=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        require(page!=nullptr,"Cannot allocate isolated native function");
        // RIP-relative load deliberately requires relocation in the real trampoline.
        const unsigned char code[]{0x8b,0x05,0x0a,0,0,0,0x01,0xc8,0xc3,
            0x90,0x90,0x90,0x90,0x90,0x90,0x90,7,0,0,0};
        std::memcpy(page,code,sizeof(code));
        DWORD protection=0;
        require(VirtualProtect(page,4096,PAGE_EXECUTE_READ,&protection)!=FALSE,
                "Cannot protect isolated function");
        require(FlushInstructionCache(GetCurrentProcess(),page,sizeof(code))!=FALSE,
                "Cannot publish isolated instructions");
        const auto target=reinterpret_cast<Target>(page);
        require(target(5)==12,"Unmodified native function produced wrong result");
        check(MH_Initialize(),"Initialize real MinHook");
        initialized=true;
        check(MH_CreateHook(page,reinterpret_cast<void*>(&replacement),
            reinterpret_cast<void**>(&trampoline)),"Create real detour");
        require(trampoline!=target,"Hook returned the original entry instead of a trampoline");
        require(trampoline(5)==12,"RIP-relative trampoline relocation is incorrect");
        require(target(5)==12 && intercepted==0,"Disabled hook intercepted a call");
        check(MH_QueueEnableHook(page),"Queue detour");
        require(target(5)==12 && intercepted==0,"Queued hook enabled prematurely");
        if (!fault_noop) check(MH_ApplyQueued(),"Apply queued detour");
        require(target(5)==112 && intercepted==1,"Hook claimed success without native redirection");
        require(trampoline(9)==16,"Enabled trampoline no longer executes original function");
        check(MH_DisableHook(page),"Disable detour");
        require(target(5)==12 && intercepted==1,"Disabled hook did not restore original function");
        check(MH_EnableHook(page),"Re-enable detour");
        require(target(8)==115 && intercepted==2,"Re-enabled detour failed");
        check(MH_RemoveHook(page),"Remove detour");
        require(target(5)==12 && intercepted==2,"Removed detour left target patched");
        require(std::memcmp(page,code,sizeof(code))==0,"Detour teardown changed native bytes");
        check(MH_Uninitialize(),"Uninitialize detours");
        initialized=false;
        require(VirtualFree(page,0,MEM_RELEASE)!=FALSE,"Cannot release isolated executable page");
        page=nullptr;
        std::cout<<"PASS: actual x64 redirection, queued activation, RIP-relative trampoline, disable/re-enable, byte-exact removal; no game launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        if (initialized) {
            const auto status=MH_Uninitialize();
            if (status!=MH_OK) std::cerr<<"FAIL: detour cleanup: "<<MH_StatusToString(status)<<'\n';
        }
        if (page && !VirtualFree(page,0,MEM_RELEASE))
            std::cerr<<"FAIL: isolated executable allocation cleanup\n";
        return 1;
    }
}
