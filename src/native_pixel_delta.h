#pragma once
#include <cstdint>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace olvr {

inline void write_native_pixel_runs(std::ostream& output,const std::vector<char>& before,
                                    const std::vector<char>& pixels,bool xor_difference) {
    if (before.empty() || before.size()!=pixels.size() || before.size()%8 ||
        before.size()/8>0x7fffffff)
        throw std::runtime_error("Invalid lossless native pixel-delta extent");
    const auto integer=[&](std::uint64_t value,unsigned bytes) {
        for (unsigned i=0;i<bytes;++i) output.put(static_cast<char>((value>>(i*8))&255));
    };
    output.write("OLVRPD01",8);
    integer(pixels.size(),8);
    const auto count=pixels.size()/8;
    for (std::size_t first=0;first<count;) {
        const bool changed=std::memcmp(before.data()+first*8,pixels.data()+first*8,8)!=0;
        auto end=first+1;
        while (end<count && (std::memcmp(before.data()+end*8,pixels.data()+end*8,8)!=0)==changed)
            ++end;
        integer(std::uint32_t(end-first)|(changed?0x80000000u:0u),4);
        if (changed) {
            if (xor_difference) {
                for (auto i=first*8;i<end*8;++i)
                    output.put(static_cast<char>(static_cast<unsigned char>(before[i])^
                                                  static_cast<unsigned char>(pixels[i])));
            } else output.write(pixels.data()+first*8,std::streamsize((end-first)*8));
        }
        first=end;
    }
    if (!output) throw std::runtime_error("Cannot retain lossless native pixel-delta evidence");
}

inline void write_native_pixel_delta(std::ostream& output,const std::vector<char>& before,
                                     const std::vector<char>& pixels) {
    write_native_pixel_runs(output,before,pixels,false);
}

inline void write_native_pixel_xor(std::ostream& output,const std::vector<char>& left,
                                   const std::vector<char>& right) {
    write_native_pixel_runs(output,left,right,true);
}

}
