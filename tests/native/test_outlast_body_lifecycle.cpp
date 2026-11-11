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
}
int main(int argc,char** argv) {
    try {
        const std::string mode=argc==2?argv[1]:"";
        require(argc<=2 && (mode.empty() || mode=="--partial-animation" || mode=="--crouch-cycles"),
            "Unknown lifecycle test mode");
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
        if (mode.empty() || mode=="--crouch-cycles") {
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
