#include "native_ppm.h"
#include <iostream>
#include <sstream>
#include <string>

namespace {
struct CountedBuffer:std::stringbuf {
    unsigned writes=0;
    std::streamsize xsputn(const char* data,std::streamsize count) override {
        ++writes;return std::stringbuf::xsputn(data,count);
    }
};
void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    try {
        for (const auto extent:{std::pair<UINT,UINT>{3,2},{256,1},{2688,2880}}) {
            const auto width=extent.first,height=extent.second;
            std::vector<DWORD> pixels(std::size_t(width)*height);
            std::string expected;expected.reserve(pixels.size()*3);
            for (std::size_t i=0;i<pixels.size();++i) {
                const DWORD pixel=DWORD(i*0x010307u);pixels[i]=pixel;
                expected.push_back(char(pixel>>16));expected.push_back(char(pixel>>8));
                expected.push_back(char(pixel));
            }
            CountedBuffer buffer;std::ostream output(&buffer);
            olvr::write_native_ppm_rgb(output,pixels.data(),pixels.size(),width,height);
            require(buffer.str()==expected,"Buffered native RGB differs from the original per-pixel writer");
            require(buffer.writes==height,"Native RGB writer did not emit exactly one write per row");
        }
        std::vector<DWORD> atlas{1,2,3,4,5,6,7,8,9,10,11,12};
        for (unsigned eye=0;eye<2;++eye) {
            std::string expected;
            for (unsigned y=0;y<2;++y) for (unsigned x=0;x<3;++x) {
                expected.push_back(0);expected.push_back(0);
                expected.push_back(char(atlas[y*6+eye*3+x]));
            }
            std::ostringstream output;
            olvr::write_native_ppm_rgb(output,atlas.data()+eye*3,atlas.size()-eye*3,3,2,6);
            require(output.str()==expected,"Native RGB writer mixed strided stereo eyes");
        }
        DWORD pixel=0;
        unsigned rejected=0;
        for (const auto extent:{std::pair<UINT,UINT>{0,1},{1,0},{2,1},{16385,1},{1,8193}}) {
            std::ostringstream output;
            try {olvr::write_native_ppm_rgb(output,&pixel,1,extent.first,extent.second);}
            catch (const std::invalid_argument&) {++rejected;}
        }
        require(rejected==5,"Native RGB accepted an invalid extent or truncated pixels");
        std::ostringstream failed;failed.setstate(std::ios::badbit);
        bool surfaced=false;
        try {olvr::write_native_ppm_rgb(failed,&pixel,1,1,1);}
        catch (const std::runtime_error&) {surfaced=true;}
        require(surfaced,"Native RGB silently ignored an output failure");
        std::cout<<"PASS: native RGB byte equivalence; all channel values; real portrait extent; stereo strides; row-bounded writes; invalid extents and output failures rejected\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
