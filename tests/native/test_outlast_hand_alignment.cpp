#include <outlast_body_ik.h>
#ifndef OLVR_BASELINE_BENCH
#include "outlast_hand_alignment.h"
#include "outlast_native_view.h"
#endif
#include <chrono>
#include <fstream>
#include <iostream>
#include <filesystem>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool condition,const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class T> void field_write(void* pointer,std::size_t offset,const T& value) {
    std::memcpy(static_cast<unsigned char*>(pointer)+offset,&value,sizeof(value));
}
#ifndef OLVR_BASELINE_BENCH
template<class F> void rejects(F action) {
    bool rejected=false;
    try {action();} catch (const std::exception&) {rejected=true;}
    require(rejected,"Invalid hand fit or topology was accepted");
}
Vec3 world_vector(const NativeSkeleton& rig,Vec3 v) {
    return {v.x*rig.world[0]+v.y*rig.world[4]+v.z*rig.world[8],
        v.x*rig.world[1]+v.y*rig.world[5]+v.z*rig.world[9],
        v.x*rig.world[2]+v.y*rig.world[6]+v.z*rig.world[10]};
}
#endif
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || (argc==3 && std::string(argv[2])=="--benchmark"),"Native rig fixture required");
        std::ifstream input(argv[1],std::ios::binary);
        auto read=[&](void* target,std::size_t size) {
            require(bool(input.read(static_cast<char*>(target),std::streamsize(size))),"Truncated rig fixture");
        };
        UINT64 magic=0;UINT count=0;std::array<INT,20> bones{};
        read(&magic,8);read(&count,4);read(bones.data(),sizeof(bones));
        require(magic==0x314b49524c4full && count==73,"Expected actual 73-bone native hero fixture");
        std::vector<INT> parents(count);read(parents.data(),count*sizeof(INT));
        std::array<float,16> transform{};read(transform.data(),sizeof(transform));
        std::vector<BoneAtom> atoms(count);read(atoms.data(),count*sizeof(BoneAtom));
        require(input.peek()==std::char_traits<char>::eof(),"Unexpected native rig fixture suffix");
        std::array<unsigned char,0x400> component{},asset{};
        std::vector<unsigned char> reference(count*0x50);
        auto reference_path=std::filesystem::path(argv[1]);reference_path.replace_extension(".reference.bin");
        std::ifstream reference_input(reference_path,std::ios::binary);
        require(bool(reference_input.read(reinterpret_cast<char*>(reference.data()),std::streamsize(reference.size()))) &&
            reference_input.peek()==std::char_traits<char>::eof(),"Actual immutable native reference fixture required");
        field_write(asset.data(),0xc4,reference.data());field_write(asset.data(),0xcc,INT(count));
        field_write(component.data(),0x238,asset.data());field_write(component.data(),0xc0,transform);
        field_write(component.data(),0x2d8,atoms.data());field_write(component.data(),0x2e0,INT(count));
        auto rig=outlast_skeleton(component.data());
        require(rig.parents==parents,"Native current and immutable hierarchy disagree");
        BodyIKTargets targets{};
        targets.tracking={{transform[12],transform[13],transform[14]},{0,1,0},{0,0,1},{-1,0,0},100,{0,0,0}};
        targets.tracked={true,true};targets.grip_anchored=true;
        const std::array<INT,16> skin{31,32,29,30,55,56,53,54,57,58,65,66,59,60,71,72};
        OutlastBodyIK ik;ik.calibrate(rig,bones,targets,skin);
        if (argc==3) {
            constexpr unsigned frames=50000;
            UINT64 hash=14695981039346656037ull;
            auto solved=rig;
            const auto start=std::chrono::steady_clock::now();
            for (unsigned frame=0;frame<frames;++frame) {
                solved.atoms=atoms;
                const float phase=float(frame%720)*.01745329252f;
                targets.head.position={.03f*std::sin(phase),-.3f*(1+std::sin(phase)),.02f*std::cos(phase)};
                targets.compensate_native_crouch=(frame%200)<100;
                for (unsigned side=0;side<2;++side) {
                    targets.hands[side]={{side?.25f:-.25f,-.35f+.05f*std::sin(phase),-.4f+.05f*std::cos(phase)},
                        {0,std::sin(phase*.5f),0,std::cos(phase*.5f)}};
                }
                ik.apply(solved,targets);
                if (frame%31==0 || frame+1==frames) {
                    const auto* bytes=reinterpret_cast<const unsigned char*>(solved.atoms.data());
                    for (std::size_t i=0;i<solved.atoms.size()*sizeof(BoneAtom);++i) {
                        hash^=bytes[i];hash*=1099511628211ull;
                    }
                }
            }
            const auto nanoseconds=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"{\"frames\":"<<frames<<",\"ns_per_pose\":"<<nanoseconds/frames
                <<",\"pose_hash\":\""<<hash<<"\"}\n";
            return 0;
        }
