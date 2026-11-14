#include "outlast_body_ik.h"
#include <fstream>
#include <iostream>
#include <filesystem>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
double distance(Vec3 a,Vec3 b) {
    const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
    return std::sqrt(x*x+y*y+z*z);
}
template<class T> void field_write(void* pointer,std::size_t offset,const T& value) {
    std::memcpy(static_cast<unsigned char*>(pointer)+offset,&value,sizeof(value));
}
Vec3 world(const NativeSkeleton& rig,INT bone) {
    const auto matrix=skeleton_bone_matrix(rig,bone);
    return {matrix[12],matrix[13],matrix[14]};
}
Vec3 coordinates(const TrackingFrame& frame,Vec3 point) {
    const auto d=add(point,scale(frame.origin,-1));
    return {dot(d,frame.right)/100,dot(d,frame.up)/100,dot(d,frame.backward)/100};
}
Vec3 oracle_rotate(Quaternion q,Vec3 v) {
    const double x=q.x,y=q.y,z=q.z,w=q.w;
    return {float((1-2*y*y-2*z*z)*v.x+(2*x*y-2*z*w)*v.y+(2*x*z+2*y*w)*v.z),
        float((2*x*y+2*z*w)*v.x+(1-2*x*x-2*z*z)*v.y+(2*y*z-2*x*w)*v.z),
        float((2*x*z-2*y*w)*v.x+(2*y*z+2*x*w)*v.y+(1-2*x*x-2*y*y)*v.z)};
}
void skinning_oracle(const NativeSkeleton& rig,const std::vector<BoneAtom>& source) {
    for (const auto segment:{std::array<INT,3>{10,31,32},std::array<INT,3>{11,29,30},
                            std::array<INT,3>{34,55,56},std::array<INT,3>{35,53,54},
                            std::array<INT,3>{61,57,58},std::array<INT,3>{62,65,66},
                            std::array<INT,3>{67,59,60},std::array<INT,3>{68,71,72}}) {
        const auto& before=source.at(segment[0]);
        const auto& after=rig.atoms.at(segment[0]);
        const Quaternion inverse{-before.rotation.x,-before.rotation.y,-before.rotation.z,before.rotation.w};
        for (unsigned i=1;i<3;++i) {
            const auto& base=source.at(segment[i]);
            const auto& skin=rig.atoms.at(segment[i]);
            const auto local=oracle_rotate(inverse,add(base.position,scale(before.position,-1)));
            require(distance(skin.position,add(after.position,oracle_rotate(after.rotation,local)))<.02,
                "Independent parallel skinning-chain position oracle failed");
            for (const Vec3 axis:{Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,1}}) {
                const auto expected=oracle_rotate(after.rotation,
                    oracle_rotate(inverse,oracle_rotate(base.rotation,axis)));
                require(distance(oracle_rotate(skin.rotation,axis),expected)<.001,
                    "Independent parallel skinning-chain roll oracle failed");
            }
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || argc==3,"Body proof requires its retained native pose fixture");
        const std::string fault=argc==3?argv[2]:"";
        require(fault.empty() || fault=="--fault-frozen" || fault=="--fault-stretch" ||
            fault=="--fault-camera" || fault=="--fault-skinning" ||
            fault=="--fault-grip-offset" || fault=="--fault-grip-axes" ||
            fault=="--body-follow","Unknown body proof control");
        std::ifstream file(argv[1],std::ios::binary);
        auto read=[&](void* data,std::size_t size) {
            if (!file.read(static_cast<char*>(data),std::streamsize(size)))
                throw std::runtime_error("Native body fixture truncated");
        };
        UINT64 magic=0;UINT count=0;std::array<INT,20> bones{};
        read(&magic,8);read(&count,4);read(bones.data(),sizeof(bones));
        require(magic==0x314b49524c4full && count>0 && count<=256,"Invalid native body fixture");
        std::vector<INT> parents(count);read(parents.data(),parents.size()*4);
        std::array<float,16> matrix{};read(matrix.data(),sizeof(matrix));
        std::vector<BoneAtom> original(count);read(original.data(),original.size()*sizeof(BoneAtom));
        require(file.peek()==std::char_traits<char>::eof(),"Native fixture has unexpected suffix");
        std::array<unsigned char,0x400> component{},asset{};
        std::vector<unsigned char> reference(count*0x50);
        auto current=original;
        for (UINT i=0;i<count;++i) field_write(reference.data(),i*0x50+0x40,parents[i]);
        field_write(asset.data(),0xc4,reference.data());field_write(asset.data(),0xcc,INT(count));
        field_write(component.data(),0x238,asset.data());field_write(component.data(),0xc0,matrix);
        field_write(component.data(),0x2d8,current.data());field_write(component.data(),0x2e0,INT(count));
        auto native=outlast_skeleton(component.data());
        {
            // Actual native-body-motion-012 evaluation 217; walking produces nonunit scale.
            auto walking=native;
            walking.world={0,-1,0,0,1,0,0,0,0,0,1,0,-9261.60645f,1.56230164f,-1.82378626f,1};
            walking.atoms[3]={{.703278661f,.0450209081f,.707471311f,-.053448081f},
                {-.376933217f,-.185711205f,89.9030151f},1.00000024f};
            const std::array<float,16> actual{
                -.0123014823f,.00508499146f,.999911726f,0,
                -.990233302f,-.138950497f,-.011475794f,0,
                .138879836f,-.990286648f,.00674444437f,0,
                -9261.79199f,1.93923485f,88.0792313f,1};
            const auto reconstructed=skeleton_bone_matrix(walking,3);
            for (unsigned i=0;i<16;++i)
                require(reconstructed[i]==actual[i],"Actual walking GetBoneMatrix scale arithmetic changed");
        }
        BodyIKTargets targets{};
        targets.tracking={{matrix[12],matrix[13],matrix[14]},{0,1,0},{0,0,1},{-1,0,0},100,{0,0,0}};
        targets.tracked={true,true};
        for (unsigned side=0;side<2;++side) {
            targets.hands[side].orientation={0,0,0,1};
            targets.hands[side].position=coordinates(targets.tracking,world(native,bones[side?13:9]));
        }
        OutlastBodyIK ik;
        const std::array<INT,16> skin_bones{31,32,29,30,55,56,53,54,57,58,65,66,59,60,71,72};
        for (unsigned fault_binding=0;fault_binding<5;++fault_binding) {
            auto invalid=skin_bones;
            if (fault_binding==0) invalid[0]=-1;
            if (fault_binding==1) invalid[0]=INT(count);
            if (fault_binding==2) invalid[0]=invalid[2];
            if (fault_binding==3) std::swap(invalid[0],invalid[1]);
            if (fault_binding==4) invalid[0]=bones[7];
            OutlastBodyIK rejected;
            bool threw=false;
            try {rejected.calibrate(native,bones,targets,invalid);}
            catch (const std::runtime_error&) {threw=true;}
            require(threw && !rejected.calibrated(),"Invalid named native skinning binding was accepted");
        }
        ik.calibrate(native,bones,targets,skin_bones);
        {
            auto neutral=native;
            ik.apply(neutral,targets);
            for (const float yaw:{-2.35619449f,-1.04719755f,1.04719755f,2.35619449f}) {
                auto turning=native;
                auto request=targets;request.body_yaw_radians=yaw;
                const auto result=ik.apply(turning,request);
                require(distance(world(turning,bones[5]),world(neutral,bones[5]))<.02,
                    "Following torso moved the tracked head position");
                for (unsigned side=0;side<2;++side) {
                    require(result.hand_target_error[side]<.1f,
                        "Following torso detached a controller");
                }
                const auto up=bone_unit(bone_world_to_component_vector(native,targets.tracking.up));
                const auto reference=add(neutral.atoms[bones[1]].position,
                    scale(neutral.atoms[bones[5]].position,-1));
                const auto axis=scale(up,std::sin(-yaw*.5f));
                const auto expected=oracle_rotate({axis.x,axis.y,axis.z,std::cos(yaw*.5f)},reference);
                const auto actual=add(turning.atoms[bones[1]].position,
                    scale(turning.atoms[bones[5]].position,-1));
                require(distance(actual,expected)<.02,"Body yaw did not follow mapped headset handedness");
            }
        }
        if (fault=="--body-follow") {
            std::cout<<"PASS: actual_native_73_bone_body_follow; bounded_deadzone_separate_from_view; "
                "mapped_handedness; preserved_head_position; maintained_controller_targets; yaw_both_directions\n";
            return 0;
        }
        const float standing_head=original[bones[5]].position.z;
        for (unsigned frame=0;frame<120;++frame) {
            current=original;
            auto rig=outlast_skeleton(component.data());
            targets.head.position.y=-.6f*float(frame%30)/29;
            const float angle=.5f*std::sin(float(frame)*.13f);
            std::array<Vec3,2> expected_hands{};
            for (unsigned side=0;side<2;++side) {
                auto target=world(native,bones[side?13:9]);
                target.z+=20;target.x+=8*std::sin(float(frame)*.1f+(side?1.f:0.f));
                expected_hands[side]=target;
                targets.hands[side].position=coordinates(targets.tracking,target);
                targets.hands[side].orientation={0,std::sin(angle*.5f),0,std::cos(angle*.5f)};
            }
            const auto result=ik.apply(rig,targets);
            if (fault=="--fault-frozen") rig.atoms=original;
            if (fault=="--fault-stretch") rig.atoms[bones[8]].position.x+=5;
            if (fault=="--fault-camera") rig.atoms[2].position.z+=1;
            if (fault=="--fault-skinning")
                for (INT bone:{29,30,31,32,53,54,55,56,57,58,59,60,65,66,71,72})
                    rig.atoms[bone]=original[bone];
            require(result.solved_hands==2,"Both controller arms not solved");
            require(std::abs(rig.atoms[bones[5]].position.z-standing_head-targets.head.position.y*100)<.02,
                "Physical crouch did not lower the actual body head by tracked height");
            for (const auto chain:{std::array<INT,3>{7,8,9},std::array<INT,3>{11,12,13},
                                  std::array<INT,3>{14,15,16},std::array<INT,3>{17,18,19}})
                for (unsigned segment=0;segment<2;++segment) {
                    const auto a=bones[chain[segment]],b=bones[chain[segment+1]];
                    require(std::abs(distance(rig.atoms[a].position,rig.atoms[b].position)-
                        distance(original[a].position,original[b].position))<.015,
                        "Native IK stretched or collapsed a limb");
                }
            for (unsigned side=0;side<2;++side) {
                require(distance(world(rig,bones[side?13:9]),expected_hands[side])<.03,
                    "Independent controller-space hand oracle failed");
                require(distance(rig.atoms[bones[side?13:9]].position,result.hands[side])<.01,
                    "Native hand did not reach bounded controller target");
                require(distance(rig.atoms[bones[side?19:16]].position,original[bones[side?19:16]].position)<.02,
                    "Physical crouch moved the planted feet");
                const auto before=skeleton_bone_matrix(native,bones[side?13:9]);
                const auto after=skeleton_bone_matrix(rig,bones[side?13:9]);
                const Vec3 basis{before[0],before[1],before[2]};
                const auto& mapping=targets.tracking;
                const auto relative=oracle_rotate(targets.hands[side].orientation,
                    {dot(basis,mapping.right),dot(basis,mapping.up),dot(basis,mapping.backward)});
                const auto expected=add(add(scale(mapping.right,relative.x),scale(mapping.up,relative.y)),
                    scale(mapping.backward,relative.z));
                require(distance({after[0],after[1],after[2]},expected)<.001,
                    "Independent reflected-basis wrist orientation oracle failed");
            }
            for (unsigned index:{0u,1u,2u})
                require(std::memcmp(&rig.atoms[index],&original[index],sizeof(BoneAtom))==0,
                    "Body IK changed the native camera/root ancestors");
            skinning_oracle(rig,original);
            for (const auto& atom:rig.atoms) {
                const double norm=double(atom.rotation.x)*atom.rotation.x+double(atom.rotation.y)*atom.rotation.y+
                    double(atom.rotation.z)*atom.rotation.z+double(atom.rotation.w)*atom.rotation.w;
                require(std::abs(norm-1)<.001 && std::isfinite(atom.scale),"Invalid native bone rotation/scale");
            }
            std::memcpy(current.data(),rig.atoms.data(),current.size()*sizeof(BoneAtom));
            ik.restore(component.data());
            require(std::memcmp(current.data(),original.data(),current.size()*sizeof(BoneAtom))==0,
                "Native cache restoration accumulated VR pose feedback");
        }
        for (const float height:{-.2f,-.05f,0.f,.05f,.2f}) {
            current=original;
            auto rig=outlast_skeleton(component.data());
            auto fitted=targets;
            fitted.head.position={0,0,0};
            fitted.standing_height_delta=height;
            for (unsigned side=0;side<2;++side) {
                auto goal=world(native,bones[side?13:9]);
                goal.z+=height*100;
                fitted.hands[side].position=coordinates(fitted.tracking,goal);
            }
            ik.apply(rig,fitted);
            require(std::abs(rig.atoms[bones[5]].position.z-standing_head-height*100)<.02,
                "Measured standing height did not fit the upper body");
            for (unsigned side=0;side<2;++side) {
                require(distance(rig.atoms[bones[side?19:16]].position,original[bones[side?19:16]].position)<.02,
                    "Standing-height fitting moved a planted foot");
                auto expected=world(native,bones[side?13:9]);
                expected.z+=height*100;
                require(distance(world(rig,bones[side?13:9]),expected)<.03,
                    "Standing-height fitting missed the translated hand");
            }
            skinning_oracle(rig,original);
        }
        for (const float angle:{-1.f,-.5f,.5f,1.f}) {
            current=original;
            auto rig=outlast_skeleton(component.data());
            bone_rotate(rig,bones[2],{0,0,std::sin(angle*.5f),std::cos(angle*.5f)});
            auto source=original;
            for (const INT helper:{57,58,59,60,65,66,71,72})
                source[helper]=rig.atoms[helper];
            auto stable=targets;
            stable.head.position={0,0,0};
            for (unsigned side=0;side<2;++side)
                stable.hands[side].position=coordinates(stable.tracking,world(native,bones[side?13:9]));
            const auto result=ik.apply(rig,stable);
            for (unsigned side=0;side<2;++side)
                require(result.hand_target_error[side]<.03 &&
                    distance(world(rig,bones[side?13:9]),world(native,bones[side?13:9]))<.03,
                    "Native upper-body animation displaced a tracked controller hand");
            skinning_oracle(rig,source);
        }
        auto corpus_path=std::filesystem::path(argv[1]);corpus_path.replace_extension(".poses.bin");
        std::ifstream corpus(corpus_path,std::ios::binary);
        UINT64 corpus_magic=0;UINT poses=0,pose_bones=0;
        auto read_pose=[&](void* data,std::size_t bytes) {
            if (!corpus.read(static_cast<char*>(data),std::streamsize(bytes)))
                throw std::runtime_error("Actual native crouch corpus missing or truncated");
        };
        read_pose(&corpus_magic,8);read_pose(&poses,4);read_pose(&pose_bones,4);
        require(corpus_magic==0x3153504f4c4full && poses>=8 && poses<=64 && pose_bones==count,
            "Invalid actual native crouch pose corpus");
        for (UINT pose=0;pose<poses;++pose) {
            std::vector<BoneAtom> source(count);read_pose(source.data(),source.size()*sizeof(BoneAtom));
            for (unsigned step=0;step<20;++step) {
                current=source;
                auto rig=outlast_skeleton(component.data());
                targets.compensate_native_crouch=true;
                targets.stance_transition=true;
                targets.head.position.y=-.6f*float(step)/19;
                for (unsigned side=0;side<2;++side) {
                    auto goal=world(native,bones[side?13:9]);goal.z+=20+targets.head.position.y*100;
                    targets.hands[side].position=coordinates(targets.tracking,goal);
                }
                ik.apply(rig,targets);
                skinning_oracle(rig,original);
                require(std::abs(rig.atoms[bones[5]].position.z-standing_head-targets.head.position.y*100)<.02,
                    "Native crouch corpus doubled tracked body height");
                for (unsigned side=0;side<2;++side)
                    require(distance(rig.atoms[bones[side?19:16]].position,
                        original[bones[side?19:16]].position)<.02,
                        "Native crouch corpus lifted a planted foot");
                for (unsigned index:{0u,1u,2u})
                    require(std::memcmp(&rig.atoms[index],&source[index],sizeof(BoneAtom))==0,
                        "Native crouch corpus changed a camera ancestor");
            }
        }
        require(corpus.peek()==std::char_traits<char>::eof(),"Native crouch corpus has unexpected suffix");
        {
            auto baseline=targets;
            baseline.compensate_native_crouch=false;baseline.native_crouched=false;
            baseline.stance_transition=false;baseline.head.position={};baseline.tracked={true,true};
            OutlastBodyIK neutral,tilted;
            neutral.calibrate(native,bones,baseline,skin_bones);
            auto initially_tilted=baseline;
            initially_tilted.hands[0].orientation={0,.70710678f,0,.70710678f};
            initially_tilted.hands[1].orientation={.70710678f,0,0,.70710678f};
            tilted.calibrate(native,bones,initially_tilted,skin_bones);
            for (unsigned frame=0;frame<120;++frame) {
                auto requested=baseline;
                const float angle=float(frame)*.0523598776f;
                for (unsigned side=0;side<2;++side) {
                    const auto shoulder=world(native,bones[side?11:7]);
                    const Vec3 delta{float(30+20*std::sin(angle)),side?-25.f:25.f,
                        float(-45+15*std::cos(angle))};
                    const auto goal=add(shoulder,delta);
                    requested.hands[side].position=coordinates(requested.tracking,goal);
                    requested.hands[side].orientation={0,std::sin(angle*.5f),0,std::cos(angle*.5f)};
                }
                current=original;auto first=outlast_skeleton(component.data());
                auto second=first;
                const auto result=neutral.apply(first,requested);
                tilted.apply(second,requested);
                for (unsigned side=0;side<2;++side) {
                    const auto goal=add(requested.tracking.origin,
                        scale(map_tracking(requested.tracking,requested.hands[side].position),100));
                    const auto hand=bones[side?13:9];
                    require(distance(world(first,hand),goal)<.03,
                        "Unclamped physical controller wrist alignment failed");
                    require(result.hand_target_error[side]<.03 &&
                        result.shoulder_adjustment[side]<=35.001f,
                        "Tracked-hand reach telemetry or bounded shoulder adaptation failed");
                    require(distance(first.atoms[hand].position,second.atoms[hand].position)<.001,
                        "Initial controller orientation changed hand position");
                    for (Vec3 axis:{Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,1}})
                        require(distance(oracle_rotate(first.atoms[hand].rotation,axis),
                            oracle_rotate(second.atoms[hand].rotation,axis))<.001,
                            "Initial controller grip contaminated native wrist calibration");
                }
                skinning_oracle(first,original);
            }
        }
        targets.compensate_native_crouch=false;targets.stance_transition=false;
        targets.head.position={};targets.tracked[0]=false;current=original;
        auto lost=outlast_skeleton(component.data());ik.apply(lost,targets);
        require(distance(lost.atoms[bones[9]].position,original[bones[9]].position)<.001,
            "Lost controller tracking reused the left hand pose");
        targets.tracked[0]=true;targets.hands[0].position={100,100,100};
        auto unreachable=outlast_skeleton(component.data());const auto bounded=ik.apply(unreachable,targets);
        const auto a=bones[7],b=bones[8],c=bones[9];
        require(distance(unreachable.atoms[a].position,unreachable.atoms[c].position)<=
            distance(original[a].position,original[b].position)+distance(original[b].position,original[c].position)+.01,
            "Unreachable controller target stretched the native arm");
        require(bounded.shoulder_adjustment[0]<=35.001f && bounded.hand_target_error[0]>100,
            "Pathological tracking target escaped the explicit reach limit");
        {
            auto path=std::filesystem::path(argv[1]);path.replace_extension(".reference.bin");
            std::ifstream source(path,std::ios::binary);
            require(bool(source.read(reinterpret_cast<char*>(reference.data()),std::streamsize(reference.size()))) &&
                source.peek()==std::char_traits<char>::eof(),"Immutable native reference fixture missing or truncated");
            current=original;auto reference_rig=outlast_skeleton(component.data());
            auto grip_targets=targets;
            grip_targets.grip_anchored=true;grip_targets.tracked={true,true};
            grip_targets.head={{0,0,0},{0,0,0,1}};
            OutlastBodyIK first,animated;
            first.calibrate(reference_rig,bones,grip_targets,skin_bones);
            auto animated_rig=reference_rig;
            bone_rotate(animated_rig,bones[7],{.3f,0,0,std::sqrt(.91f)});
            bone_rotate(animated_rig,bones[11],{0,.4f,0,std::sqrt(.84f)});
            animated.calibrate(animated_rig,bones,grip_targets,skin_bones);
            for (unsigned side=0;side<2;++side) {
                const auto& anchor=first.grip_anchor(side);
                auto opposite=grip_targets.tracking;
                opposite.right=skeleton_vector(reference_rig.world,anchor.x);
                opposite.backward=skeleton_vector(reference_rig.world,scale(anchor.z,-1));
                opposite.up=cross(opposite.right,opposite.backward);
                const auto rotation=outlast_grip_wrist_rotation(reference_rig,opposite,{0,0,0,1},anchor);
                require(distance(oracle_rotate(rotation,anchor.x),anchor.x)<.001 &&
                    distance(oracle_rotate(rotation,anchor.z),scale(anchor.z,-1))<.001,
                    "A half-turn around the palm normal changed the palm-facing axis");
            }
            for (unsigned frame=0;frame<360;++frame) {
                const float angle=float(frame)*.0523598776f;
                const Vec3 axis=frame<120?Vec3{1,0,0}:frame<240?Vec3{0,1,0}:Vec3{0,0,1};
                for (unsigned side=0;side<2;++side) {
                    auto goal=world(reference_rig,bones[side?13:9]);goal.z+=10;
                    grip_targets.hands[side].position=coordinates(grip_targets.tracking,goal);
                    grip_targets.hands[side].orientation={axis.x*std::sin(angle*.5f),
                        axis.y*std::sin(angle*.5f),axis.z*std::sin(angle*.5f),std::cos(angle*.5f)};
                }
                auto a=reference_rig,b=reference_rig;
                const auto result=first.apply(a,grip_targets);
                animated.apply(b,grip_targets);
                for (unsigned side=0;side<2;++side) {
                    const auto hand=bones[side?13:9];
                    Vec3 center{};
                    for (const INT finger:side?std::array<INT,4>{40,43,46,49}:std::array<INT,4>{16,19,22,25})
                        center=add(center,scale(skeleton_field<Vec3>(reference.data(),std::size_t(finger)*0x50+0x20),.125f));
                    const auto& original_wrist=a.atoms[hand];
                    const auto goal=add(grip_targets.tracking.origin,scale(map_tracking(grip_targets.tracking,
                        grip_targets.hands[side].position),100));
                    if (fault=="--fault-grip-offset") a.atoms[hand].position=bone_world_to_component(a,goal);
                    if (fault=="--fault-grip-axes") {
                        a.atoms[hand].rotation=original[hand].rotation;
                        a.atoms[hand].position=add(bone_world_to_component(a,goal),scale(
                            oracle_rotate(a.atoms[hand].rotation,scale(center,a.atoms[hand].scale)),-1));
                    }
                    const auto palm=add(world(a,hand),skeleton_vector(a.world,
                        oracle_rotate(a.atoms[hand].rotation,scale(center,a.atoms[hand].scale))));
                    require(distance(palm,goal)<.03 && result.hand_target_error[side]<.03,
                        "Physical palm centroid, not the wrist/fingertips, must meet the XR grip");
                    const auto index=skeleton_field<Vec3>(reference.data(),std::size_t(side?40:16)*0x50+0x20);
                    const auto little=skeleton_field<Vec3>(reference.data(),std::size_t(side?49:25)*0x50+0x20);
                    const auto z=bone_unit(add(little,scale(index,-1)));
                    const auto x=bone_unit(cross(center,z));
                    for (const auto pair:{std::pair<Vec3,Vec3>{x,{1,0,0}},{z,{0,0,1}}}) {
                        const auto actual=bone_unit(skeleton_vector(a.world,
                            oracle_rotate(original_wrist.rotation,pair.first)));
                        const auto expected=bone_unit(map_tracking(grip_targets.tracking,
                            oracle_rotate(grip_targets.hands[side].orientation,pair.second)));
                        require(distance(actual,expected)<.001,
                            "Native palm normal or little-to-thumb tube axis disagrees with OpenXR grip");
                    }
                    require(distance(a.atoms[hand].position,b.atoms[hand].position)<.001,
                        "Animated calibration pose contaminated the immutable grip offset");
                }
            }
        }
        std::cout<<"PASS: native_body_checks="<<checks<<"; native_73_bone_fixture; native_crouch_corpus="<<poses
            <<"; 120_pose_squats; "
            <<"both_arms_four_limbs; planted_feet; fixed_segment_lengths; root_camera_ancestors_preserved; "
            <<"parallel_skinning_chains; cache_feedback_removed; lost_hand_native_pose; unreachable_target_bounded; "
            <<"measured_standing_torso_fit; planted_height_fit_feet; animation_independent_tracked_torso; "
            <<"immutable_reference_palm; grip_centroid_not_wrist; grip_axes_360_rotations\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
