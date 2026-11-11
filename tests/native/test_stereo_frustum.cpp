#include "stereo_frustum.h"
#include <iostream>

using namespace olvr;

static std::size_t checks=0;
static void require(bool value,const char* text) {
    ++checks;
    if (!value) throw std::runtime_error(text);
}

// Clip inequalities evaluated at all eight world-space box corners. This does
// not use the implementation's normalized plane extraction or support radii.
static std::pair<bool,bool> reference(const StereoViewConstants& views,Vec3 center,Vec3 extent) {
    bool visible=false,inside=false;
    for (const auto& eye:views) {
        std::array<unsigned,6> outside{};
        bool all_inside=true;
        for (unsigned corner=0;corner<8;++corner) {
            const double point[]{double(center.x)+(corner&1?extent.x:-extent.x)+eye[20],
                double(center.y)+(corner&2?extent.y:-extent.y)+eye[21],
                double(center.z)+(corner&4?extent.z:-extent.z)+eye[22],1};
            double clip[4]{};
            for (unsigned row=0;row<4;++row)
                for (unsigned column=0;column<4;++column) clip[row]+=eye[row*4+column]*point[column];
            const double distances[]{clip[0]+clip[3],clip[3]-clip[0],
                clip[1]+clip[3],clip[3]-clip[1],clip[2],clip[3]-clip[2]};
            for (unsigned plane=0;plane<6;++plane)
                if (distances[plane]<0) {++outside[plane];all_inside=false;}
        }
        bool intersects=true;
        for (const auto count:outside) if (count==8) intersects=false;
        visible |= intersects;inside |= all_inside;
    }
    return {visible,inside};
}

