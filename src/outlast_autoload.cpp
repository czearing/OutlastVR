#include <windows.h>
#include <d3d9.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
HMODULE own_module=nullptr;
std::filesystem::path module_path(HMODULE module) {
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if (!length || length>=path.size()) throw std::runtime_error("Cannot locate Outlast VR autoloader");
    return std::wstring(path.data(),length);
}
template<class F> F symbol(HMODULE module,const char* name) {
    const auto address=GetProcAddress(module,name);
    if (!address) throw std::runtime_error(std::string("Missing D3D9 entry: ")+name);
    F result=nullptr;
    static_assert(sizeof(result)==sizeof(address));
    std::memcpy(&result,&address,sizeof(result));
    return result;
}
HMODULE system_d3d9() {
    static const HMODULE module=[] {
        std::array<wchar_t,32768> folder{};
        const auto length=GetSystemDirectoryW(folder.data(),UINT(folder.size()));
        if (!length || length>=folder.size()) throw std::runtime_error("Cannot locate system D3D9");
        const auto path=std::filesystem::path(std::wstring(folder.data(),length))/L"d3d9.dll";
        const auto loaded=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loaded) throw std::runtime_error("Cannot load absolute system D3D9");
        return loaded;
    }();
    return module;
}
void report(const std::exception& error) {
    OutputDebugStringA(error.what());
    MessageBoxA(nullptr,error.what(),"Outlast VR could not start",MB_OK|MB_ICONERROR);
}
}

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT version) {
    try {
        if (_wcsicmp(module_path(nullptr).filename().c_str(),L"OLGame.exe")!=0)
            return symbol<IDirect3D9*(WINAPI*)(UINT)>(system_d3d9(),"Direct3DCreate9")(version);
        static const HMODULE runtime=[] {
            const auto path=module_path(own_module).parent_path()/L"OutlastVRSinglePass.dll";
            const auto loaded=LoadLibraryExW(path.c_str(),nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!loaded) throw std::runtime_error("Cannot load OutlastVRSinglePass.dll beside d3d9.dll");
            return loaded;
        }();
        return symbol<IDirect3D9*(WINAPI*)(UINT)>(runtime,"OutlastVR_Create9")(version);
    } catch (const std::exception& error) {report(error);return nullptr;}
}
extern "C" __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT version,IDirect3D9Ex** output) {
    try {
        return symbol<HRESULT(WINAPI*)(UINT,IDirect3D9Ex**)>(system_d3d9(),"Direct3DCreate9Ex")(version,output);
    } catch (const std::exception& error) {report(error);return E_FAIL;}
}
extern "C" __declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR color,LPCWSTR name) {
    return symbol<int(WINAPI*)(D3DCOLOR,LPCWSTR)>(system_d3d9(),"D3DPERF_BeginEvent")(color,name);
}
extern "C" __declspec(dllexport) int WINAPI D3DPERF_EndEvent() {
    return symbol<int(WINAPI*)()>(system_d3d9(),"D3DPERF_EndEvent")();
}
extern "C" __declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus() {
    return symbol<DWORD(WINAPI*)()>(system_d3d9(),"D3DPERF_GetStatus")();
}
extern "C" __declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    return symbol<BOOL(WINAPI*)()>(system_d3d9(),"D3DPERF_QueryRepeatFrame")();
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR color,LPCWSTR name) {
    symbol<void(WINAPI*)(D3DCOLOR,LPCWSTR)>(system_d3d9(),"D3DPERF_SetMarker")(color,name);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR color,LPCWSTR name) {
    symbol<void(WINAPI*)(D3DCOLOR,LPCWSTR)>(system_d3d9(),"D3DPERF_SetRegion")(color,name);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD options) {
    symbol<void(WINAPI*)(DWORD)>(system_d3d9(),"D3DPERF_SetOptions")(options);
}
extern "C" __declspec(dllexport) void WINAPI DebugSetMute() {
    symbol<void(WINAPI*)()>(system_d3d9(),"DebugSetMute")();
}
extern "C" __declspec(dllexport) void WINAPI DebugSetLevel(DWORD level) {
    symbol<void(WINAPI*)(DWORD)>(system_d3d9(),"DebugSetLevel")(level);
}
extern "C" __declspec(dllexport) void* WINAPI Direct3DShaderValidatorCreate9() {
    return symbol<void*(WINAPI*)()>(system_d3d9(),"Direct3DShaderValidatorCreate9")();
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {own_module=module;DisableThreadLibraryCalls(module);}
    return TRUE;
}
