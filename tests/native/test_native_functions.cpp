#include "outlast_native_pe.h"
#include "sha256.h"
#include <iostream>

using namespace olvr;
static unsigned checks=0;
static void require(bool value,const char* text) {
    ++checks;if (!value) throw std::runtime_error(text);
}
template<class T> static void put(std::vector<unsigned char>& bytes,std::size_t offset,T value) {
    if (offset>bytes.size() || sizeof(value)>bytes.size()-offset)
        throw std::runtime_error("Invalid test mutation");
    std::memcpy(bytes.data()+offset,&value,sizeof(value));
}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2 || (argc==3 && std::wstring(argv[2])==L"--alternate"),
            "Specify a locally owned executable and optional --alternate; neither will be executed");
        const bool alternate=argc==3;
        std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
        require(bool(input),"Cannot read native function fixture");
        const auto size=input.tellg();
        require(size>0 && size<=128*1024*1024,"Invalid native function fixture extent");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));input.seekg(0);
        require(bool(input.read(reinterpret_cast<char*>(bytes.data()),size)),"Incomplete native function fixture");
        const OutlastNativePE image(bytes);
        const OutlastNativeFunctions functions(image);
        const std::uint32_t indexed=alternate?0x22f1ce0:0xbf5b60;
        const auto& group=functions.at(indexed);
        const auto body=functions.body(image,indexed);
        require(group.ranges.size()==(alternate?3u:7u),"Chained indexed ranges were lost");
        require(body.size()==(alternate?890u:627u),"Indexed body was truncated to its primary unwind record");
        require(sha256_bytes(body.data(),body.size())==(alternate?
            "cdec97babbbb8c640f74461088ffa51c135ddf1e83da8954e19222b9955897e8":
            "ff6939df22849635bde7285b764f3d7f5c6fa5f53f5a0d991dcbcd98049ff263"),
            "Reconstructed native body differs from independent REA evidence");
        require(functions.containing(group.ranges.back().rva)->entry==indexed,
            "A chained continuation was misidentified as another function");
        bool refused=false;
        try {functions.at(group.ranges[1].rva);} catch (const std::invalid_argument&) {refused=true;}
        require(refused,"A continuation was admitted as a primary function");
        require(!functions.containing(0xffffffffu),"An unmapped address acquired a native function owner");
        const auto directory=image.directory(3);
        const auto first=functions.records().front();
        const auto child=std::find_if(functions.records().begin(),functions.records().end(),
            [&](const auto& entry) {return entry.begin==group.ranges[1].rva;});
        require(child!=functions.records().end(),"Missing chained test record");
        const auto header=image.value<std::uint32_t>(child->unwind);
        const auto chain=child->unwind+4+((((header>>16)&255u)+1)&~1u)*2;
        const auto chain_raw=image.raw_offset(chain,12);
        const auto pe=std::size_t(image.value<std::uint32_t>(0x3c));
        const auto optional=pe+24;
        auto fault=[&](auto mutate,const char* message) {
            auto broken=bytes;mutate(broken);
            bool rejected=false;
            try {const OutlastNativePE candidate(broken);const OutlastNativeFunctions index(candidate);}
            catch (const std::invalid_argument&) {rejected=true;}
            require(rejected,message);
        };
        fault([&](auto& b) {put(b,optional+112+3*8,std::uint32_t(0xfffffff0));},
            "An out-of-file exception directory was accepted");
        fault([&](auto& b) {put(b,optional+116+3*8,directory.size-1);},
            "A partial runtime-function record was accepted");
        fault([&](auto& b) {put(b,image.raw_offset(directory.rva+4,4),first.begin);},
            "An empty executable function range was accepted");
        fault([&](auto& b) {put(b,chain_raw,child->begin+1);},
            "An inexact chained parent was accepted");
        fault([&](auto& b) {
            put(b,chain_raw,child->begin);put(b,chain_raw+4,child->end);put(b,chain_raw+8,child->unwind);
        },"A self-referential unwind chain was accepted");
        fault([&](auto& b) {put(b,image.raw_offset(child->unwind,1),std::uint8_t(0));},
            "An invalid unwind version was accepted");
        fault([&](auto& b) {
            const auto code=image.section(first.begin);
            const auto row=std::size_t(code-image.sections().data());
            const auto table=optional+image.value<std::uint16_t>(std::uint32_t(pe+20));
            put(b,table+row*40+36,code->flags&~std::uint32_t(0x20000000));
        },"A non-executable runtime-function body was accepted");
        std::cout<<"PASS: "<<checks<<" native PE/unwind checks; "<<group.ranges.size()
            <<" native indexed fragments, "<<body.size()<<" exact bytes; no target execution\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
