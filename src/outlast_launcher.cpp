#include "outlast_native_image.h"
#include "outlast_config.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>

namespace {
class Handle {
    HANDLE value_=nullptr;
public:
    explicit Handle(HANDLE value=nullptr):value_(value) {}
    ~Handle(){if (value_ && value_!=INVALID_HANDLE_VALUE) CloseHandle(value_);}
    HANDLE get() const {return value_;}
    void reset(HANDLE value) {
        if (value_ && value_!=INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_=value;
    }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
void check(bool value,const char* operation) {
    if (!value) throw std::runtime_error(std::string(operation)+"; Win32="+std::to_string(GetLastError()));
}
std::filesystem::path executable_path() {
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    check(length>0 && length<path.size(),"Resolve launcher path");
    return std::wstring(path.data(),length);
}
std::uintptr_t remote_module(DWORD process,const std::wstring& basename) {
    HANDLE value=INVALID_HANDLE_VALUE;
    for (unsigned attempt=0;attempt<100;++attempt) {
        value=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,process);
        if (value!=INVALID_HANDLE_VALUE || GetLastError()!=ERROR_BAD_LENGTH) break;
        Sleep(10);
    }
    Handle snapshot(value);
    check(snapshot.get()!=INVALID_HANDLE_VALUE,"Enumerate owned suspended process modules");
    MODULEENTRY32W module{};module.dwSize=sizeof(module);
    check(Module32FirstW(snapshot.get(),&module)!=FALSE,"Read owned suspended module census");
    do {
        if (_wcsicmp(module.szModule,basename.c_str())==0)
            return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
    } while (Module32NextW(snapshot.get(),&module));
    throw std::runtime_error("Required module is absent from the owned suspended process");
}
std::uintptr_t remote_system_function(DWORD process,const char* name) {
    const auto function=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),name);
    check(function!=nullptr,"Resolve system loader function");
    HMODULE owner=nullptr;
    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(function),&owner)!=FALSE,"Resolve forwarded system function owner");
    std::array<wchar_t,32768> path{};
    check(GetModuleFileNameW(owner,path.data(),DWORD(path.size()))!=0,"Resolve system function module name");
    const auto displacement=reinterpret_cast<std::uintptr_t>(function)-reinterpret_cast<std::uintptr_t>(owner);
    return remote_module(process,std::filesystem::path(path.data()).filename().wstring())+displacement;
}
DWORD call(HANDLE process,std::uintptr_t function,void* argument,const char* operation) {
    Handle thread(CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(function),argument,0,nullptr));
    check(thread.get()!=nullptr,operation);
    const auto waited=WaitForSingleObject(thread.get(),120000);
    if (waited==WAIT_TIMEOUT) throw std::runtime_error(std::string(operation)+" timed out; the owned suspended process will be stopped");
    check(waited==WAIT_OBJECT_0,operation);
    DWORD result=0;check(GetExitCodeThread(thread.get(),&result)!=FALSE,"Read owned initialization result");return result;
}
void bootstrap_loader(HANDLE process) {
    const auto local=GetModuleHandleW(L"ntdll.dll");
    const auto exit=GetProcAddress(local,"RtlExitUserThread");
    check(exit!=nullptr,"Resolve the initial native thread exit routine");
    SYSTEM_INFO system{};GetSystemInfo(&system);
    std::uintptr_t address=reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto maximum=reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    void* previous=nullptr;
    while (address<maximum) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process,reinterpret_cast<void*>(address),&region,sizeof(region))) break;
        if (region.Type==MEM_IMAGE && region.AllocationBase!=previous) {
            previous=region.AllocationBase;
            std::array<wchar_t,32768> path{};
            const auto length=GetMappedFileNameW(process,region.AllocationBase,path.data(),DWORD(path.size()));
            if (length && _wcsicmp(std::filesystem::path(std::wstring(path.data(),length)).filename().c_str(),L"ntdll.dll")==0) {
                const auto entry=reinterpret_cast<std::uintptr_t>(region.AllocationBase)+
                    reinterpret_cast<std::uintptr_t>(exit)-reinterpret_cast<std::uintptr_t>(local);
                std::array<unsigned char,16> remote{};SIZE_T read=0;
                check(ReadProcessMemory(process,reinterpret_cast<void*>(entry),remote.data(),remote.size(),&read)!=FALSE &&
                    read==remote.size() && std::memcmp(remote.data(),reinterpret_cast<const void*>(exit),remote.size())==0,
                    "Validate the mapped native bootstrap entry");
                check(call(process,entry,nullptr,"Initialize Windows loader with the game thread still suspended")==0,
                    "Complete native loader bootstrap");
                return;
            }
        }
        const auto next=reinterpret_cast<std::uintptr_t>(region.BaseAddress)+region.RegionSize;
        if (next<=address) break;
        address=next;
    }
    throw std::runtime_error("Initial ntdll image is absent from the owned suspended process");
}
class RemoteMemory {
    HANDLE process_;void* pointer_=nullptr;
public:
    RemoteMemory(HANDLE process,const void* data,std::size_t size):process_(process) {
        pointer_=VirtualAllocEx(process,nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        check(pointer_!=nullptr,"Allocate owned loader argument");
        SIZE_T written=0;
        if (!WriteProcessMemory(process,pointer_,data,size,&written) || written!=size) {
            VirtualFreeEx(process,pointer_,0,MEM_RELEASE);pointer_=nullptr;
            check(false,"Write owned loader argument");
        }
    }
    ~RemoteMemory(){if (pointer_) VirtualFreeEx(process_,pointer_,0,MEM_RELEASE);}
    void* get() const {return pointer_;}
};
}

