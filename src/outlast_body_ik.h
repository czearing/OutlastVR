#pragma once
#include "outlast_skeleton.h"
#include <optional>
#include <bitset>

namespace olvr {
inline Quaternion bone_multiply(Quaternion a,Quaternion b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline Quaternion bone_inverse(Quaternion q) {return {-q.x,-q.y,-q.z,q.w};}
inline Vec3 bone_unit(Vec3 v) {
    const float length=std::sqrt(dot(v,v));
    if (!std::isfinite(length) || length<1e-6f) throw std::runtime_error("Degenerate IK direction");
    return scale(v,1/length);
}
inline Quaternion bone_normalize(Quaternion q) {
    const float length=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if (!std::isfinite(length) || length<1e-6f) throw std::runtime_error("Degenerate IK rotation");
    return {q.x/length,q.y/length,q.z/length,q.w/length};
}
inline Quaternion bone_between(Vec3 from,Vec3 to) {
    from=bone_unit(from);to=bone_unit(to);
    const auto value=std::clamp(dot(from,to),-1.f,1.f);
    if (value<-.99999f) {
        const auto axis=bone_unit(cross(from,std::abs(from.x)<.8f?Vec3{1,0,0}:Vec3{0,1,0}));
        return {axis.x,axis.y,axis.z,0};
    }
    const auto axis=cross(from,to);
    return bone_normalize({axis.x,axis.y,axis.z,1+value});
}
class BoneHierarchy {
    std::vector<INT> parents_;
    std::vector<std::bitset<256>> ancestors_;
    std::vector<std::vector<INT>> subtrees_;
public:
    explicit BoneHierarchy(const std::vector<INT>& parents):parents_(parents),
        ancestors_(parents.size()),subtrees_(parents.size()) {
        if (parents.empty() || parents.size()>256)
            throw std::runtime_error("Invalid bounded native bone hierarchy");
        for (std::size_t bone=0;bone<parents.size();++bone) {
            if (bone) {
                if (parents[bone]<0 || std::size_t(parents[bone])>=bone)
                    throw std::runtime_error("Cached native hierarchy must be parent-before-child");
                ancestors_[bone]=ancestors_[std::size_t(parents[bone])];
            }
            ancestors_[bone].set(bone);
            for (std::size_t root=0;root<=bone;++root)
                if (ancestors_[bone].test(root)) subtrees_[root].push_back(INT(bone));
        }
    }
    bool matches(const std::vector<INT>& parents) const {return parents_==parents;}
    bool descendant(INT index,INT root) const {
        return ancestors_.at(std::size_t(index)).test(std::size_t(root));
    }
    const std::vector<INT>& subtree(INT root) const {return subtrees_.at(std::size_t(root));}
};
inline bool bone_descendant(const NativeSkeleton& skeleton,INT index,INT root,
                            const BoneHierarchy* hierarchy=nullptr) {
    if (hierarchy) return hierarchy->descendant(index,root);
    while (index>root) index=skeleton.parents.at(std::size_t(index));
    return index==root;
}
inline void bone_rotate(NativeSkeleton& skeleton,INT root,Quaternion delta,
                        const BoneHierarchy* hierarchy=nullptr) {
    const auto pivot=skeleton.atoms.at(std::size_t(root)).position;
    auto apply=[&](INT index) {
        auto& atom=skeleton.atoms[std::size_t(index)];
        atom.position=add(pivot,rotate(delta,add(atom.position,scale(pivot,-1))));
        atom.rotation=bone_normalize(bone_multiply(delta,atom.rotation));
    };
    if (hierarchy) for (const auto index:hierarchy->subtree(root)) apply(index);
    else for (std::size_t i=std::size_t(root);i<skeleton.atoms.size();++i)
        if (bone_descendant(skeleton,INT(i),root)) apply(INT(i));
}
inline void bone_translate(NativeSkeleton& skeleton,INT root,Vec3 delta,
                           const BoneHierarchy* hierarchy=nullptr) {
    auto apply=[&](INT index) {auto& point=skeleton.atoms[std::size_t(index)].position;point=add(point,delta);};
    if (hierarchy) for (const auto index:hierarchy->subtree(root)) apply(index);
    else for (std::size_t i=std::size_t(root);i<skeleton.atoms.size();++i)
        if (bone_descendant(skeleton,INT(i),root)) apply(INT(i));
}
inline Vec3 bone_solve_limb(NativeSkeleton& skeleton,INT upper,INT middle,INT end,
                           Vec3 target,Vec3 pole,Quaternion orientation,const BoneHierarchy* hierarchy=nullptr) {
    if (skeleton.parents.at(std::size_t(middle))!=upper ||
        skeleton.parents.at(std::size_t(end))!=middle || !finite(target))
        throw std::runtime_error("IK limb does not match the observed native three-bone chain");
    const auto root=skeleton.atoms.at(std::size_t(upper)).position;
    const auto joint=skeleton.atoms.at(std::size_t(middle)).position;
    const auto tip=skeleton.atoms.at(std::size_t(end)).position;
    const float a=std::sqrt(dot(add(joint,scale(root,-1)),add(joint,scale(root,-1))));
    const float b=std::sqrt(dot(add(tip,scale(joint,-1)),add(tip,scale(joint,-1))));
    if (a<.01f || b<.01f) throw std::runtime_error("Native IK limb has zero length");
    auto difference=add(target,scale(root,-1));
    const float requested=std::sqrt(dot(difference,difference));
    const auto direction=requested>1e-6f?scale(difference,1/requested):bone_unit(add(tip,scale(root,-1)));
    const float distance=std::clamp(requested,std::abs(a-b)+.001f,a+b-.001f);
    target=add(root,scale(direction,distance));
    auto bend=add(pole,scale(direction,-dot(pole,direction)));
    if (dot(bend,bend)<1e-6f) {
        const auto old=add(joint,scale(root,-1));
        bend=add(old,scale(direction,-dot(old,direction)));
    }
    if (dot(bend,bend)<1e-6f)
        bend=cross(direction,std::abs(direction.x)<.8f?Vec3{1,0,0}:Vec3{0,1,0});
    bend=bone_unit(bend);
    const float along=(a*a+distance*distance-b*b)/(2*distance);
    const float away=std::sqrt(std::max(0.f,a*a-along*along));
    const auto elbow=add(root,add(scale(direction,along),scale(bend,away)));
    bone_rotate(skeleton,upper,bone_between(add(joint,scale(root,-1)),add(elbow,scale(root,-1))),hierarchy);
    const auto new_joint=skeleton.atoms[std::size_t(middle)].position;
    bone_rotate(skeleton,middle,bone_between(
        add(skeleton.atoms[std::size_t(end)].position,scale(new_joint,-1)),add(target,scale(new_joint,-1))),hierarchy);
    bone_rotate(skeleton,end,bone_multiply(bone_normalize(orientation),
        bone_inverse(skeleton.atoms[std::size_t(end)].rotation)),hierarchy);
    return target;
}
inline Vec3 bone_world_to_component_vector(const NativeSkeleton& skeleton,Vec3 world) {
    const Vec3 axes[]{
        {skeleton.world[0],skeleton.world[1],skeleton.world[2]},
        {skeleton.world[4],skeleton.world[5],skeleton.world[6]},
        {skeleton.world[8],skeleton.world[9],skeleton.world[10]}};
    const float squared=dot(axes[0],axes[0]);
    if (!std::isfinite(squared) || squared<1e-6f ||
        std::abs(dot(axes[1],axes[1])-squared)>.001f*squared ||
        std::abs(dot(axes[2],axes[2])-squared)>.001f*squared ||
        std::abs(dot(axes[0],axes[1]))>.001f*squared ||
        std::abs(dot(axes[0],axes[2]))>.001f*squared ||
        std::abs(dot(axes[1],axes[2]))>.001f*squared)
        throw std::runtime_error("Native body IK requires the observed uniformly scaled rigid mesh transform");
    return {dot(world,axes[0])/squared,dot(world,axes[1])/squared,dot(world,axes[2])/squared};
}
inline Vec3 bone_world_to_component(const NativeSkeleton& skeleton,Vec3 world) {
    return bone_world_to_component_vector(skeleton,add(world,
        {-skeleton.world[12],-skeleton.world[13],-skeleton.world[14]}));
}
inline Quaternion bone_tracking_rotation(const NativeSkeleton& skeleton,const TrackingFrame& tracking,Quaternion q) {
    const float tracking_sign=dot(cross(tracking.right,tracking.up),tracking.backward)<0?-1.f:1.f;
    const Vec3 a{skeleton.world[0],skeleton.world[1],skeleton.world[2]};
    const Vec3 b{skeleton.world[4],skeleton.world[5],skeleton.world[6]};
    const Vec3 c{skeleton.world[8],skeleton.world[9],skeleton.world[10]};
    const float mesh_sign=dot(cross(a,b),c)<0?-1.f:1.f;
    const auto axis=scale(bone_world_to_component_vector(skeleton,
        scale(map_tracking(tracking,{q.x,q.y,q.z}),tracking_sign)),
        mesh_sign*std::sqrt(dot(a,a)));
    return bone_normalize({axis.x,axis.y,axis.z,q.w});
}
class NativeCameraFingerGrip {
    INT left_,right_;
    std::array<Quaternion,15> left_reference_{},right_reference_{};
    static Quaternion reflected(Quaternion q) {return {-q.x,-q.y,q.z,q.w};}
public:
    NativeCameraFingerGrip(const NativeSkeleton& skeleton,INT left,INT right):left_(left),right_(right) {
        const auto* reference=skeleton_field<const unsigned char*>(skeleton.asset,0xc4);
        for (unsigned finger=0;finger<5;++finger)
            for (unsigned joint=0;joint<3;++joint) {
                const auto offset=finger*3+joint,index=offset+1;
                const auto l=left_+INT(index),r=right_+INT(index);
                const auto lp=joint?l-1:left_,rp=joint?r-1:right_;
                if (skeleton.parents.at(std::size_t(l))!=lp || skeleton.parents.at(std::size_t(r))!=rp)
                    throw std::runtime_error("Camera grip requires the actual paired five three-joint finger chains");
                const auto position=[&](INT bone) {
                    return skeleton_field<Vec3>(reference,std::size_t(bone)*0x50+0x20);
                };
                const auto rotation=[&](INT bone) {
                    return bone_normalize(skeleton_field<Quaternion>(reference,std::size_t(bone)*0x50+0x10));
                };
                const auto a=position(l),b=position(r);
                const auto difference=add(a,{-b.x,-b.y,b.z});
                const auto lq=rotation(l),rq=rotation(r),mirror=reflected(rq);
                const auto alignment=lq.x*mirror.x+lq.y*mirror.y+lq.z*mirror.z+lq.w*mirror.w;
                if (!finite(difference) || dot(difference,difference)>1e-8f ||
                    !std::isfinite(alignment) || std::abs(alignment)<.99999f)
                    throw std::runtime_error("Native left/right finger references are not anatomical local-Z mirrors");
                left_reference_[offset]=lq;right_reference_[offset]=rq;
            }
    }
    void apply(NativeSkeleton& skeleton,const std::vector<BoneAtom>& source) const {
        for (unsigned i=0;i<15;++i) {
            const auto l=std::size_t(left_)+i+1,r=std::size_t(right_)+i+1;
            const auto lp=std::size_t(skeleton.parents.at(l)),rp=std::size_t(skeleton.parents.at(r));
            const auto& original=source.at(l);
            const auto& original_parent=source.at(lp);
            const auto& parent=skeleton.atoms.at(lp);
            if (original_parent.scale<=0 || parent.scale<=0)
                throw std::runtime_error("Mirrored native camera grip has invalid parent scale");
            const auto right_local=bone_normalize(bone_multiply(
                bone_inverse(source.at(rp).rotation),source.at(r).rotation));
            // Conjugating by a reflection reverses quaternion X/Y, not just positions.
            const auto delta=reflected(bone_multiply(right_local,bone_inverse(right_reference_[i])));
            const auto left_local=bone_multiply(delta,left_reference_[i]);
            const auto position=scale(rotate(bone_inverse(original_parent.rotation),
                add(original.position,scale(original_parent.position,-1))),1/original_parent.scale);
            auto& target=skeleton.atoms.at(l);
            target.rotation=bone_normalize(bone_multiply(parent.rotation,left_local));
            target.position=add(parent.position,rotate(parent.rotation,scale(position,parent.scale)));
            target.scale=parent.scale*original.scale/original_parent.scale;
        }
    }
};
struct NativeGripAnchor {
    Vec3 center{},x{},z{};
};
class NativeControllerFingers {
    std::array<std::array<Quaternion,15>,2> rotations_{};
    std::array<std::array<Quaternion,15>,2> closed_{};
    std::array<std::array<Vec3,15>,2> positions_{};
    std::array<INT,2> wrists_{};
    static Quaternion blend(Quaternion a,Quaternion b,float t) {
        const float sign=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w<0?-1.f:1.f;
        return bone_normalize({a.x*(1-t)+b.x*t*sign,a.y*(1-t)+b.y*t*sign,
            a.z*(1-t)+b.z*t*sign,a.w*(1-t)+b.w*t*sign});
    }
public:
    NativeControllerFingers(const NativeSkeleton& skeleton,INT left,INT right):wrists_{left,right} {
        const auto* reference=skeleton_field<const unsigned char*>(skeleton.asset,0xc4);
        for (unsigned side=0;side<2;++side) for (unsigned i=0;i<15;++i) {
            const auto index=wrists_[side]+INT(i)+1;
            if (skeleton.parents.at(std::size_t(index))!=(i%3?index-1:wrists_[side]))
                throw std::runtime_error("Controller fingers require the actual five native three-joint chains");
            rotations_[side][i]=bone_normalize(skeleton_field<Quaternion>(
                reference,std::size_t(index)*0x50+0x10));
            constexpr float bend[]{1.221730476f,1.570796327f,.959931089f};
            closed_[side][i]=bone_multiply(rotations_[side][i],
                {0,0,-std::sin(bend[i%3]*.5f),std::cos(bend[i%3]*.5f)});
            positions_[side][i]=skeleton_field<Vec3>(reference,std::size_t(index)*0x50+0x20);
            if (!finite(positions_[side][i])) throw std::runtime_error("Nonfinite immutable native finger reference");
        }
    }
    void apply(NativeSkeleton& skeleton,const std::vector<BoneAtom>& original,unsigned side,
               float trigger,float squeeze,bool camera) const {
        if (side>1 || !std::isfinite(trigger) || !std::isfinite(squeeze) ||
            trigger<0 || trigger>1 || squeeze<0 || squeeze>1)
            throw std::invalid_argument("Invalid controller finger articulation");
        // Original, unmodified raised-camera local joints: native168 normal, pose300.
        constexpr std::array<Quaternion,15> stock{{
            {-.4213492552f,.2870322302f,-.3389591224f,-.7906857892f},
            {.0054090438f,-.0312653081f,.3036119197f,-.9522673075f},
            {.0000066283f,-.0000249360f,.1698643833f,-.9854674478f},
            {-.0422781746f,.0417034361f,.0006096075f,-.9982349462f},
            {.0000001510f,.0000002448f,.3602821928f,-.9328433639f},
            {-.0000021521f,.0000036731f,.3140278201f,-.9494137813f},
            {.0672658777f,.0196894714f,.1126702882f,-.9911574207f},
            {.0000319258f,.0000771869f,.3847671113f,-.9230136852f},
            {.0000269876f,-.0000749945f,.3583573025f,-.9335845101f},
            {.0999985870f,-.0029516544f,.1953591541f,-.9756158933f},
            {.0000000633f,-.0000001335f,.2046251090f,-.9788404184f},
            {-.0000026803f,.0000033548f,.3375403293f,-.9413110677f},
            {.1373515731f,-.0026136958f,.0687398614f,-.9881308342f},
            {-.0182035971f,-.0002301797f,.1927944990f,-.9810702611f},
            {-.0000014452f,.0000039289f,.3067466391f,-.9517912058f}
        }};
        for (unsigned i=0;i<15;++i) {
            const auto index=std::size_t(wrists_[side])+i+1;
            const auto parent=std::size_t(skeleton.parents[index]);
            const unsigned finger=i/3;
            const auto reference=rotations_[side][i];
            auto closed=stock[i];
            if (!side) closed={-closed.x,-closed.y,closed.z,closed.w};
            const float amount=camera?1.f:finger==0?std::max(trigger,squeeze):finger==1?trigger:squeeze;
            if (!camera && finger) {
                closed=closed_[side][i];
            }
            const auto local=blend(reference,closed,amount);
            const auto& p=skeleton.atoms[parent];
            auto& target=skeleton.atoms[index];
            if (original[parent].scale<=0 || p.scale<=0)
                throw std::runtime_error("Controller finger parent has invalid scale");
            target.position=add(p.position,rotate(p.rotation,scale(positions_[side][i],p.scale)));
            target.rotation=bone_normalize(bone_multiply(p.rotation,local));
            target.scale=p.scale*original[index].scale/original[parent].scale;
        }
    }
};
inline NativeGripAnchor outlast_grip_anchor(const NativeSkeleton& skeleton,INT hand) {
    const auto* reference=skeleton_field<const unsigned char*>(skeleton.asset,0xc4);
    std::vector<Vec3> knuckles;
    for (std::size_t i=std::size_t(hand)+1;i<skeleton.parents.size();++i) {
        if (skeleton.parents[i]!=hand) continue;
        unsigned children=0;
        for (std::size_t j=i+1;j<skeleton.parents.size();++j)
            if (skeleton.parents[j]==INT(i)) ++children;
        if (!children) continue;
        if (children!=1) throw std::runtime_error("Native palm finger hierarchy changed");
        const auto position=skeleton_field<Vec3>(reference,i*0x50+0x20);
        if (!finite(position)) throw std::runtime_error("Nonfinite native reference palm");
        knuckles.push_back(position);
    }
    if (knuckles.size()!=5) throw std::runtime_error("Native palm requires five reference finger roots");
    const auto thumb=std::min_element(knuckles.begin(),knuckles.end(),
        [](Vec3 a,Vec3 b) {return dot(a,a)<dot(b,b);});
    const auto thumb_position=*thumb;
    knuckles.erase(thumb);
    std::sort(knuckles.begin(),knuckles.end(),[&](Vec3 a,Vec3 b) {
        a=add(a,scale(thumb_position,-1));b=add(b,scale(thumb_position,-1));
        return dot(a,a)<dot(b,b);
    });
    NativeGripAnchor result{};
    Vec3 distal{};
    for (const auto point:knuckles) {
        if (dot(thumb_position,thumb_position)>=dot(point,point)*.75f)
            throw std::runtime_error("Native reference thumb/palm geometry is ambiguous");
        distal=add(distal,scale(point,.25f));
    }
    result.center=scale(distal,.5f);
    // OpenXR grip -Z runs from little finger toward thumb, not toward
    // the fingertips. These immutable metacarpals also locate the palm.
    result.z=bone_unit(add(knuckles.back(),scale(knuckles.front(),-1)));
    result.x=bone_unit(cross(distal,result.z));
    result.z=bone_unit(add(result.z,scale(result.x,-dot(result.z,result.x))));
    return result;
}
inline Quaternion outlast_grip_wrist_rotation(const NativeSkeleton& skeleton,const TrackingFrame& tracking,
                                              Quaternion grip,const NativeGripAnchor& anchor) {
    const auto mapped_x=bone_unit(bone_world_to_component_vector(skeleton,
        map_tracking(tracking,rotate(grip,{1,0,0}))));
    const auto mapped_z=bone_unit(bone_world_to_component_vector(skeleton,
        map_tracking(tracking,rotate(grip,{0,0,1}))));
    const auto first=bone_between(anchor.x,mapped_x);
    const auto rotated_z=rotate(first,anchor.z);
    const float twist=std::atan2(dot(mapped_x,cross(rotated_z,mapped_z)),
        std::clamp(dot(rotated_z,mapped_z),-1.f,1.f));
    const auto axis=scale(mapped_x,std::sin(twist*.5f));
    return bone_normalize(bone_multiply({axis.x,axis.y,axis.z,std::cos(twist*.5f)},first));
}
struct BodyIKTargets {
    std::array<EyePose,2> hands{};
    std::array<EyePose,2> controllers{};
    float hand_wristward_m=0;
    std::array<Vec3,2> raw_hand_positions{};
    std::array<bool,2> world_contact{};
    std::array<bool,2> tracked{};
    bool mirror_camera_grip=false;
    bool controller_fingers=false;
    std::array<float,2> finger_trigger{},finger_squeeze{};
    std::array<bool,2> controller_camera_grip{};
    EyePose head{{0,0,0},{0,0,0,1}};
    TrackingFrame tracking{};
    float standing_height_delta=0;
    bool compensate_native_crouch=false;
    bool native_crouched=false,stance_transition=false;
    bool grip_anchored=false;
};
struct BodyIKResult {
    std::array<Vec3,2> hands{};
    std::array<Vec3,2> feet{};
    std::array<float,2> hand_target_error{},shoulder_adjustment{};
    unsigned solved_hands=0;
};
class OutlastBodyIK {
    void* component_=nullptr;
    void* asset_=nullptr;
    std::array<INT,20> bones_{};
    struct SkinningChain {INT driver,first,second;};
    std::array<SkinningChain,8> skinning_{};
    std::array<Quaternion,2> hand_offset_{};
    std::array<NativeGripAnchor,2> grip_anchors_{};
    bool grip_calibrated_=false;
    Quaternion head_offset_{0,0,0,1};
    std::vector<BoneAtom> original_,applied_,standing_;
    std::optional<std::array<Vec3,2>> crouch_foot_reference_;
    std::optional<BoneHierarchy> hierarchy_;
    std::optional<NativeCameraFingerGrip> camera_finger_grip_;
    std::optional<NativeControllerFingers> controller_fingers_;
public:
    bool calibrated() const {return component_!=nullptr;}
    const NativeGripAnchor& grip_anchor(unsigned side) const {
        if (!grip_calibrated_) throw std::logic_error("Native palm has no immutable grip calibration");
        return grip_anchors_.at(side);
    }
    Vec3 hand_anchor(const NativeSkeleton& skeleton,unsigned side,bool grip) const {
        const auto& wrist=skeleton.atoms.at(std::size_t(bones_[side?13:9]));
        return grip?add(wrist.position,rotate(wrist.rotation,scale(grip_anchor(side).center,wrist.scale))):
            wrist.position;
    }
    bool owns(const NativeSkeleton& skeleton) const {
        return component_==skeleton.component && asset_==skeleton.asset &&
            hierarchy_ && hierarchy_->matches(skeleton.parents);
    }
    void reset() {*this=OutlastBodyIK{};}
    void calibrate(const NativeSkeleton& skeleton,const std::array<INT,20>& bones,
                   const BodyIKTargets& targets,const std::array<INT,16>& skin_bones) {
        for (const auto index:bones)
            if (index<0 || std::size_t(index)>=skeleton.atoms.size())
                throw std::runtime_error("Required native VR body bone is missing");
        bones_=bones;
        for (const auto chain: {std::array<INT,3>{7,8,9},std::array<INT,3>{11,12,13},
                               std::array<INT,3>{14,15,16},std::array<INT,3>{17,18,19}})
            if (skeleton.parents[bones_[chain[1]]]!=bones_[chain[0]] ||
                skeleton.parents[bones_[chain[2]]]!=bones_[chain[1]])
                throw std::runtime_error("Native VR rig chain topology changed");
        unsigned segment=0;
        for (const INT role:{7,8,11,12,14,15,17,18}) {
            const auto first=skin_bones[segment*2],second=skin_bones[segment*2+1];
            for (const auto index:{first,second}) {
                if (index<=0 || std::size_t(index)>=skeleton.atoms.size() ||
                    std::find(bones.begin(),bones.end(),index)!=bones.end() ||
                    std::count(skin_bones.begin(),skin_bones.end(),index)!=1)
                    throw std::runtime_error("Required distinct native VR skinning bone is missing");
            }
            if (skeleton.parents[second]!=first)
                throw std::runtime_error("Named native parallel skinning-chain topology changed");
            skinning_[segment]={bones_[role],first,second};
            ++segment;
        }
        if (targets.grip_anchored) {
            for (unsigned side=0;side<2;++side)
                grip_anchors_[side]=outlast_grip_anchor(skeleton,bones_[side?13:9]);
            grip_calibrated_=true;
        }
        hierarchy_.emplace(skeleton.parents);
        component_=skeleton.component;asset_=skeleton.asset;
        original_.clear();applied_.clear();
        camera_finger_grip_.reset();
        controller_fingers_.reset();
        standing_=skeleton.atoms;
        crouch_foot_reference_.reset();
        head_offset_=bone_multiply(bone_inverse(bone_tracking_rotation(skeleton,targets.tracking,
            targets.head.orientation)),skeleton.atoms[bones_[5]].rotation);
        for (unsigned side=0;side<2;++side)
            hand_offset_[side]=skeleton.atoms[bones_[side?13:9]].rotation;
    }
    bool restore(void* component) const {
        if (component!=component_ || applied_.empty() ||
            skeleton_field<void*>(component,0x238)!=asset_ ||
            skeleton_field<INT>(component,0x2e0)!=INT(applied_.size())) return false;
        auto* atoms=skeleton_field<BoneAtom*>(component,0x2d8);
        if (atoms && std::memcmp(atoms,applied_.data(),applied_.size()*sizeof(BoneAtom))==0) {
            std::memcpy(atoms,original_.data(),original_.size()*sizeof(BoneAtom));
            return true;
        }
        bool restored=false;
        if (atoms)
            for (std::size_t i=0;i<applied_.size();++i)
                // Native partial evaluations can replace only some cached atoms.
                if (std::memcmp(&atoms[i],&applied_[i],sizeof(BoneAtom))==0 &&
                    std::memcmp(&original_[i],&applied_[i],sizeof(BoneAtom))!=0) {
                    atoms[i]=original_[i];
                    restored=true;
                }
        return restored;
    }
    BodyIKResult apply(NativeSkeleton& skeleton,const BodyIKTargets& targets) {
        if (!owns(skeleton)) throw std::logic_error("Native body IK has no matching calibrated rig");
        const auto* hierarchy=&*hierarchy_;
        original_=skeleton.atoms;
        BodyIKResult result{};
        const std::array<INT,2> feet{bones_[16],bones_[19]};
        std::array<Quaternion,2> foot_rotations{};
        {
            // Native locomotion can rotate the authored shoulders independently of
            // tracking. Keep the tracked torso stable; retain native standing legs.
            const INT reference_root=bones_[targets.compensate_native_crouch?1:2];
            for (std::size_t i=0;i<skeleton.atoms.size();++i) {
                const bool native_arm=(!targets.tracked[0] && bone_descendant(skeleton,INT(i),bones_[6],hierarchy)) ||
                    (!targets.tracked[1] && bone_descendant(skeleton,INT(i),bones_[10],hierarchy));
                if (bone_descendant(skeleton,INT(i),reference_root,hierarchy) &&
                    (targets.compensate_native_crouch || !native_arm)) {
                    skeleton.atoms[i]=standing_[i];
                    skeleton.atoms[i].scale=original_[i].scale;
                }
            }
            for (const INT hand:{bones_[9],bones_[13]}) {
                const auto delta=bone_multiply(skeleton.atoms[hand].rotation,
                    bone_inverse(original_[hand].rotation));
                for (std::size_t i=std::size_t(hand)+1;i<skeleton.atoms.size();++i)
                    if (bone_descendant(skeleton,INT(i),hand,hierarchy)) {
                        skeleton.atoms[i].position=add(skeleton.atoms[hand].position,
                            rotate(delta,add(original_[i].position,scale(original_[hand].position,-1))));
                        skeleton.atoms[i].rotation=bone_normalize(bone_multiply(delta,original_[i].rotation));
                    }
            }
            if (targets.compensate_native_crouch && targets.native_crouched &&
                !targets.stance_transition && !crouch_foot_reference_)
                crouch_foot_reference_=std::array<Vec3,2>{original_[feet[0]].position,original_[feet[1]].position};
            if (!targets.compensate_native_crouch || !targets.native_crouched)
                crouch_foot_reference_.reset();
        }
        if (!targets.compensate_native_crouch)
            for (std::size_t i=4;i<skinning_.size();++i)
                for (const INT helper:{skinning_[i].first,skinning_[i].second})
                    skeleton.atoms[helper]=original_[helper];
        for (unsigned hand=0;hand<2;++hand) {
            result.feet[hand]=skeleton.atoms[feet[hand]].position;
            if (targets.compensate_native_crouch && targets.native_crouched &&
                !targets.stance_transition && crouch_foot_reference_)
                result.feet[hand]=add(result.feet[hand],add(original_[feet[hand]].position,
                    scale((*crouch_foot_reference_)[hand],-1)));
            foot_rotations[hand]=skeleton.atoms[feet[hand]].rotation;
        }
        std::array<std::array<BoneAtom,3>,8> skinning_source{};
        for (std::size_t i=0;i<skinning_.size();++i) {
            const auto& chain=skinning_[i];
            skinning_source[i]={skeleton.atoms[chain.driver],skeleton.atoms[chain.first],skeleton.atoms[chain.second]};
        }
        if (!std::isfinite(targets.standing_height_delta))
            throw std::runtime_error("Nonfinite native body standing-height adjustment");
        bone_translate(skeleton,bones_[2],bone_world_to_component_vector(skeleton,
            scale(targets.tracking.up,targets.standing_height_delta*targets.tracking.units_per_meter)),hierarchy);
        const auto displacement=bone_world_to_component_vector(skeleton,
            scale(map_tracking(targets.tracking,targets.head.position),targets.tracking.units_per_meter));
        bone_translate(skeleton,bones_[1],{displacement.x*.55f,displacement.y*.55f,
            displacement.z},hierarchy);
        const auto head=skeleton.atoms[bones_[5]].position;
        const auto spine=skeleton.atoms[bones_[2]].position;
        const Vec3 lean{displacement.x*.45f,displacement.y*.45f,0};
        if (dot(lean,lean)>1e-6f)
            bone_rotate(skeleton,bones_[2],bone_between(add(head,scale(spine,-1)),
                add(add(head,lean),scale(spine,-1))),hierarchy);
        bone_rotate(skeleton,bones_[5],bone_multiply(bone_multiply(
            bone_tracking_rotation(skeleton,targets.tracking,targets.head.orientation),head_offset_),
            bone_inverse(skeleton.atoms[bones_[5]].rotation)),hierarchy);
        for (unsigned side=0;side<2;++side) {
            const INT leg=side?17:14;
            const auto forward=bone_world_to_component_vector(skeleton,scale(targets.tracking.backward,-1));
            bone_solve_limb(skeleton,bones_[leg],bones_[leg+1],bones_[leg+2],
                result.feet[side],forward,foot_rotations[side],hierarchy);
            if (!targets.tracked[side]) continue;
            const INT arm=side?11:7;
            const auto rotation=targets.grip_anchored?
                outlast_grip_wrist_rotation(skeleton,targets.tracking,targets.hands[side].orientation,grip_anchor(side)):
                bone_multiply(bone_tracking_rotation(skeleton,targets.tracking,targets.hands[side].orientation),
                    hand_offset_[side]);
            const auto world_target=add(targets.tracking.origin,
                scale(map_tracking(targets.tracking,targets.hands[side].position),targets.tracking.units_per_meter));
            const auto anchor_goal=bone_world_to_component(skeleton,world_target);
            const auto goal=targets.grip_anchored?add(anchor_goal,scale(rotate(rotation,
                scale(grip_anchor(side).center,skeleton.atoms[bones_[arm+2]].scale)),-1)):anchor_goal;
            const auto shoulder=skeleton.atoms[bones_[arm]].position;
            const auto elbow=skeleton.atoms[bones_[arm+1]].position;
            const auto wrist=skeleton.atoms[bones_[arm+2]].position;
            const auto length=[](Vec3 value){return std::sqrt(dot(value,value));};
            const float upper=length(add(elbow,scale(shoulder,-1)));
            const float lower=length(add(wrist,scale(elbow,-1)));
            const auto delta=add(goal,scale(shoulder,-1));
            const float requested=length(delta);
            const float reach=std::clamp(requested,std::abs(upper-lower)+.002f,upper+lower-.002f);
            const float mesh_scale=length({skeleton.world[0],skeleton.world[1],skeleton.world[2]});
            const float limit=.35f*targets.tracking.units_per_meter/mesh_scale;
            const float adjustment=std::clamp(requested-reach,-limit,limit);
            // Native first-person shoulders sit behind the camera. Prioritize
            // tracked wrists by adapting the clavicle, without stretching arm segments.
            if (requested>1e-6f && std::abs(adjustment)>1e-6f) {
                bone_translate(skeleton,bones_[arm-1],scale(delta,adjustment/requested),hierarchy);
                result.shoulder_adjustment[side]=std::abs(adjustment)*mesh_scale;
            }
            const auto pole=bone_world_to_component_vector(skeleton,
                map_tracking(targets.tracking,{side?.4f:-.4f,-.7f,.15f}));
            result.hands[side]=bone_solve_limb(skeleton,bones_[arm],bones_[arm+1],bones_[arm+2],
                goal,pole,rotation,hierarchy);
            result.hand_target_error[side]=length(add(anchor_goal,
                scale(hand_anchor(skeleton,side,targets.grip_anchored),-1)))*mesh_scale;
            ++result.solved_hands;
        }
        // Retail vertices use parallel, split-segment skin bones, not the IK
        // driver joints. Carry their authored roll/offsets with the solved segment.
        for (std::size_t i=0;i<skinning_.size();++i) {
            const auto& chain=skinning_[i];
            const auto& before=skinning_source[i][0];
            const auto& after=skeleton.atoms[chain.driver];
            const auto delta=bone_normalize(bone_multiply(after.rotation,bone_inverse(before.rotation)));
            for (unsigned helper=0;helper<2;++helper) {
                const auto& source=skinning_source[i][helper+1];
                auto& target=skeleton.atoms[helper?chain.second:chain.first];
                target.position=add(after.position,rotate(delta,add(source.position,scale(before.position,-1))));
                target.rotation=bone_normalize(bone_multiply(delta,source.rotation));
            }
        }
        if (targets.controller_fingers) {
            if (!controller_fingers_) controller_fingers_.emplace(skeleton,bones_[9],bones_[13]);
            for (unsigned side=0;side<2;++side) if (targets.tracked[side])
                controller_fingers_->apply(skeleton,original_,side,targets.finger_trigger[side],
                    targets.finger_squeeze[side],targets.controller_camera_grip[side]);
        } else if (targets.mirror_camera_grip) {
            if (!targets.tracked[0] || !targets.grip_anchored)
                throw std::logic_error("Mirrored camera grip requires the tracked native left palm");
            if (!camera_finger_grip_)
                camera_finger_grip_.emplace(skeleton,bones_[9],bones_[13]);
            camera_finger_grip_->apply(skeleton,original_);
        }
        for (const auto& atom:skeleton.atoms)
            if (!finite(atom.position)) throw std::runtime_error("Native body IK produced a nonfinite pose");
        applied_=skeleton.atoms;
        return result;
    }
};
}
