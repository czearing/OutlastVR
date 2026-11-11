#include "isolated_d3d9.h"
#include "sm3_tracked_uniforms.h"
#include <iostream>

using namespace olvr;

int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=11)
            throw std::invalid_argument("Specify rejected shader directory, original/regenerate, atlas/point masks and six native boolean options");
        const std::filesystem::path directory(argv[1]);
        const std::wstring mode(argv[2]);
        if (mode!=L"original" && mode!=L"regenerate")
            throw std::invalid_argument("Unknown native shader rejection replay");
        auto number=[&](unsigned index,unsigned maximum) {
            const std::wstring text(argv[index]);
            std::size_t consumed=0;
            const auto value=std::stoul(text,&consumed);
            if (consumed!=text.size() || value>maximum)
                throw std::invalid_argument("Invalid original native shader option");
            return static_cast<unsigned>(value);
        };
        const auto load=[&](const char* name) {
            const auto words=read_shader(directory/name);
            return sm3::Words(words.begin(),words.end());
        };
        const auto native_vertex=load("shader-rejection-original-vs.bin");
        const auto native_pixel=load("shader-rejection-original-ps.bin");
        auto vertex=load("shader-rejection-patched-vs.bin");
        auto pixel=load("shader-rejection-patched-ps.bin");
        const bool tracking=number(9,1)!=0;
        const sm3::PixelOptions options{number(3,65535),number(5,1)!=0,number(6,1)!=0,
            number(7,1)!=0,number(8,1)!=0,tracking,number(10,1)!=0,number(4,65535)};
        if (mode==L"regenerate") {
            auto pair=sm3::patch_pair(native_vertex,native_pixel,options);
            if (tracking) pair=sm3::track_uniforms(std::move(pair)).pair;
            std::cout<<"Regenerated pixel matches rejected native bytes="<<(pixel==pair.pixel)<<'\n';
            vertex=std::move(pair.vertex);pixel=std::move(pair.pixel);
        }
        IsolatedDevice isolated(64,32);
        auto& device=*isolated.device.get();
        const std::vector<DWORD> vertex_words(vertex.begin(),vertex.end()),pixel_words(pixel.begin(),pixel.end());
        Com<IDirect3DVertexShader9> vs;
        Com<IDirect3DPixelShader9> ps;
        gpu_check(device.CreateVertexShader(vertex_words.data(),vs.put()),"Replay actual rejected native vertex shader");
        gpu_check(device.CreatePixelShader(pixel_words.data(),ps.put()),"Replay actual rejected native pixel shader");
        std::cout<<"PASS: actual rejected native shader pair and exact original renderer configuration admitted\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