#ifndef OLVR_BASELINE_BENCH
        BoneHierarchy hierarchy(parents);
        for (INT root=0;root<INT(count);++root)
            for (INT bone=0;bone<INT(count);++bone)
                require(bone_descendant(rig,bone,root)==bone_descendant(rig,bone,root,&hierarchy),
                    "Cached hierarchy changed actual native ancestry");
        for (unsigned frame=0;frame<120;++frame) {
            auto first=rig,second=rig;
            const float angle=float(frame)*.037f;
            const auto rotation=bone_normalize({std::sin(angle),.2f,.1f,std::cos(angle)});
            for (INT root:{0,3,12,36,69}) {
                bone_translate(first,root,{.1f,-.2f,.3f});
                bone_translate(second,root,{.1f,-.2f,.3f},&hierarchy);
                bone_rotate(first,root,rotation);
                bone_rotate(second,root,rotation,&hierarchy);
            }
            for (const auto chain:{std::array<INT,3>{10,11,12},std::array<INT,3>{34,35,36}}) {
                const auto goal=add(first.atoms[chain[2]].position,{2,-1,3});
                bone_solve_limb(first,chain[0],chain[1],chain[2],goal,{0,0,-1},rotation);
                bone_solve_limb(second,chain[0],chain[1],chain[2],goal,{0,0,-1},rotation,&hierarchy);
            }
            require(std::memcmp(first.atoms.data(),second.atoms.data(),count*sizeof(BoneAtom))==0,
                "Cached subtree transforms changed native pose bytes");
        }
        {
            auto requested=targets;
            requested.controller_fingers=true;
            for (unsigned side=0;side<2;++side)
                requested.hands[side]={{side?.25f:-.25f,-.35f,-.4f},{0,0,0,1}};
            requested.finger_trigger={.4f,.7f};requested.finger_squeeze={.6f,.8f};
            for (unsigned side=0;side<2;++side) {
                const auto wrist=bones[side?13:9];
                for (const INT root:{bones[side?11:7],wrist,wrist+4,wrist+5})
                    for (const float visible_scale:{0.f,.25f,1.f}) {
                        auto pose=rig;
                        for (INT bone=0;bone<INT(count);++bone)
                            if (hierarchy.descendant(bone,root)) pose.atoms[std::size_t(bone)].scale=visible_scale;
                        const auto native=pose.atoms;
                        ik.apply(pose,requested);
                        for (INT bone=0;bone<INT(count);++bone) {
                            require(pose.atoms[std::size_t(bone)].scale==native[std::size_t(bone)].scale,
                                "Actual-rig pickup changed a native visibility scale");
                            for (const float value:skeleton_bone_matrix(pose,bone))
                                require(std::isfinite(value),"Actual-rig pickup produced a nonfinite skin matrix");
                        }
                        std::memcpy(atoms.data(),pose.atoms.data(),count*sizeof(BoneAtom));
                        require(ik.restore(component.data()) &&
                            std::memcmp(atoms.data(),native.data(),count*sizeof(BoneAtom))==0,
                            "Pickup restoration overwrote native hiding or retained a collapsed tracked pose");
                    }
            }
        }
        auto changed=rig;changed.parents[12]=0;
        require(!ik.owns(changed),"Changed native topology retained stale cached rig ownership");
        rejects([&]{ik.apply(changed,targets);});
        changed=rig;changed.asset=component.data();require(!ik.owns(changed),"Changed asset retained cached rig");
        changed=rig;changed.component=asset.data();require(!ik.owns(changed),"Changed component retained cached rig");
        for (auto bad:{std::vector<INT>{},std::vector<INT>{-1,1},std::vector<INT>{-1,-1},
                       std::vector<INT>(257,-1)}) rejects([&]{BoneHierarchy invalid(bad);});
        auto topology=parents;topology[12]=0;
        BoneHierarchy rebuilt(topology);
        require(!hierarchy.matches(topology) && rebuilt.matches(topology) &&
            !rebuilt.descendant(12,11),"Changed native topology was not rebuilt correctly");
        for (const auto text:{L"",L"abc",L"2cm",L"nan",L"inf",L"6.01",L"-6.01"})
            rejects([&]{outlast_hand_offset_cm(text);});
        rejects([&]{outlast_hand_offset_cm(nullptr);});
        require(outlast_hand_offset_cm(L"0")==0 && outlast_hand_offset_cm(L"2")==.02f &&
            outlast_hand_offset_cm(L"-6")==-.06f && outlast_hand_offset_cm(L"6")==.06f,
            "Explicit controller fit units or limits changed");
        for (unsigned frame=0;frame<360;++frame) {
            const float angle=float(frame)*.01745329252f;
            const auto rotation=bone_normalize({std::sin(angle*.5f),.3f*std::sin(angle),.2f*std::cos(angle),std::cos(angle*.5f)});
            const EyePose raw{{.25f,-.4f,-.5f},rotation};
            const auto unchanged=outlast_aligned_hand(raw,0);
            require(std::memcmp(&raw,&unchanged,sizeof(raw))==0,
                "Zero fit did not exactly preserve the previous grip pose");
            const auto fitted=outlast_aligned_hand(raw);
            const auto delta=add(fitted.position,scale(raw.position,-1));
            require(std::abs(std::sqrt(dot(delta,delta))-.02f)<1e-6f &&
                std::memcmp(&fitted.orientation,&raw.orientation,sizeof(rotation))==0,
                "Controller fit changed hand rotation or correction distance");
            OutlastRecenter recenter;
            const EyePose eye{{1,1.7f,-2},{0,std::sin(angle*.5f),0,std::cos(angle*.5f)}};
            recenter.recenter({eye,eye});
            const auto fit_after_recenter=outlast_aligned_hand(recenter.relative(raw));
            const auto recentered_fit=recenter.relative(fitted);
            const auto difference=add(fit_after_recenter.position,scale(recentered_fit.position,-1));
            require(std::sqrt(dot(difference,difference))<1e-6f,
                "Controller-local fit changed under native recenter translation/yaw");
            for (unsigned side=0;side<2;++side) {
                const auto anchor=outlast_grip_anchor(rig,side?36:12);
                const auto wrist=outlast_grip_wrist_rotation(rig,targets.tracking,rotation,anchor);
                const auto anatomical_wristward=world_vector(rig,rotate(wrist,scale(cross(anchor.z,anchor.x),-1)));
                require(dot(bone_unit(map_tracking(targets.tracking,delta)),bone_unit(anatomical_wristward))>.9999f,
                    "Hand correction moved toward the fingers instead of the actual native wrist");
            }
        }
        rejects([&]{outlast_aligned_hand({{0,0,0},{0,0,0,0}});});
        rejects([&]{outlast_aligned_hand({{0,0,0},{0,0,0,1}},.061f);});
        std::cout<<"Native hand fit/topology: "<<checks<<" checks passed\n";
#else
        throw std::runtime_error("Frozen baseline executable is benchmark-only");
#endif
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
