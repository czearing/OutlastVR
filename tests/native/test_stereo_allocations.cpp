#include "indexed_stereo.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

static std::size_t allocations=0;
static bool counting=false;

#if defined(__GNUC__)
#define ALLOCATION_NOINLINE __attribute__((noinline))
#else
#define ALLOCATION_NOINLINE
#endif
ALLOCATION_NOINLINE void* operator new(std::size_t size) {
    if (counting) ++allocations;
    if (auto* result=std::malloc(size?size:1)) return result;
    throw std::bad_alloc();
}
ALLOCATION_NOINLINE void* operator new[](std::size_t size) {return ::operator new(size);}
ALLOCATION_NOINLINE void operator delete(void* pointer) noexcept {std::free(pointer);}
ALLOCATION_NOINLINE void operator delete[](void* pointer) noexcept {std::free(pointer);}
ALLOCATION_NOINLINE void operator delete(void* pointer,std::size_t) noexcept {std::free(pointer);}
ALLOCATION_NOINLINE void operator delete[](void* pointer,std::size_t) noexcept {std::free(pointer);}
#undef ALLOCATION_NOINLINE

class Device final:public olvr::IndexedDevice {
public:
    std::array<std::uint32_t,16> frequencies{};
    std::uint32_t draws=0;
    std::int32_t get_frequency(std::uint32_t slot,std::uint32_t& value) noexcept override {
        value=frequencies[slot];return 0;
    }
    std::int32_t set_frequency(std::uint32_t slot,std::uint32_t value) noexcept override {
        frequencies[slot]=value;return 0;
    }
    std::int32_t draw_indexed(const olvr::Draw&) noexcept override {
        ++draws;return 0;
    }
};

int main() {
    try {
        const std::vector<olvr::Stream> streams{{0,olvr::StreamRate::vertex},
            {2,olvr::StreamRate::object_instance,3}};
        olvr::IndexedStereoPlan cached{};
#ifndef OLVR_ALLOCATION_BASELINE
        olvr::plan_indexed_stereo(olvr::DrawKind::indexed,true,1,1,16,15,streams,cached);
#else
        (void)cached;
#endif
        Device device;
        device.frequencies.fill(1);
        const auto before=device.frequencies;
        constexpr unsigned count=100000;
        const auto start=std::chrono::steady_clock::now();
        counting=true;
        for (unsigned index=0;index<count;++index) {
#ifdef OLVR_ALLOCATION_BASELINE
            const auto plan=olvr::plan_indexed_stereo(olvr::DrawKind::indexed,true,1,index%127+1,16,15,streams);
#else
            olvr::plan_indexed_stereo(olvr::DrawKind::indexed,true,1,index%127+1,16,15,streams,cached);
            const auto& plan=cached;
#endif
            const auto result=olvr::submit_indexed_stereo(device,plan,{4,0,0,3,0,1});
            if (!result.succeeded() || device.frequencies!=before)
                throw std::runtime_error("Allocation benchmark lost single submission or caller stream state");
        }
        counting=false;
        if (device.draws!=count) throw std::runtime_error("Scheduler did not issue exactly one draw per submission");
        const auto elapsed=std::chrono::duration<double,std::micro>(
            std::chrono::steady_clock::now()-start).count();
#ifndef OLVR_ALLOCATION_BASELINE
        if (allocations) throw std::runtime_error("Steady-state stereo scheduling still allocates");
#else
        if (!allocations) throw std::runtime_error("Old scheduler control failed to expose allocation pressure");
#endif
        std::cout<<"PASS: draws="<<device.draws<<"; heap_allocations="<<allocations
            <<"; scheduling_us_per_draw="<<elapsed/count<<"; caller_frequencies_preserved; not_GPU_or_headset_FPS\n";
    } catch (const std::exception& error) {
        counting=false;std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
