#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace olvr {
// Fixed native addresses also depend on code, vtables, globals and unwind data.
struct OutlastNativeSection {
    std::string_view name;
    std::uint32_t rva,size,flags;
    const char* sha256;
};
inline constexpr std::string_view outlast_native_profile="outlast-win64-native-1";
inline constexpr std::array outlast_native_sections{
    OutlastNativeSection{".text",0x1000,0x145e0af,0x60000020,
        "89cb66ca9e9a616e7b6fb32768ce9aa57ddc2c923018e20166ea8c644b70f1b6"},
    OutlastNativeSection{".rdata",0x1460000,0x99aaa0,0x40000040,
        "d007033684a1114862583f7ed2ecac2f29a715c23e0e37a847119eeccaa9ce69"},
    OutlastNativeSection{".data",0x1dfb000,0x24b880,0xc0000040,
        "ed1e30d5a6b697a2ca59404ed103b2914b9308711147c83d833b960ff215ea4f"},
    OutlastNativeSection{".pdata",0x2047000,0x10dd18,0x40000040,
        "22d4ddbf12ca8e9fa125940ebc499340e07e2e3865eeb0b3b7a8b5c8420a7bf9"},
    OutlastNativeSection{".reloc",0x216b000,0xc0122,0x42000040,
        "83f17f86e8f52af390fb73513c382c645371d73e4ea7954aceef28139341b1a7"},
};
struct OutlastNativeRange {std::uint32_t rva,size;};
// Only these source-bound debug ranges are ignored, never ranges supplied by a candidate.
inline constexpr std::array outlast_native_debug{
    OutlastNativeRange{0x162de70,28},OutlastNativeRange{0x1b5da98,99}
};
inline constexpr std::array<OutlastNativeRange,16> outlast_native_directories{{
    {0x1df6d30,0x3d70},{0x1df2a20,0x294},{0,0},{0x2047000,0x10dd18},
    {0,0},{0x216b000,0x94724},{0x162de70,0x1c},{0,0},
    {0,0},{0,0},{0,0},{0,0},{0x1460000,0x1478},{0x1df270c,0xa0},{0,0},{0,0}
}};
}
