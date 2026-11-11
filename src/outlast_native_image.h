#pragma once
#include "sha256.h"
#include <cstring>
#include <type_traits>
#include <vector>

namespace olvr {

inline constexpr const char* outlast_retail_sha256=
    "06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96";

class OutlastNativeImage {
    std::vector<unsigned char> bytes_;
    std::size_t sections_=0;
    unsigned section_count_=0;
    template<class T> T read(std::size_t offset) const {
        static_assert(std::is_integral_v<T>);
        if (offset>bytes_.size() || sizeof(T)>bytes_.size()-offset)
            throw std::invalid_argument("Retail PE field exceeds the exact image extent");
        T value{};std::memcpy(&value,bytes_.data()+offset,sizeof(value));return value;
    }
public:
    explicit OutlastNativeImage(const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if (!input) throw std::runtime_error("Cannot read the selected retail Outlast image");
        const auto size=input.tellg();
        if (size<64 || size>128*1024*1024)
            throw std::invalid_argument("Outlast image extent is outside the exact-build bounds");
        bytes_.resize(std::size_t(size));input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes_.data()),size))
            throw std::runtime_error("Retail Outlast image read was incomplete");
        if (sha256_bytes(bytes_.data(),bytes_.size())!=outlast_retail_sha256)
            throw std::invalid_argument("Unsupported Outlast executable SHA256; native hooks must not be installed");
        if (read<std::uint16_t>(0)!=0x5a4d) throw std::invalid_argument("Invalid retail DOS signature");
        const auto pe=std::size_t(read<std::uint32_t>(0x3c));
        if (pe>bytes_.size() || bytes_.size()-pe<24 ||
            read<std::uint32_t>(pe)!=0x4550 || read<std::uint16_t>(pe+4)!=0x8664)
            throw std::invalid_argument("Retail image is not a native x64 PE");
        const auto optional_size=read<std::uint16_t>(pe+20);
        if (optional_size<112 || read<std::uint16_t>(pe+24)!=0x20b ||
            read<std::uint64_t>(pe+48)!=0x140000000ull)
            throw std::invalid_argument("Retail image has an unexpected PE32+ layout or preferred base");
        section_count_=read<std::uint16_t>(pe+6);
        sections_=pe+24+optional_size;
        if (!section_count_ || section_count_>96 || sections_>bytes_.size() ||
            section_count_>(bytes_.size()-sections_)/40)
            throw std::invalid_argument("Retail section table exceeds the image");
    }
    std::vector<unsigned char> body(std::uint32_t rva,std::size_t size) const {
        if (!size) throw std::invalid_argument("Native body cannot be empty");
        std::size_t selected=0;unsigned matches=0;
        for (unsigned index=0;index<section_count_;++index) {
            const auto section=sections_+index*40;
            const auto address=read<std::uint32_t>(section+12);
            const auto raw_size=read<std::uint32_t>(section+16);
            const auto raw=std::size_t(read<std::uint32_t>(section+20));
            if (rva<address || std::uint64_t(rva)-address>=raw_size) continue;
            const auto displacement=std::size_t(rva-address);
            if (size>raw_size-displacement || raw>bytes_.size() ||
                displacement>bytes_.size()-raw || size>bytes_.size()-raw-displacement)
                throw std::invalid_argument("Native body crosses its mapped retail section");
            selected=raw+displacement;++matches;
        }
        if (matches!=1) throw std::invalid_argument("Native body does not have one exact retail mapping");
        return {bytes_.begin()+selected,bytes_.begin()+selected+size};
    }
};

inline std::vector<unsigned char> outlast_proof_body(
    const std::filesystem::path& path,std::uint32_t rva,std::size_t size,const char* digest) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read the selected native proof input");
    std::vector<unsigned char> bytes;
    if (input.tellg()==std::streampos(size)) {
        bytes.resize(size);input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),size))
            throw std::runtime_error("Private native body fixture read was incomplete");
    } else bytes=OutlastNativeImage(path).body(rva,size);
    if (sha256_bytes(bytes.data(),bytes.size())!=digest)
        throw std::invalid_argument("Native body differs from the exact MCP-recovered SHA256");
    return bytes;
}

}
