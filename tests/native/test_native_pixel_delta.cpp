#include "native_pixel_delta.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

static std::vector<char> read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open original native pixel evidence");
    return std::vector<char>(std::istreambuf_iterator<char>(file),{});
}

int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=4 && !(argc==5 && std::wstring(argv[4])==L"--xor"))
            throw std::runtime_error("Usage: test_native_pixel_delta before after new-output [--xor]");
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output))
            throw std::runtime_error("Pixel-delta test refuses to overwrite evidence");
        const auto before=read(argv[1]),after=read(argv[2]);
        std::ofstream file(output,std::ios::binary);
        if (argc==5) olvr::write_native_pixel_xor(file,before,after);
        else olvr::write_native_pixel_delta(file,before,after);
        file.close();
        if (!file) throw std::runtime_error("Cannot complete native pixel-delta test output");
        unsigned rejected=0;
        for (const auto& invalid:std::vector<std::vector<char>>{{},std::vector<char>(7),std::vector<char>(16)}) {
            try {
                std::ostringstream buffer;
                olvr::write_native_pixel_delta(buffer,std::vector<char>(8),invalid);
            } catch (const std::runtime_error&) {++rejected;}
        }
        if (rejected!=3) throw std::runtime_error("Pixel-delta writer accepted invalid extents");
        std::cout<<"PASS: native_pixel_delta_writer; invalid_extents_rejected="<<rejected
                 <<"; raw_bytes="<<after.size()<<"; stored_bytes="<<std::filesystem::file_size(output)<<'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
