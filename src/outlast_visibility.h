#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace olvr {

// Applies only to owned live views; renderer destruction must occur after restoration.
class OutlastVisibilityScope {
    struct Saved { unsigned char* view; std::int32_t count; };
    std::vector<Saved> saved_;
public:
    OutlastVisibilityScope(void* views,std::size_t bytes,std::uint32_t count) {
        constexpr std::size_t stride=0x12c0,offset=0x3b0+0x110;
        if (!views || !count || count>64 || bytes/stride<count)
            throw std::invalid_argument("Invalid bounded owned Outlast visibility views");
        saved_.reserve(count);
        for (std::uint32_t i=0;i<count;++i) {
            auto* view=static_cast<unsigned char*>(views)+i*stride;
            std::int32_t planes=0;
            std::memcpy(&planes,view+offset,sizeof(planes));
            if (planes<0 || planes>4096 || planes%4)
                throw std::invalid_argument("Invalid native permuted frustum plane count");
            saved_.push_back({view,planes});
        }
        const std::int32_t unbounded=0;
        for (const auto& item:saved_)
            std::memcpy(item.view+offset,&unbounded,sizeof(unbounded));
    }
    ~OutlastVisibilityScope() {
        for (const auto& item:saved_)
            std::memcpy(item.view+0x3b0+0x110,&item.count,sizeof(item.count));
    }
    OutlastVisibilityScope(const OutlastVisibilityScope&)=delete;
    OutlastVisibilityScope& operator=(const OutlastVisibilityScope&)=delete;
};

}
