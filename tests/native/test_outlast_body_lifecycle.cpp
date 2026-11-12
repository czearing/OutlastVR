#include "outlast_body_ik.h"
#include <iostream>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool condition,const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class T> void write(void* object,std::size_t offset,const T& value) {
    std::memcpy(static_cast<unsigned char*>(object)+offset,&value,sizeof(value));
}
float distance(Vec3 a,Vec3 b) {
    const auto delta=add(a,scale(b,-1));
    return std::sqrt(dot(delta,delta));
}
bool equal(const BoneAtom& a,const BoneAtom& b) {
    return std::memcmp(&a,&b,sizeof(a))==0;
}
struct Fixture {
    std::array<unsigned char,0x400> component{},asset{};
    std::array<unsigned char,36*0x50> reference{};
    std::vector<BoneAtom> native,current;
    std::array<INT,20> bones{};
    std::array<INT,16> skin{};
    BodyIKTargets targets{};
    OutlastBodyIK ik;
    Fixture() {
        const std::array<INT,20> parents{0,0,1,2,3,4,4,6,7,8,4,10,11,12,1,14,15,1,17,18};
        const std::array<Vec3,20> positions{{
            {0,0,0},{0,0,95},{0,0,115},{0,0,130},{0,0,145},{0,0,170},
            {0,-12,145},{0,-20,145},{3,-45,140},{8,-65,135},
            {0,12,145},{0,20,145},{3,45,140},{8,65,135},
            {0,-10,90},{3,-10,45},{10,-10,0},
            {0,10,90},{3,10,45},{10,10,0}}};
        for (INT i=0;i<20;++i) {
            bones[i]=i;
            write(reference.data(),std::size_t(i)*0x50+0x40,parents[i]);
            native.push_back({{0,0,0,1},positions[i],1});
        }
        unsigned segment=0;
        for (const INT driver:{7,8,11,12,14,15,17,18}) {
            for (unsigned joint=0;joint<2;++joint) {
                const INT index=20+INT(segment*2+joint);
                skin[segment*2+joint]=index;
                write(reference.data(),std::size_t(index)*0x50+0x40,joint?index-1:driver);
                native.push_back({{0,0,0,1},add(positions[driver],{float(joint+1),0,0}),1});
            }
            ++segment;
        }
        current=native;
        write(asset.data(),0xc4,reference.data());
        write(asset.data(),0xcc,INT(native.size()));
        write(component.data(),0x238,asset.data());
        write(component.data(),0x2d8,current.data());
        write(component.data(),0x2e0,INT(current.size()));
        const std::array<float,16> world{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        write(component.data(),0xc0,world);
        targets.tracking={{0,0,170},{0,1,0},{0,0,1},{-1,0,0},100,{}};
        targets.tracked={true,true};
        for (unsigned side=0;side<2;++side) {
            const auto position=positions[side?13:9];
            targets.hands[side]={{position.y/100,(position.z-170)/100,-position.x/100},{0,0,0,1}};
        }
        ik.calibrate(outlast_skeleton(component.data()),bones,targets,skin);
    }
    NativeSkeleton solve() {
        auto rig=outlast_skeleton(component.data());
        const auto result=ik.apply(rig,targets);
        const unsigned expected=unsigned(targets.tracked[0])+unsigned(targets.tracked[1]);
        require(result.solved_hands==expected,"Tracking loss/reacquisition solved the wrong arms");
        for (unsigned side=0;side<2;++side)
            if (targets.tracked[side]) {
                const auto goal=add(targets.tracking.origin,scale(map_tracking(targets.tracking,
                    targets.hands[side].position),targets.tracking.units_per_meter));
                const auto matrix=skeleton_bone_matrix(rig,side?13:9);
                require(distance({matrix[12],matrix[13],matrix[14]},goal)<.03f,
                    "Tracked hand detached from its moving body/controller frame");
            }
        return rig;
    }
};
void shadow_lifecycle() {
    Fixture visible,shadow,npc;
    std::array<unsigned char,0x2d00> hero{};
    write(hero.data(),0x444,visible.component.data());
    write(hero.data(),0x2ce0,shadow.component.data());
    require(outlast_body_component(hero.data(),visible.component.data())==0,
        "Visible hero lost tracked-pose ownership");
    require(outlast_body_component(hero.data(),shadow.component.data())==1,
        "Native shadow mesh is excluded from the tracked pose");
    require(!outlast_body_component(hero.data(),npc.component.data()),
        "NPC skeleton was mistaken for a tracked hero mesh");
    for (unsigned frame=0;frame<48;++frame) {
        for (auto* fixture:{&visible,&shadow}) {
            fixture->current=fixture->native;
            fixture->targets.head.position={.03f+.02f*std::sin(float(frame)),0,0};
            fixture->targets.hands[0].position={-.42f,-.2f,-.3f};
            fixture->targets.hands[1].position={.42f,-.25f,-.3f};
            fixture->targets.tracked={frame%3!=0,frame%4!=0};
            const auto solved=fixture->solve();
            std::memcpy(fixture->current.data(),solved.atoms.data(),solved.atoms.size()*sizeof(BoneAtom));
        }
        for (unsigned bone=0;bone<visible.current.size();++bone)
            require(equal(visible.current[bone],shadow.current[bone]),
                "Visible and shadow bones diverged with identical native rigs and controller goals");
        require(shadow.ik.restore(shadow.component.data()),
            "Shadow pose could not restore its own native animation");
        require(std::memcmp(shadow.current.data(),shadow.native.data(),
            shadow.native.size()*sizeof(BoneAtom))==0,"Shadow restoration retained tracked feedback");
        require(visible.ik.restore(visible.component.data()),
            "Restoring the shadow consumed the visible mesh's pose cache");
        require(std::memcmp(npc.current.data(),npc.native.data(),
            npc.native.size()*sizeof(BoneAtom))==0,"Tracked shadow update changed an NPC");
    }
    write(shadow.component.data(),0x33c,visible.component.data());
    require(!outlast_body_component(hero.data(),shadow.component.data()),
        "Parent-animated shadow was treated as an independent local skeleton");
    write(shadow.component.data(),0x33c,static_cast<void*>(nullptr));
    write(shadow.component.data(),0x238,static_cast<void*>(nullptr));
    require(!outlast_body_component(hero.data(),shadow.component.data()),
        "Uninitialized shadow mesh was accepted for tracked IK");
}
void hand_orientation(bool visibility=false) {
    std::array<unsigned char,0x400> component{},asset{};
    std::array<unsigned char,33*0x50> reference{};
    std::vector<BoneAtom> atoms(33,BoneAtom{{0,0,0,1},{0,0,0},1});
    const std::array<Vec3,5> roots{{{2,0,-5},{9,0,-3},{10,0,-1},{9,0,1},{8,0,3}}};
    for (unsigned side=0;side<2;++side) {
        const INT wrist=side?17:1;
        write(reference.data(),std::size_t(wrist)*0x50+0x40,INT(0));
        for (unsigned finger=0;finger<5;++finger) for (unsigned joint=0;joint<3;++joint) {
            const INT index=wrist+INT(finger*3+joint)+1;
            auto position=joint?Vec3{3,0,0}:roots[finger];
            if (side) position.z=-position.z;
            write(reference.data(),std::size_t(index)*0x50+0x40,joint?index-1:wrist);
            write(reference.data(),std::size_t(index)*0x50+0x20,position);
            write(reference.data(),std::size_t(index)*0x50+0x10,Quaternion{0,0,0,1});
            atoms[std::size_t(index)].position=add(atoms[std::size_t(joint?index-1:wrist)].position,position);
        }
    }
    write(asset.data(),0xc4,reference.data());write(asset.data(),0xcc,INT(atoms.size()));
    write(component.data(),0x238,asset.data());
    write(component.data(),0x2d8,atoms.data());write(component.data(),0x2e0,INT(atoms.size()));
    const std::array<float,16> world{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    write(component.data(),0xc0,world);
    const auto source=outlast_skeleton(component.data());
    NativeControllerFingers fingers(source,1,17);
    if (visibility) {
        NativeCameraFingerGrip camera(source,1,17);
        for (unsigned side=0;side<2;++side) for (unsigned joint:{0u,1u,2u,4u,5u,6u}) {
            const INT wrist=side?17:1,root=wrist+INT(joint);
            for (float native_scale:{0.f,-1.f,.25f,1.e-30f,std::numeric_limits<float>::denorm_min()}) {
                auto native=source;
                for (std::size_t bone=0;bone<native.atoms.size();++bone)
                    if (bone_descendant(native,INT(bone),root)) native.atoms[bone].scale=native_scale;
                for (bool mirror:{false,true}) {
                    auto rig=native;
                    if (mirror) camera.apply(rig,native.atoms);
                    else fingers.apply(rig,native.atoms,side,.4f,.7f,false);
                    for (std::size_t bone=0;bone<rig.atoms.size();++bone) {
                        require(rig.atoms[bone].scale==native.atoms[bone].scale,
                            "Finger articulation changed native hidden/mirrored bone visibility");
                        require(finite(rig.atoms[bone].position),
                            "Hidden native finger produced a nonfinite position");
                        const auto matrix=skeleton_bone_matrix(rig,INT(bone));
                        for (const auto value:matrix)
                            require(std::isfinite(value),"Hidden native finger produced a nonfinite skin matrix");
                        if (rig.atoms[bone].scale==0)
                            for (unsigned axis=0;axis<3;++axis) for (unsigned row=0;row<3;++row)
                                require(matrix[axis*4+row]==0,"Articulation made a hidden native bone visible");
                    }
                }
            }
        }
        auto expected=source;
        fingers.apply(expected,source.atoms,0,.4f,.7f,false);
        for (unsigned frame=0;frame<24;++frame) {
            auto rig=source;
            if (frame%2)
                for (std::size_t bone=1;bone<17;++bone) rig.atoms[bone].scale=0;
            const auto native=rig.atoms;
            fingers.apply(rig,native,0,.4f,.7f,false);
            if (!(frame%2))
                require(std::memcmp(rig.atoms.data(),expected.atoms.data(),rig.atoms.size()*sizeof(BoneAtom))==0,
                    "Pickup visibility recovery retained a collapsed or altered hand");
        }
        for (bool mirror:{false,true}) {
            auto rig=source;
            rig.atoms[1].scale=.5f;
            if (mirror) camera.apply(rig,source.atoms);
            else fingers.apply(rig,source.atoms,0,0,0,false);
            for (std::size_t bone=1;bone<17;++bone)
                require(rig.atoms[bone].scale==.5f &&
                    distance(rig.atoms[bone].position,scale(source.atoms[bone].position,.5f))<.0001f,
                    "Finger visibility fix changed inherited positive-scale transforms");
        }
        auto invalid=source;
        invalid.atoms[1].scale=std::numeric_limits<float>::quiet_NaN();
        bool rejected=false;
        try {fingers.apply(invalid,invalid.atoms,0,0,0,false);}
        catch (const std::runtime_error&) {rejected=true;}
        require(rejected,"Nonfinite native finger scale was silently accepted");
        return;
    }
    const TrackingFrame tracking{{0,0,0},{0,1,0},{0,0,1},{-1,0,0},100,{}};
    for (unsigned frame=0;frame<48;++frame) {
        const float angle=float(frame)*.130899694f;
        const auto axis=bone_unit(Vec3{1,2,-3});
        const auto vector=scale(axis,std::sin(angle*.5f));
        const Quaternion grip{vector.x,vector.y,vector.z,std::cos(angle*.5f)};
        const auto right=map_tracking(tracking,rotate(grip,{1,0,0}));
        const auto wristward=map_tracking(tracking,rotate(grip,{0,1,0}));
        for (unsigned side=0;side<2;++side) {
            const INT wrist=side?17:1;
            const auto anchor=outlast_grip_anchor(source,wrist);
            auto rig=source;
            rig.atoms[std::size_t(wrist)].rotation=
                outlast_grip_wrist_rotation(rig,tracking,grip,anchor);
            fingers.apply(rig,source.atoms,side,0,0,false);
            const auto open=rig;
            Vec3 knuckles{};
            for (unsigned finger=1;finger<5;++finger)
                knuckles=add(knuckles,scale(rig.atoms[std::size_t(wrist)+finger*3+1].position,.25f));
            require(dot(bone_unit(knuckles),wristward)<-.98f,
                "Open native hand faces wristward instead of toward the fingertips");
            fingers.apply(rig,source.atoms,side,.1f,.1f,false);
            for (unsigned finger=1;finger<5;++finger) {
                const auto tip=std::size_t(wrist)+finger*3+3;
                const auto bend=add(rig.atoms[tip].position,scale(open.atoms[tip].position,-1));
                require(dot(bend,right)*(side?-1.f:1.f)>.03f,
                    "Native finger starts curling onto the back of the hand");
            }
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        const std::string mode=argc==2?argv[1]:"";
        require(argc<=2 && (mode.empty() || mode=="--partial-animation" || mode=="--crouch-cycles" ||
            mode=="--hand-orientation" || mode=="--hidden-bones" || mode=="--shadow-pose" ||
            mode=="--fault-native-crouch"),
            "Unknown lifecycle test mode");
        if (mode.empty() || mode=="--shadow-pose") shadow_lifecycle();
        if (mode.empty() || mode=="--hand-orientation") hand_orientation();
        if (mode.empty() || mode=="--hidden-bones") {
            hand_orientation(true);
            std::cout<<"PASS: native_hidden_bone_visibility; both_hands; partial_finger_hiding; "
                <<"signed_scales; finite_skinning; repeated_recovery; nonfinite_rejected\n";
        }
        if (mode.empty() || mode=="--partial-animation") {
            Fixture f;
            f.targets.head.position={.03f,-.2f,.02f};
            auto solved=f.solve();
            f.current=solved.atoms;
            // A native animation replaces one component-space atom, not the whole cache.
            auto animated=f.native[5];
            animated.position.x+=17;
            f.current[5]=animated;
            require(f.ik.restore(f.component.data()),"Partial animation prevented removal of cached VR bones");
            for (std::size_t i=0;i<f.current.size();++i)
                require(equal(f.current[i],i==5?animated:f.native[i]),
                    "Restoration retained VR feedback or overwrote a newly authored animation atom");
            require(!f.ik.restore(f.component.data()),"Restoration rewrote an already restored pose");
            f.current=f.native;
            f.targets.tracked[0]=false;
            auto lost=f.solve();
            f.current=lost.atoms;
            require(f.ik.restore(f.component.data()),"Tracking-loss pose could not be restored");
            require(std::memcmp(f.current.data(),f.native.data(),f.native.size()*sizeof(BoneAtom))==0,
                "Tracking loss contaminated the next native animation");
            f.targets.tracked[0]=true;
            solved=f.solve();
            f.current=solved.atoms;
            auto replacement=f.asset;
            write(f.component.data(),0x238,replacement.data());
            require(!f.ik.restore(f.component.data()) &&
                std::memcmp(f.current.data(),solved.atoms.data(),f.current.size()*sizeof(BoneAtom))==0,
                "Restoration changed a replacement hero asset");
            write(f.component.data(),0x238,f.asset.data());
            require(f.ik.restore(f.component.data()),"Original hero asset failed to recover");
            for (unsigned frame=0;frame<60;++frame) {
                f.current=f.native;
                const float angle=float(frame)*.1f,c=std::cos(angle),s=std::sin(angle);
                const std::array<float,16> world{c,s,0,0,-s,c,0,0,0,0,1,0,
                    7000+float(frame)*13,-5000+float(frame)*7,20,1};
                write(f.component.data(),0xc0,world);
                auto turn=[&](Vec3 v) {return Vec3{c*v.x-s*v.y,s*v.x+c*v.y,v.z};};
                f.targets.tracking.origin={world[12],world[13],world[14]+170};
                f.targets.tracking.right=turn({0,1,0});
                f.targets.tracking.up={0,0,1};
                f.targets.tracking.backward=turn({-1,0,0});
                f.targets.head.position={};
                f.targets.tracked={frame%3!=0,frame%4!=0};
                solved=f.solve();
                f.current=solved.atoms;
                f.ik.restore(f.component.data());
                require(std::memcmp(f.current.data(),f.native.data(),f.native.size()*sizeof(BoneAtom))==0,
                    "Moving/turning/tracking changes accumulated VR bone feedback");
            }
            f.ik.reset();
            require(!f.ik.calibrated() && !f.ik.restore(f.component.data()),
                "Reset retained ownership or a stale animation cache");
            f.targets.tracked={true,true};
            f.ik.calibrate(outlast_skeleton(f.component.data()),f.bones,f.targets,f.skin);
            f.solve();
        }
        if (mode.empty() || mode=="--crouch-cycles" || mode=="--fault-native-crouch") {
            for (const float height:{0.f,80.f,60.f,40.f,20.f,40.f,0.f}) {
                Fixture manual;
                manual.targets.native_crouched=height!=0 && mode!="--fault-native-crouch";
                manual.targets.tracking.origin.z-=height;
                for (unsigned i=1;i<manual.current.size();++i) {
                    if (i==16 || i==19) continue;
                    const float offset=i==15 || i==18?height*.5f:height;
                    manual.current[i].position.z-=offset;
                }
                const auto source=manual.current;
                const auto solved=manual.solve();
                require(std::abs(solved.atoms[5].position.z-(170-height))<.03f,
                    "Native crouch body head diverged from its authored camera height");
                for (const INT foot:{16,19})
                    require(distance(solved.atoms[foot].position,source[foot].position)<.03f,
                        "Native crouch head alignment moved a planted foot");
                require(equal(solved.atoms[0],source[0]),"Native crouch moved the native root");
            }
            Fixture f;
            f.targets.compensate_native_crouch=true;
            for (unsigned cycle=0;cycle<6;++cycle) {
                f.current=f.native;
                f.targets.native_crouched=false;
                f.targets.stance_transition=true;
                f.targets.head.position.y=-.1f;
                for (auto& hand:f.targets.hands) hand.position.y=-.45f;
                f.solve();
                f.current=f.native;
                for (const INT foot:{16,19}) f.current[foot].position.x+=4+float(cycle)*8;
                f.targets.native_crouched=true;
                f.targets.stance_transition=false;
                f.targets.head.position.y=-.5f;
                for (auto& hand:f.targets.hands) hand.position.y=-.85f;
                const auto settled=f.solve();
                for (const INT foot:{16,19})
                    require(distance(settled.atoms[foot].position,f.native[foot].position)<.03f,
                        "Interrupted crouch cycle reused a previous stance's foot reference");
                require(std::abs(settled.atoms[5].position.z-120)<.03f,
                    "Physical crouching doubled or omitted the tracked head-height change");
                require(equal(settled.atoms[0],f.native[0]),"Physical crouch moved the native root");
            }
        }
        std::cout<<"PASS: synthetic_body_lifecycle_checks="<<checks
            <<"; partial_animation_restore; native_animation_preserved; tracking_loss_recovery; "
            <<"asset_replacement; moving_turning_body; repeated_interrupted_crouch\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
