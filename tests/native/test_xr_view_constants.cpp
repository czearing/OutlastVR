#include "xr_view_constants.h"
#include <iostream>
#include <limits>

using namespace olvr;

namespace {
UINT checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::array<double, 4> clip(const std::array<float, 32>& matrix, Vec3 point) {
    std::array<double, 4> result{};
    for (UINT row = 0; row < 4; ++row)
        result[row] = double(matrix[row*4])*point.x + double(matrix[row*4+1])*point.y +
                      double(matrix[row*4+2])*point.z + matrix[row*4+3];
    return result;
}
void close(double actual, double expected, const char* message, double tolerance = .0001) {
    check(std::isfinite(actual) && std::abs(actual-expected) <= tolerance, message);
}
template<class F> void refuses(F operation) {
    bool refused = false;
    try { operation(); }
    catch (const std::invalid_argument&) { refused = true; }
    check(refused, "Invalid eye transform was accepted");
}
}

int main(int argc, char** argv) {
    try {
        const bool fault = argc == 2 && std::string(argv[1]) == "--fault-projection";
        if (argc > 2 || (argc == 2 && !fault))
            throw std::invalid_argument("Usage: test_xr_view_constants.exe [--fault-projection]");
        const TrackingFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,-1},1,{0,0,0}};
        const EyePose identity{{0,0,0},{0,0,0,1}};
        const EyeFov fov{-.8f,.9f,.7f,-.6f};
        auto matrix = eye_constants(frame, identity, fov, .1f, 100.f);
        if (fault) matrix[0] *= 1.15f;
        for (const float z : {.1f,1.f,2.f,6.f,100.f}) {
            auto left = clip(matrix, {z*std::tan(fov.left),0,z});
            auto right = clip(matrix, {z*std::tan(fov.right),0,z});
            auto up = clip(matrix, {0,z*std::tan(fov.up),z});
            auto down = clip(matrix, {0,z*std::tan(fov.down),z});
            close(left[0]/left[3], -1, "Left submitted FOV differs from rendered projection");
            close(right[0]/right[3], 1, "Right submitted FOV differs from rendered projection");
            close(up[1]/up[3], 1, "Up submitted FOV differs from rendered projection");
            close(down[1]/down[3], -1, "Down submitted FOV differs from rendered projection");
        }
        const auto near_clip = clip(matrix, {0,0,.1f}), far_clip = clip(matrix, {0,0,100});
        close(near_clip[2]/near_clip[3], 0, "D3D near depth is wrong");
        close(far_clip[2]/far_clip[3], 1, "D3D far depth is wrong");
        const EyeFov symmetric{-.785398163f,.785398163f,.785398163f,-.785398163f};
        for (const float depth : {1.f,2.f,6.f,20.f}) {
            const auto left = eye_constants(frame, {{-.032f,0,0},{0,0,0,1}}, symmetric, .1f,100);
            const auto right = eye_constants(frame, {{.032f,0,0},{0,0,0,1}}, symmetric, .1f,100);
            const auto a = clip(left, {0,0,depth}), b = clip(right, {0,0,depth});
            close(128*(b[0]/b[3]-a[0]/a[3]), -256*.064/(2*depth), "Physical IPD disparity is wrong");
        }
        const double root_half = std::sqrt(.5);
        // Independent cardinal outcomes avoid testing quaternion rotation against itself.
        struct RotationCase { Quaternion pose; Vec3 forward, right, up; };
        const float h = static_cast<float>(root_half);
        const RotationCase rotations[]{
            {{0,0,0,1},{0,0,1},{1,0,0},{0,1,0}},
            {{0,h,0,h},{-1,0,0},{0,0,1},{0,1,0}},
            {{0,-h,0,h},{1,0,0},{0,0,-1},{0,1,0}},
            {{h,0,0,h},{0,1,0},{1,0,0},{0,0,-1}},
            {{-h,0,0,h},{0,-1,0},{1,0,0},{0,0,1}},
            {{0,0,h,h},{0,0,1},{0,1,0},{-1,0,0}},
            {{0,1,0,0},{0,0,-1},{-1,0,0},{0,1,0}},
            {{0,0,1,0},{0,0,1},{-1,0,0},{0,-1,0}}
        };
        for (const auto& rotation : rotations) {
            matrix = eye_constants(frame, {{0,0,0},rotation.pose}, symmetric,.1f,100);
            auto center = clip(matrix, scale(rotation.forward,5));
            close(center[0]/center[3],0,"Rotated forward direction is not centered");
            close(center[1]/center[3],0,"Rotated forward direction is not centered vertically");
            close(center[3],5,"Rotated forward depth has the wrong sign");
            auto right = clip(matrix, add(scale(rotation.forward,5),rotation.right));
            auto up = clip(matrix, add(scale(rotation.forward,5),rotation.up));
            close(right[0]/right[3],.2,"Yaw/roll right direction is inverted");
            close(up[1]/up[3],.2,"Pitch/roll up direction is inverted");
        }
        const Vec3 point{5,10,300};
        TrackingFrame engine{{100,-200,500},{0,1,0},{0,0,1},{-1,0,0},100,{-80,140,-400}};
        EyePose tracked{{.03f,.12f,-.2f},{0,0,0,1}};
        const auto translated = eye_constants(engine,tracked,fov,1,10000);
        const auto world_point = add(point,engine.pretranslation);
        engine.pretranslation = {0,0,0};
        const auto unshifted = eye_constants(engine,tracked,fov,1,10000);
        const auto a = clip(translated,world_point), b = clip(unshifted,point);
        for (UINT axis = 0; axis < 4; ++axis)
            close(a[axis],b[axis],"Common UE3 pretranslation changed physical camera projection",.001);
        close(unshifted[16],120,"OpenXR forward translation has wrong engine-axis mapping");
        close(unshifted[17],-197,"IPD world-scale mapping is wrong");
        close(unshifted[18],512,"Head height world-scale mapping is wrong");
        for (UINT fault = 0; fault < 8; ++fault) {
            auto invalid_frame = frame;
            auto invalid_pose = identity;
            auto invalid_fov = fov;
            float near_plane=.1f, far_plane=100;
            if (fault==0) invalid_frame.units_per_meter=0;
            if (fault==1) invalid_frame.right={2,0,0};
            if (fault==2) invalid_frame.up=invalid_frame.right;
            if (fault==3) invalid_pose.orientation.w=2;
            if (fault==4) invalid_pose.position.x=std::numeric_limits<float>::quiet_NaN();
            if (fault==5) invalid_fov.right=invalid_fov.left;
            if (fault==6) near_plane=0;
            if (fault==7) far_plane=near_plane;
            refuses([&] { eye_constants(invalid_frame,invalid_pose,invalid_fov,near_plane,far_plane); });
        }
        std::cout<<"PASS: "<<checks<<" independent tracking/projection checks; asymmetric FOV, IPD, cardinal rotations, "
                     "engine basis, world scale, shared pretranslation and invalid inputs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