int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-mono";
        const bool body_fault=argc==2 && std::string(argv[1])=="--fault-body-camera";
        if (argc!=1 && !fault && !body_fault) throw std::invalid_argument("Unknown frustum test argument");
        const TrackingFrame simple{{0,0,0},{1,0,0},{0,1,0},{0,0,-1},1,{0,0,0}};
        const EyeFov symmetric{-.785398163f,.785398163f,.785398163f,-.785398163f};
        StereoViewConstants boundary{
            eye_constants(simple,{{-.032f,0,0},{0,0,0,1}},symmetric,.1f,100),
            eye_constants(simple,{{.032f,0,0},{0,0,0,1}},symmetric,.1f,100)};
        auto tested=boundary;
        if (fault) tested[1]=tested[0];
        StereoFrustum fringe(tested);
        require(fringe.sphere({1.01f,0,1},0),"Right-eye-only sphere was culled");
        require(fringe.sphere({-1.01f,0,1},0),"Left-eye-only sphere was culled");
        require(!fringe.sphere({1.1f,0,1},0),"Outside sphere was not culled");
        require(fringe.sphere({1.1f,0,1},.1f),"Intersecting sphere was culled");
        bool inside=true;
        require(fringe.box({1.01f,0,1},{.002f,.002f,.002f},&inside) && inside,
            "Right-eye-only box was not fully inside the stereo union");
        require(!fringe.box({0,0,-5},{.1f,.1f,.1f},&inside) && !inside,
            "Behind-camera box survived stereo culling");
        for (const float yaw:{-1.57079632679f,1.57079632679f,3.14159265359f}) {
            const Quaternion head{0,std::sin(yaw/2),0,std::cos(yaw/2)};
            StereoViewConstants turned{};
            for (unsigned eye=0;eye<2;++eye) {
                const float ipd=eye?.032f:-.032f;
                turned[eye]=eye_constants(simple,
                    {{ipd*std::cos(yaw),0,-ipd*std::sin(yaw)},head},symmetric,.1f,100);
            }
            const StereoFrustum head_frustum(body_fault?boundary:turned);
            const Vec3 ahead{-10*std::sin(yaw),0,10*std::cos(yaw)};
            require(!fringe.sphere(ahead,.1f),"Head-turn fixture was already visible to the unchanged body camera");
            require(head_frustum.sphere(ahead,.1f),
                "Head-facing sphere disappeared with the body camera unchanged");
            require(head_frustum.box(ahead,{.1f,.1f,.1f},&inside) && inside,
                "Head-facing box disappeared with the body camera unchanged");
            require(!head_frustum.sphere(scale(ahead,-1),.1f) &&
                !head_frustum.box(scale(ahead,-1),{.1f,.1f,.1f}),
                "Head-turn culling retained objects behind the tracked eyes");
            for (int x=-12;x<=12;++x) for (int z=-12;z<=12;++z) {
                const Vec3 point{float(x)+.37f,.23f,float(z)+.41f},extent{.1f,.2f,.15f};
                const auto expected=reference(turned,point,extent);
                bool full=false;
                const auto actual=head_frustum.box(point,extent,&full);
                const auto expanded=reference(turned,point,{.12f,.22f,.17f});
                require(!expected.first || actual,"A head-turn clip-visible box was culled");
                require(expanded.first || !actual,"A head-turn outside box was retained");
                require(!full || expected.second,"Head-turn full-inside disagrees with clip corners");
            }
        }
        for (unsigned orientation=0;orientation<5;++orientation) {
            const TrackingFrame frame{{30,-5,7},{0,1,0},{0,0,1},{-1,0,0},100,{-20,2,-3}};
            const float angle=float(orientation)*.37f;
            const Quaternion q{0,std::sin(angle*.5f),0,std::cos(angle*.5f)};
            StereoViewConstants views{
                eye_constants(frame,{{-.032f,.12f,-.2f},q},{-.9f,.65f,.75f,-.7f},.5f,2000),
                eye_constants(frame,{{.032f,.12f,-.2f},q},{-.6f,1.f,.8f,-.65f},.5f,2000)};
            StereoFrustum frustum(views);
            for (int x=-8;x<=8;++x) for (int y=-8;y<=8;++y) for (int z=-8;z<=8;++z) {
                const Vec3 point{float(x)*40+17,float(y)*33+19,float(z)*41+23};
                for (const Vec3 extent:{Vec3{0,0,0},Vec3{.3f,1.7f,3.1f},Vec3{5,9,13}}) {
                    const auto expected=reference(views,point,extent);
                    bool actual_inside=false;
                    const auto actual=frustum.box(point,extent,&actual_inside);
                    const auto expanded=reference(views,point,
                        {extent.x+.02f,extent.y+.02f,extent.z+.02f});
                    require(!expected.first || actual,"Clip-visible box was incorrectly culled");
                    require(expanded.first || !actual,"Well-outside rotated box was not culled");
                    require(!actual_inside || expected.second,"Full-inside box claim disagrees with clip corners");
                    if (extent.x==0) require(!expected.first || frustum.sphere(point,0),
                        "Clip-visible point sphere was incorrectly culled");
                    if (!actual) require(!expected.first,"Cull decision disagrees with clip inequalities");
                }
            }
        }
        for (unsigned malformed=0;malformed<6;++malformed) {
            bool refused=false;
            try {
                if (malformed==0) fringe.sphere({0,0,0},-1);
                if (malformed==1) fringe.box({NAN,0,0},{1,1,1});
                if (malformed==2) {
                    auto invalid=boundary;invalid[0][0]=NAN;StereoFrustum reject(invalid);
                }
                if (malformed==3) {
                    StereoViewConstants invalid{};StereoFrustum reject(invalid);
                }
                if (malformed==4) fringe.sphere({NAN,0,0},1);
                if (malformed==5) fringe.sphere({0,0,0},INFINITY);
            } catch (const std::invalid_argument&) {refused=true;}
            require(refused,"Malformed stereo culling input was accepted");
        }
        std::cout<<"PASS: stereo-union frustum; independent_checks="<<checks
            <<"; eye-only boundaries, world/pretranslation, rotated asymmetric views, eight-corner clip oracle"
            <<"; unchanged body with -90/+90/180-degree head turns\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