int wmain(int argc,wchar_t** argv) {
    PROCESS_INFORMATION process{};
    Handle owned_process,owned_thread;
    bool resumed=false,headless=false;
    try {
        bool verify=false,native_resolution=false;
        std::filesystem::path game_override,requested_evidence;
        std::set<std::wstring> options;
        for (int i=1;i<argc;++i) {
            const std::wstring option=argv[i];
            if (!options.insert(option).second) throw std::invalid_argument("Duplicate launcher option");
            if (option==L"--version") {
                std::cout<<"Outlast VR "<<OLVR_VERSION<<'\n';
                return 0;
            }
            if (option==L"--help") {
                std::cout<<"Outlast VR\nUsage: OutlastVRLauncher.exe [--game \"path\\to\\OLGame.exe\"] "
                    "[--verify | --native-resolution | --headless-verify new-absolute-directory]\n"
                    "Settings: OutlastVR.ini beside the launcher. --verify never starts the game.\n"
                    "--version prints the mod's base version.\n";
                return 0;
            }
            if (option==L"--game" || option==L"--headless-verify") {
                if (++i>=argc) throw std::invalid_argument("Launcher option requires a path");
                if (option==L"--game") game_override=argv[i];
                else {headless=true;requested_evidence=argv[i];}
            } else if (option==L"--verify") verify=true;
            else if (option==L"--native-resolution") native_resolution=true;
            else throw std::invalid_argument("Unknown launcher option; use --help");
        }
        if (unsigned(verify)+unsigned(headless)+unsigned(native_resolution)>1)
            throw std::invalid_argument("Select only one launch mode");
        const auto folder=executable_path().parent_path();
        auto config=olvr::load_outlast_config(folder);
        if (!game_override.empty()) config.game_executable=std::filesystem::absolute(game_override);
        const auto dll=folder/L"OutlastVRSinglePass.dll";
        const auto game=olvr::outlast_game_path(folder,config);
        olvr::OutlastNativeImage image(game);
        if (!std::filesystem::is_regular_file(dll)) throw std::runtime_error("The matching mod DLL must be beside the launcher");
        HMODULE mapped=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
        check(mapped!=nullptr,"Read mod export directory without running the DLL");
        const auto initialize=GetProcAddress(mapped,"OutlastVR_Initialize");
        const auto offset=reinterpret_cast<std::uintptr_t>(initialize)-reinterpret_cast<std::uintptr_t>(mapped);
        FreeLibrary(mapped);
        check(initialize!=nullptr,"Resolve explicit mod initialization export");
        if (!headless) {
            const auto loader=config.openxr_loader.empty()?folder/L"openxr_loader.dll":
                olvr::config_path(folder,config.openxr_loader);
            mapped=LoadLibraryExW(loader.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
            check(mapped!=nullptr,"Read configured OpenXR loader; extract every release file or correct vr.openxr_loader");
            const auto entry=GetProcAddress(mapped,"xrGetInstanceProcAddr");
            FreeLibrary(mapped);
            check(entry!=nullptr,"Configured OpenXR loader is missing xrGetInstanceProcAddr");
        }
        if (verify) {
            std::cout<<"PASS: compatible native ABI "<<image.profile()
                <<", mod export and OpenXR loader; no game process created\n"
                <<"Executable SHA256 (diagnostic only): "<<image.digest()<<'\n';
            return 0;
        }
        std::filesystem::path evidence;
        if (headless) {
            evidence=requested_evidence;
            if (!evidence.is_absolute() || std::filesystem::exists(evidence))
                throw std::invalid_argument("Headless evidence directory must be an absent absolute path");
            if (!std::filesystem::is_regular_file(folder/L"OutlastVRHeadlessProbe.dll"))
                throw std::runtime_error("The headset-free probe DLL must be beside this launcher");
            if (!std::filesystem::create_directory(evidence))
                throw std::runtime_error("Cannot create the requested headless evidence directory");
            check(SetEnvironmentVariableW(L"OLVR_HEADLESS_PROBE",L"1")!=FALSE,"Select isolated headless runtime");
            check(SetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",evidence.c_str())!=FALSE,"Set owned probe evidence path");
        } else {
            check(SetEnvironmentVariableW(L"OLVR_HEADLESS_PROBE",nullptr)!=FALSE,"Clear inherited probe selection");
            check(SetEnvironmentVariableW(L"OLVR_PROBE_EVIDENCE",nullptr)!=FALSE,"Clear inherited probe evidence path");
        }
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        if (headless) {
            startup.dwFlags|=STARTF_USESHOWWINDOW;
            startup.wShowWindow=SW_SHOWNOACTIVATE;
        }
        check(SetEnvironmentVariableW(L"OLVR_NATIVE_RESOLUTION",native_resolution?L"1":nullptr)!=FALSE,
            "Select explicit native/headset-recommended resolution");
        std::wstring command=L"\""+game.wstring()+L"\"";
        if (headless) {
            command+=L" -windowed -ResX=1280 -ResY=720 -NoStartupMovies";
            wchar_t collectible[2]{};
            if (GetEnvironmentVariableW(L"OLVR_PROBE_COLLECTIBLE",collectible,2)==1 && collectible[0]==L'1')
                command+=L" -NOSTEAM";
        }
        else if (!native_resolution) command+=L" -windowed";
        check(SetEnvironmentVariableW(L"SteamAppId",L"238320")!=FALSE,"Set the owned launcher's Outlast app identity");
        check(SetEnvironmentVariableW(L"SteamGameId",L"238320")!=FALSE,"Set the owned launcher's Outlast game identity");
        check(CreateProcessW(game.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,
            nullptr,game.parent_path().c_str(),&startup,&process)!=FALSE,"Create user-requested suspended Outlast");
        owned_process.reset(process.hProcess);owned_thread.reset(process.hThread);
        bootstrap_loader(process.hProcess);
        const auto library=dll.wstring();
        RemoteMemory argument(process.hProcess,library.c_str(),(library.size()+1)*sizeof(wchar_t));
        call(process.hProcess,remote_system_function(process.dwProcessId,"LoadLibraryW"),
            argument.get(),"Load the separate mod DLL");
        const auto module=remote_module(process.dwProcessId,dll.filename().wstring());
        const auto status=call(process.hProcess,module+offset,nullptr,"Initialize compatible native renderer and OpenXR hooks");
        if (status!=0) throw std::runtime_error("Mod initialization refused; read logs\\OutlastVRSinglePass.log; status="+std::to_string(status));
        check(ResumeThread(process.hThread)!=DWORD(-1),"Resume user-requested modded Outlast");
        resumed=true;
        if (headless) {
            std::cout<<"Owned headset-free Outlast PID="<<process.dwProcessId<<"; no headset output.\n"<<std::flush;
            wchar_t scenario[8]{};
            const auto scenario_length=GetEnvironmentVariableW(L"OLVR_PROBE_CONTINUE",scenario,UINT(std::size(scenario)));
            if (scenario_length && (scenario_length!=1 || scenario[0]!=L'1'))
                throw std::invalid_argument("Invalid Continue probe selection");
            const auto deadline=GetTickCount64()+(scenario_length?300000:120000);
            bool complete=false;
            for (;;) {
                const auto waited=WaitForSingleObject(process.hProcess,100);
                if (waited==WAIT_OBJECT_0) {
                    DWORD code=0;
                    check(GetExitCodeProcess(process.hProcess,&code)!=FALSE,"Read actual Outlast exit");
                    throw std::runtime_error("Outlast exited before capture completion; exit="+std::to_string(code));
                }
                check(waited==WAIT_TIMEOUT,"Monitor owned native verification session");
                if (std::filesystem::is_regular_file(evidence/L"capture-error.txt"))
                    throw std::runtime_error("Native probe reported an error; read capture-error.txt");
                if (std::filesystem::is_regular_file(evidence/L"capture-complete.txt")) {complete=true;break;}
                if (GetTickCount64()>=deadline) break;
            }
            const auto terminated=TerminateProcess(process.hProcess,complete?0:ERROR_TIMEOUT);
            const auto termination_error=terminated?ERROR_SUCCESS:GetLastError();
            const auto shutdown=WaitForSingleObject(process.hProcess,60000);
            if (shutdown==WAIT_TIMEOUT)
                throw std::runtime_error("Owned Outlast shutdown exceeded 60 seconds; active-process evidence is not complete");
            check(shutdown==WAIT_OBJECT_0,"Wait for owned Outlast shutdown");
            if (!terminated) {
                DWORD code=STILL_ACTIVE;
                check(GetExitCodeProcess(process.hProcess,&code)!=FALSE,"Read concurrently completed owned Outlast exit");
                if (termination_error!=ERROR_ACCESS_DENIED || !complete || code!=0)
                    throw std::runtime_error("Owned Outlast termination failed; Win32="+
                        std::to_string(termination_error)+"; confirmed_exit="+std::to_string(code));
            }
            if (!complete) throw std::runtime_error("Native capture timed out; incomplete evidence is not a pass");
            std::cout<<"Native frame capture completed. Images and driver trace require independent grading.\n";
            return 0;
        }
        std::cout<<"Outlast resumed with the single-pass mod. F8 recenters tracking.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"ERROR: "<<error.what()<<'\n';
        if (owned_process.get() && WaitForSingleObject(owned_process.get(),0)==WAIT_TIMEOUT &&
            (!resumed || headless) &&
            !TerminateProcess(owned_process.get(),ERROR_DLL_INIT_FAILED))
            std::cerr<<"ERROR: stopping the owned suspended process failed; Win32="<<GetLastError()<<'\n';
        if (owned_process.get() && headless &&
            WaitForSingleObject(owned_process.get(),10000)!=WAIT_OBJECT_0)
            std::cerr<<"ERROR: owned probe shutdown was not confirmed; do not restore files while it runs\n";
        return 1;
    }
}
