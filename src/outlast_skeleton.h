#pragma once
#include "xr_view_constants.h"
#include <vector>
#include <cstring>
#include <optional>

namespace olvr {
struct BoneAtom {
    Quaternion rotation;
    Vec3 position;
    float scale;
};
static_assert(sizeof(BoneAtom)==0x20);
// Retail UpdateSkelPose reads its frame time from XMM1, not EDX.
using SkeletonEvaluateEntry=void(WINAPI*)(void*,float,UINT);
using BoneNameEntry=void*(WINAPI*)(void*,const wchar_t*,INT);
using BoneIndexEntry=INT(WINAPI*)(void*,UINT64);
using BoneMatrixEntry=void(WINAPI*)(void*,float*,INT);

struct NativeSkeleton {
    void* component=nullptr;
    void* asset=nullptr;
    BoneAtom* destination=nullptr;
    std::vector<BoneAtom> atoms;
    std::vector<INT> parents;
    std::array<float,16> world{};
};
template<class T> T skeleton_field(const void* object,std::size_t offset) {
    if (!object) throw std::runtime_error("Null native skeleton object");
    T result{};
    std::memcpy(&result,static_cast<const unsigned char*>(object)+offset,sizeof(result));
    return result;
}
inline std::optional<unsigned> outlast_body_component(const void* hero,const void* component) {
    if (!hero || !component || !skeleton_field<void*>(component,0x238) ||
        skeleton_field<void*>(component,0x33c)) return std::nullopt;
    if (component==skeleton_field<void*>(hero,0x444)) return 0;
    if (component==skeleton_field<void*>(hero,0x2ce0)) return 1;
    return std::nullopt;
}
inline NativeSkeleton outlast_skeleton(void* component) {
    NativeSkeleton result{};
    result.component=component;
    result.asset=skeleton_field<void*>(component,0x238);
    const auto count=skeleton_field<INT>(component,0x2e0);
    const auto reference_count=skeleton_field<INT>(result.asset,0xcc);
    const auto* reference=skeleton_field<const unsigned char*>(result.asset,0xc4);
    result.destination=skeleton_field<BoneAtom*>(component,0x2d8);
    if (count<1 || count>256 || count!=reference_count || !reference || !result.destination)
        throw std::runtime_error("Native skeleton does not match recovered bounded component/reference layout");
    result.atoms.assign(result.destination,result.destination+count);
    result.parents.resize(count);
    for (INT bone=0;bone<count;++bone) {
        const auto parent=skeleton_field<INT>(reference,std::size_t(bone)*0x50+0x40);
        if (bone && (parent<0 || parent>=bone))
            throw std::runtime_error("Native skeleton is not a parent-before-child hierarchy");
        result.parents[bone]=parent;
        const auto& atom=result.atoms[bone];
        if (!finite(atom.position) || !std::isfinite(atom.scale) ||
            !std::isfinite(dot({atom.rotation.x,atom.rotation.y,atom.rotation.z},
                              {atom.rotation.x,atom.rotation.y,atom.rotation.z})+
                           atom.rotation.w*atom.rotation.w))
            throw std::runtime_error("Nonfinite native component-space bone");
    }
    result.world=skeleton_field<std::array<float,16>>(component,0xc0);
    for (auto value:result.world)
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite native component-to-world matrix");
    return result;
}
inline Vec3 skeleton_vector(const std::array<float,16>& matrix,Vec3 value) {
    return {value.x*matrix[0]+value.y*matrix[4]+value.z*matrix[8],
            value.x*matrix[1]+value.y*matrix[5]+value.z*matrix[9],
            value.x*matrix[2]+value.y*matrix[6]+value.z*matrix[10]};
}
inline std::array<float,16> skeleton_bone_matrix(const NativeSkeleton& skeleton,INT index) {
    const auto& atom=skeleton.atoms.at(std::size_t(index));
    const auto q=atom.rotation;
    // Native GetBoneMatrix scales doubled quaternion components before products.
    const float x2=(q.x+q.x)*atom.scale,y2=(q.y+q.y)*atom.scale,z2=(q.z+q.z)*atom.scale;
    const float xx=q.x*x2,yy=q.y*y2,zz=q.z*z2;
    const float xy=q.x*y2,xz=q.x*z2,yz=q.y*z2,wx=q.w*x2,wy=q.w*y2,wz=q.w*z2;
    const Vec3 axes[]{
        {atom.scale-(yy+zz),xy+wz,xz-wy},
        {xy-wz,atom.scale-(xx+zz),yz+wx},
        {xz+wy,yz-wx,atom.scale-(xx+yy)}};
    std::array<float,16> result{};
    for (unsigned axis=0;axis<3;++axis) {
        const auto world=skeleton_vector(skeleton.world,axes[axis]);
        result[axis*4]=world.x;result[axis*4+1]=world.y;result[axis*4+2]=world.z;
    }
    const auto translation=add(skeleton_vector(skeleton.world,atom.position),
        {skeleton.world[12],skeleton.world[13],skeleton.world[14]});
    result[12]=translation.x;result[13]=translation.y;result[14]=translation.z;result[15]=1;
    return result;
}
inline constexpr const wchar_t* outlast_rig_names[]{
    L"Hero-Root",L"Hero-Pelvis",L"Hero-Spine",L"Hero-Spine1",L"Hero-Spine2",L"Hero-Head",
    L"Hero-L-Clavicle",L"Hero-L-UpperArm",L"Hero-L-Forearm",L"Hero-L-Hand",
    L"Hero-R-Clavicle",L"Hero-R-UpperArm",L"Hero-R-Forearm",L"Hero-R-Hand",
    L"Hero-L-Thigh",L"Hero-L-Calf",L"Hero-L-Foot",L"Hero-R-Thigh",L"Hero-R-Calf",L"Hero-R-Foot"};
inline constexpr const wchar_t* outlast_skinning_names[]{
    L"Hero-LUpArmTwist",L"Hero-LUpArmTwist1",L"Hero-L-ForeTwist",L"Hero-L-ForeTwist1",
    L"Hero-RUpArmTwist",L"Hero-RUpArmTwist1",L"Hero-R-ForeTwist",L"Hero-R-ForeTwist1",
    L"Hero-LThighTwist",L"Hero-LThighTwist1",L"Hero-LCalfTwist",L"Hero-LCalfTwist1",
    L"Hero-RThighTwist",L"Hero-RThighTwist1",L"Hero-RCalfTwist",L"Hero-RCalfTwist1"};
}
