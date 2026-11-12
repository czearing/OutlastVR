#define main session_case_main
#include "test_openxr_session.cpp"
#undef main
#include <new>

int main(int argc,char** argv) {
    try {
        unsigned cycles=16;
        if (argc==3 && std::string(argv[1])=="--cycles") {
            const auto text=std::string(argv[2]);
            std::size_t end=0;
            const auto parsed=std::stoul(text,&end);
            if (end!=text.size() || !parsed || parsed>4096)
                throw std::invalid_argument("Session cycles must be 1..4096");
            cycles=static_cast<unsigned>(parsed);
        } else if (argc!=1) throw std::invalid_argument("Usage: test_openxr_lifecycle [--cycles 1..4096]");
        const std::array<const char*,8> modes{{"","--tracking-resume","--asymmetric-extents",
            "--invalid-pose","--session-restart","--reference-stage","--reference-local","--motion-focus"}};
        for (unsigned cycle=0;cycle<cycles;++cycle) {
            // All sessions and GPU resources from the previous case have been destroyed.
            mock.~Mock();new(&mock) Mock{};
            motion_mock::fake=motion_mock::Fake{};
            release_observed=false;gate_opened=false;
            auto mode=std::string(modes[cycle%modes.size()]);
            char* arguments[]{argv[0],mode.data()};
            if (session_case_main(mode.empty()?1:2,arguments)!=0)
                throw std::runtime_error("Repeated in-process XR lifecycle failed at cycle "+std::to_string(cycle));
            if (mock.destroyed!=4 || motion_mock::fake.destroyed_spaces!=4 ||
                motion_mock::fake.destroyed_sets!=1 || !motion_mock::fake.spaces.empty() ||
                motion_mock::fake.vibrating[0] || motion_mock::fake.vibrating[1])
                throw std::runtime_error("Repeated session retained owned XR resources or haptics");
        }
        std::cout<<"PASS: repeated_in_process_xr_lifecycles="<<cycles
            <<"; exact_owned_handle_cleanup; per_eye_gpu_pixels; tracking_focus_reference_restart_matrix\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
