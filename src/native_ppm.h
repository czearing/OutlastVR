#pragma once
#include <windows.h>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace olvr {
inline void write_native_ppm_rgb(std::ostream& output,const DWORD* pixels,std::size_t count,
                                 UINT width,UINT height,UINT stride=0) {
    if (!stride) stride=width;
    if (!pixels || !width || !height || width>16384 || height>8192 ||
        stride<width || stride>32768 || count<std::size_t(height-1)*stride+width)
        throw std::invalid_argument("Invalid native RGB image extent");
    std::vector<char> row(std::size_t(width)*3);
    for (UINT y=0;y<height;++y) {
        for (UINT x=0;x<width;++x) {
            const auto pixel=pixels[std::size_t(y)*stride+x];
            row[x*3]=char(pixel>>16);row[x*3+1]=char(pixel>>8);row[x*3+2]=char(pixel);
        }
        output.write(row.data(),std::streamsize(row.size()));
        if (!output) throw std::runtime_error("Cannot retain native RGB image row");
    }
}
}
