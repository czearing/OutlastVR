#pragma once
#include "outlast_camcorder.h"

namespace olvr {

inline Vec3 camcorder_tracking_vector(const TrackingFrame& frame,Vec3 world) {
    return {dot(world,frame.right),dot(world,frame.up),dot(world,frame.backward)};
}
inline Vec3 camcorder_tracking_point(const TrackingFrame& frame,Vec3 world) {
    if (!std::isfinite(frame.units_per_meter) || frame.units_per_meter<=0)
        throw std::invalid_argument("Native camera requires a positive tracking scale");
    return scale(camcorder_tracking_vector(frame,add(world,scale(frame.origin,-1))),1/frame.units_per_meter);
}
inline Quaternion camcorder_align_axes(Vec3 source_forward,Vec3 source_up,Vec3 forward,Vec3 up) {
    forward=bone_unit(forward);
    up=bone_unit(add(up,scale(forward,-dot(forward,up))));
    const auto first=bone_between(source_forward,forward);
    const auto old_up=rotate(first,source_up);
    const float angle=std::atan2(dot(forward,cross(old_up,up)),dot(old_up,up));
    const auto axis=scale(forward,std::sin(angle*.5f));
    return bone_normalize(bone_multiply({axis.x,axis.y,axis.z,std::cos(angle*.5f)},first));
}
inline Quaternion camcorder_axes(Vec3 forward,Vec3 up) {
    return camcorder_align_axes({0,0,-1},{0,1,0},forward,up);
}
inline EyePose camcorder_native_pose(const TrackingFrame& frame,const std::array<float,16>& world,
                                    Vec3 point,Vec3 forward,Vec3 up) {
    const auto position=add(skeleton_vector(world,point),{world[12],world[13],world[14]});
    return {camcorder_tracking_point(frame,position),
        camcorder_axes(camcorder_tracking_vector(frame,skeleton_vector(world,forward)),
            camcorder_tracking_vector(frame,skeleton_vector(world,up)))};
}
struct NativeCamcorderPose {
    EyePose model{},lens{},screen{};
    Vec3 screen_half_extents{};
    Vec3 native_lens{};
};
inline NativeCamcorderPose outlast_native_camcorder_pose(const NativeSkeleton& mesh,
                                                        const TrackingFrame& tracking) {
    if (mesh.atoms.size()!=2 || mesh.parents!=std::vector<INT>{0,0})
        throw std::runtime_error("Retail handycam must have the admitted two-bone hierarchy");
    const auto* reference=skeleton_field<const unsigned char*>(mesh.asset,0xc4);
    const auto root_q=bone_normalize(skeleton_field<Quaternion>(reference,0x10));
    const auto root_p=skeleton_field<Vec3>(reference,0x20);
    const auto screen_q=bone_normalize(bone_multiply(root_q,
        bone_normalize(skeleton_field<Quaternion>(reference,0x50+0x10))));
    const auto screen_p=add(root_p,rotate(root_q,skeleton_field<Vec3>(reference,0x50+0x20)));
    const auto root=skeleton_bone_matrix(mesh,0),screen=skeleton_bone_matrix(mesh,1);
    // OLGame.upk's Lens and CameraScreen_INST sections; inverse reference skinning
    // places their optical center and LCD rectangle on the current native bones.
    const Vec3 lens_asset{9.208990097f,.509961605f,2.846994519f};
    const Vec3 screen_asset{3.93251705f,-9.53543067f,2.85399204f};
    const auto lens_local=rotate(bone_inverse(root_q),add(lens_asset,scale(root_p,-1)));
    const auto screen_local=rotate(bone_inverse(screen_q),add(screen_asset,scale(screen_p,-1)));
    NativeCamcorderPose result{};
    result.model=camcorder_native_pose(tracking,root,{0,0,0},
        rotate(bone_inverse(root_q),{1,0,0}),rotate(bone_inverse(root_q),{0,0,1}));
    result.lens=camcorder_native_pose(tracking,root,lens_local,
        rotate(bone_inverse(root_q),{1,0,0}),rotate(bone_inverse(root_q),{0,0,1}));
    result.screen=camcorder_native_pose(tracking,screen,screen_local,
        rotate(bone_inverse(screen_q),{1,0,0}),rotate(bone_inverse(screen_q),{0,0,1}));
    result.native_lens=add(skeleton_vector(root,lens_local),{root[12],root[13],root[14]});
    const auto right=skeleton_vector(screen,rotate(bone_inverse(screen_q),{0,1,0}));
    const auto up=skeleton_vector(screen,rotate(bone_inverse(screen_q),{0,0,1}));
    result.screen_half_extents={4.24970460f*std::sqrt(dot(right,right))/tracking.units_per_meter,
        2.47671664f*std::sqrt(dot(up,up))/tracking.units_per_meter,1};
    return result;
}

struct NativeCamcorderMeshBindings {
    using Attach=void(WINAPI*)(void*,void*,UINT64,const Vec3*,const INT*,const Vec3*);
    using Matrix=float*(WINAPI*)(float*,const Vec3*,const INT*,const Vec3*);
    using Update=void(WINAPI*)(void*,void*,void*,const float*,INT);
    BoneNameEntry name=nullptr;
    BoneIndexEntry index=nullptr;
    Attach attach=nullptr;
    Matrix matrix=nullptr;
    Update update=nullptr;
};
class NativeCamcorderMesh {
    struct Attachment {
        void* parent=nullptr;
        UINT64 bone=0;
        Vec3 position{},scale{1,1,1};
        std::array<INT,3> rotation{};
    };
    NativeCamcorderMeshBindings bindings_;
    void* hero_=nullptr;
    std::array<void*,2> components_{};
    std::array<void*,2> assets_{};
    std::array<Attachment,2> original_{};
    UINT original_flags_=0;
    bool active_=false;
    int hand_=-2;
    Attachment current_{};
    Vec3 grip_point_{};
    Vec3 mesh_grip_point_{};
    std::array<Vec3,2> palm_points_{};
    std::array<Attachment,2> held_attachments_{};
    EyePose held_pose_{};
    TrackingFrame held_tracking_{};
    Attachment held_attachment(const NativeSkeleton& body,int hand) const {
        if (hand<0 || hand>1) throw std::invalid_argument("Invalid native held-camera hand");
        const auto& target=held_attachments_[unsigned(hand)];
        if (target.parent!=body.component || bindings_.index(body.component,target.bone)<0)
            throw std::runtime_error("Tracked camera lost its native wrist");
        return target;
    }
    UINT64 name(const wchar_t* text) const {
        UINT64 result=0;bindings_.name(&result,text,1);return result;
    }
    template<class F> static F method(void* component,std::size_t offset) {
        const auto* table=skeleton_field<void**>(component,0);
        return reinterpret_cast<F>(table[offset/sizeof(void*)]);
    }
    static std::optional<Attachment> attachment(void* parent,void* component) {
        const auto* records=skeleton_field<const unsigned char*>(parent,0x3ec);
        const auto count=skeleton_field<INT>(parent,0x3f4),capacity=skeleton_field<INT>(parent,0x3f8);
        if (count<0 || capacity<count || capacity>1024 || (count && !records))
            throw std::runtime_error("Invalid native skeletal attachment array");
        for (INT i=0;i<count;++i) {
            const auto* record=records+std::size_t(i)*0x34;
            if (skeleton_field<void*>(record,0)!=component) continue;
            return Attachment{parent,skeleton_field<UINT64>(record,8),
                skeleton_field<Vec3>(record,0x10),skeleton_field<Vec3>(record,0x28),
                skeleton_field<std::array<INT,3>>(record,0x1c)};
        }
        return std::nullopt;
    }
    void attach(void* component,const Attachment& value) const {
        if (value.parent==current_.parent && attachment(value.parent,component)) {
            auto* records=skeleton_field<unsigned char*>(value.parent,0x3ec);
            const auto count=skeleton_field<INT>(value.parent,0x3f4);
            for (INT i=0;i<count;++i) {
                auto* record=records+std::size_t(i)*0x34;
                if (skeleton_field<void*>(record,0)!=component) continue;
                // Native AttachComponent appends; same-parent socket motion edits its owned record.
                std::memcpy(record+8,&value.bone,sizeof(value.bone));
                std::memcpy(record+0x10,&value.position,sizeof(value.position));
                std::memcpy(record+0x1c,value.rotation.data(),sizeof(value.rotation));
                std::memcpy(record+0x28,&value.scale,sizeof(value.scale));
                return;
            }
            throw std::runtime_error("Owned native camera attachment changed during its update");
        }
        bindings_.attach(value.parent,component,value.bone,&value.position,value.rotation.data(),&value.scale);
    }
    static void hidden(void* component,bool value) {
        if (((skeleton_field<UINT>(component,0x178)&4)!=0)!=value)
            method<void(WINAPI*)(void*,UINT)>(component,0x4c0)(component,value?1:0);
    }
    static void owner_no_see(void* component,bool value) {
        if (((skeleton_field<UINT>(component,0x178)&0x100000)!=0)!=value)
            method<void(WINAPI*)(void*,UINT)>(component,0x4a0)(component,value?1:0);
    }
    void bind(void* hero) {
        if (hero_==hero && components_[0]==skeleton_field<void*>(hero,0x2cf0) &&
            assets_[0]==skeleton_field<void*>(components_[0],0x238)) return;
        hero_=hero;active_=false;hand_=-2;
        for (unsigned i=0;i<2;++i) {
            auto* component=skeleton_field<void*>(hero,0x2cf0+i*8);
            auto* asset=skeleton_field<void*>(component,0x238);
            if (!asset || skeleton_field<UINT64>(asset,0x48)!=name(i?L"handycamShadow":L"handycam") ||
                skeleton_field<UINT64>(component,0x68)!=name(i?L"CameraMeshShadowProxyComponent":L"CameraMeshComponent") ||
                skeleton_field<INT>(asset,0xcc)!=2)
                throw std::runtime_error("Native camera component does not own the actual retail handycam asset");
            components_[i]=component;assets_[i]=asset;
            std::optional<Attachment> record;
            for (const auto offset:{0x444u,0x2ce0u}) {
                const auto candidate=attachment(skeleton_field<void*>(hero,offset),component);
                if (!candidate) continue;
                if (record) throw std::runtime_error("Retail camera has duplicate original rig attachments");
                record=candidate;
            }
            if (!record) throw std::runtime_error("Retail camera has no original native rig attachment");
            original_[i]=*record;
        }
        original_flags_=skeleton_field<UINT>(components_[0],0x178);
        const auto body=outlast_skeleton(skeleton_field<void*>(hero,0x444));
        const auto wrist=bindings_.index(body.component,name(L"Hero-R-Hand"));
        const auto socket=bindings_.index(body.component,name(L"Hero-R-Hand_aux"));
        if (wrist<0 || socket<0 || body.parents.at(std::size_t(socket))!=wrist)
            throw std::runtime_error("Retail camera socket is not on the native right hand");
        if (original_[0].parent!=body.component || original_[0].bone!=name(L"Hero-R-Hand_aux"))
            throw std::runtime_error("Retail camera's stock mount is not its authored animated right-hand socket");
        const auto anchor=outlast_grip_anchor(body,wrist);
        palm_points_[1]=anchor.center;
        const auto left=bindings_.index(body.component,name(L"Hero-L-Hand"));
        if (left<0) throw std::runtime_error("Retail camera lost the native left wrist");
        palm_points_[0]=outlast_grip_anchor(body,left).center;
        // Native raised pose300, not the auxiliary socket's reference/T-pose:
        // native_camera_grip_calibration.json retains the independent source atoms.
        grip_point_={-.812240687f,4.82349342f,-5.27160731f};
        mesh_grip_point_=grip_point_;
        const std::array<NativeGripAnchor,2> anchors{outlast_grip_anchor(body,left),anchor};
        const TrackingFrame native_axes{{0,0,0},{0,1,0},{0,0,1},{-1,0,0},1,{0,0,0}};
        for (unsigned hand=0;hand<2;++hand) {
            const auto& destination=anchors[hand];
            // The wrist solver reflects tracking Y into the native basis.
            // Keep the model fixed in the palm, looking along grip -Z with +Y up;
            // neither the animated socket nor the independent aim ray can twist it.
            const auto forward=scale(destination.z,-1);
            const auto up=scale(cross(destination.z,destination.x),-1);
            auto& target=held_attachments_[hand];
            target.parent=body.component;target.bone=name(hand?L"Hero-R-Hand":L"Hero-L-Hand");
            target.scale=original_[0].scale;
            target.rotation=camcorder_native_rotation(native_axes,{{0,0,0},
                camcorder_axes(camcorder_tracking_vector(native_axes,forward),
                    camcorder_tracking_vector(native_axes,up))});
            const Vec3 zero{};
            alignas(16) std::array<float,16> local{};
            bindings_.matrix(local.data(),&target.scale,target.rotation.data(),&zero);
            auto contact=mesh_grip_point_;
            if (!hand) contact.y=-contact.y;
            target.position=add(palm_points_[hand],scale(skeleton_vector(local,contact),-1));
        }
    }
    void enforce_control() {
        if (!active_) return;
        for (auto* component:components_) {
            auto* owner=skeleton_field<void*>(component,0x78);
            if (owner && owner!=hero_)
                throw std::runtime_error("Native retail camera belongs to a different actor");
            const auto attached=attachment(current_.parent,component);
            if (!attached || attached->bone!=current_.bone || attached->rotation!=current_.rotation ||
                std::memcmp(&attached->position,&current_.position,sizeof(Vec3))!=0 ||
                std::memcmp(&attached->scale,&current_.scale,sizeof(Vec3))!=0) attach(component,current_);
            if (!skeleton_field<void*>(component,0x78)) {
                auto* scene=skeleton_field<void*>(current_.parent,0x70);
                if (!scene || skeleton_field<void*>(current_.parent,0x78)!=hero_)
                    throw std::runtime_error("Native retail camera lost its owned parent scene");
                const auto transform=world(outlast_skeleton(current_.parent));
                // Retail visibility changes detach components before the normal update restores ownership.
                bindings_.update(component,scene,hero_,transform.data(),1);
            }
            if (skeleton_field<void*>(component,0x78)!=hero_)
                throw std::runtime_error("Native retail camera update did not restore actor ownership");
        }
        hidden(components_[0],false);
        owner_no_see(components_[0],false);
    }
public:
    explicit NativeCamcorderMesh(NativeCamcorderMeshBindings bindings):bindings_(bindings) {
        if (!bindings.name || !bindings.index || !bindings.attach || !bindings.matrix || !bindings.update)
            throw std::invalid_argument("Incomplete native retail camera mesh bindings");
    }
    void reset() {hero_=nullptr;components_={};assets_={};active_=false;hand_=-2;}
    void observe_stock_visibility(void* hero) {
        if (hero_==hero && components_[0]==skeleton_field<void*>(hero,0x2cf0)) {
            original_flags_=skeleton_field<UINT>(components_[0],0x178);
            enforce_control();
        }
    }
    void release_control() {
        if (!active_) return;
        const auto flags=original_flags_;
        // Native visibility callbacks must not reattach the controlled rig during restoration.
        active_=false;hand_=-2;
        for (unsigned i=0;i<2;++i) attach(components_[i],original_[i]);
        hidden(components_[0],(flags&4)!=0);
        owner_no_see(components_[0],(flags&0x100000)!=0);
        for (unsigned i=0;i<2;++i) {
            const auto restored=attachment(original_[i].parent,components_[i]);
            if (!restored || restored->bone!=original_[i].bone ||
                restored->rotation!=original_[i].rotation ||
                std::memcmp(&restored->position,&original_[i].position,sizeof(Vec3))!=0 ||
                std::memcmp(&restored->scale,&original_[i].scale,sizeof(Vec3))!=0)
                throw std::runtime_error("Retail camera's original native attachment was not restored; component="+
                    std::to_string(i)+"; expected_bone="+std::to_string(original_[i].bone)+
                    "; restored_bone="+std::to_string(restored?restored->bone:0)+
                    "; expected_position="+std::to_string(original_[i].position.x)+","+
                    std::to_string(original_[i].position.y)+","+std::to_string(original_[i].position.z)+
                    "; restored_position="+std::to_string(restored?restored->position.x:0)+","+
                    std::to_string(restored?restored->position.y:0)+","+
                    std::to_string(restored?restored->position.z:0));
        }
    }
    bool owns(void* hero,void* component) const {
        return active_ && hero_==hero && component==components_[0];
    }
    void* component() const {return components_[0];}
    void* asset() const {return assets_[0];}
    void* parent() const {return current_.parent;}
    UINT64 attachment_bone() const {return current_.bone;}
    int hand() const {return hand_;}
    Vec3 grip_point() const {return grip_point_;}
    Vec3 mesh_grip_point() const {
        auto result=mesh_grip_point_;
        if (hand_==0) result.y=-result.y;
        return result;
    }
    EyePose held_pose() const {return held_pose_;}
    void update_held_tracking(const TrackingFrame& tracking,const MotionFrame& motion) {
        if (!active_ || hand_<0 || !motion.focused || !motion.hands[unsigned(hand_)].grip_tracked)
            throw std::logic_error("Native camera late tracking requires its actual tracked holding hand");
        const auto& hand=motion.hands[unsigned(hand_)];
        const auto& q=hand.aim_tracked?hand.aim.orientation:hand.grip.orientation;
        held_pose_.orientation=camcorder_offset({held_pose_.position,{q.x,q.y,q.z,q.w}},{0,0,0}).orientation;
        held_tracking_=tracking;
    }
    std::array<float,16> stock_local() const {
        alignas(16) std::array<float,16> result{};
        bindings_.matrix(result.data(),&original_[0].scale,original_[0].rotation.data(),&original_[0].position);
        return result;
    }
    Quaternion tracked_orientation(const NativeSkeleton& body,Quaternion controller) const {
        if (!active_ || hand_<0 || body.component!=current_.parent)
            throw std::logic_error("Camera aim requires its current tracked native hand");
        const auto wrist=bindings_.index(body.component,name(L"Hero-R-Hand"));
        const auto socket=bindings_.index(body.component,original_[0].bone);
        if (wrist<0 || socket<0 || body.parents.at(std::size_t(socket))!=wrist)
            throw std::runtime_error("Camera aim lost its authored right-hand socket");
        const auto relative=bone_normalize(bone_multiply(
            bone_inverse(body.atoms.at(std::size_t(wrist)).rotation),
            body.atoms.at(std::size_t(socket)).rotation));
        const auto local=stock_local();
        const auto anchor=outlast_grip_anchor(body,wrist);
        const auto y=cross(anchor.z,anchor.x);
        auto axis=[&](unsigned row) {
            const auto value=rotate(relative,{local[row*4],local[row*4+1],local[row*4+2]});
            // The wrist solver maps X/Z through UE's reflected tracking basis.
            return Vec3{dot(value,anchor.x),-dot(value,y),dot(value,anchor.z)};
        };
        const auto native=camcorder_axes(axis(0),axis(2));
        const auto desired=camcorder_grip(
            {{controller.x,controller.y,controller.z,controller.w},{0,0,0}},unsigned(hand_)).orientation;
        return bone_normalize(bone_multiply(desired,bone_inverse(native)));
    }
    std::array<float,16> world(const NativeSkeleton& parent) const {
        const auto bone=bindings_.index(parent.component,current_.bone);
        if (bone<0) throw std::runtime_error("Retail camera parent bone was lost");
        alignas(16) std::array<float,16> local{};
        if (bindings_.matrix(local.data(),&current_.scale,current_.rotation.data(),&current_.position)!=local.data())
            throw std::runtime_error("Native camera matrix constructor returned an unexpected buffer");
        const auto native=skeleton_bone_matrix(parent,bone);
        std::array<float,16> result{};
        for (unsigned row=0;row<4;++row)
            for (unsigned col=0;col<4;++col)
                for (unsigned k=0;k<4;++k)
                    result[row*4+col]+=local[row*4+k]*native[k*4+col];
        return result;
    }
    void update_transform(const NativeSkeleton& parent) {
        if (!active_ || parent.component!=current_.parent) return;
        if (hand_>=0) {
            current_=held_attachment(parent,hand_);
            enforce_control();
        }
        const auto transform=world(parent);
        for (auto* component:components_) {
            const auto flags=skeleton_field<UINT>(component,0x80)|8u;
            std::memcpy(static_cast<unsigned char*>(component)+0x80,&flags,sizeof(flags));
            bindings_.update(component,skeleton_field<void*>(parent.component,0x70),hero_,transform.data(),1);
        }
    }
    std::optional<NativeCamcorderPose> pose(const TrackingFrame& tracking) const {
        if (!active_) return std::nullopt;
        if (!skeleton_field<INT>(components_[0],0x2e0)) return std::nullopt;
        return outlast_native_camcorder_pose(outlast_skeleton(components_[0]),tracking);
    }
    std::optional<EyePose> holster(void* hero,const TrackingFrame& tracking) const {
        if (hero_!=hero || hand_!=-1) return std::nullopt;
        const auto current=pose(tracking);
        return current?std::optional<EyePose>(current->model):std::nullopt;
    }
    void control(void* hero,const TrackingFrame& tracking,const CamcorderFrame& frame) {
        if (!frame.visible) {release_control();return;}
        bind(hero);
        auto* body=skeleton_field<void*>(hero,0x444);
        const int hand=frame.held?frame.hand:-1;
        if (hand< -1 || hand>1) throw std::runtime_error("Invalid retail camera ownership");
        if (hand>=0) {
            held_pose_=frame.pose;held_tracking_=tracking;
            current_=held_attachment(outlast_skeleton(body),hand);
            hand_=hand;active_=true;
            enforce_control();
            return;
        }
        if (!active_ || hand!=hand_) {
            Attachment target{};
            target.parent=body;
            target.bone=name(L"Hero-Spine2");
            const auto bone=bindings_.index(body,target.bone);
            if (bone<0) throw std::runtime_error("Native camera attachment bone is missing");
            const auto skeleton=outlast_skeleton(body);
            Vec3 forward{},up{};
            NativeSkeleton attachment_frame{};
            attachment_frame.world=skeleton_bone_matrix(skeleton,bone);
            const auto position=add(tracking.origin,scale(map_tracking(tracking,frame.pose.position),
                tracking.units_per_meter));
            target.position=bone_world_to_component(attachment_frame,position);
            forward=bone_world_to_component_vector(attachment_frame,
                map_tracking(tracking,rotate(frame.pose.orientation,{0,0,-1})));
            up=bone_world_to_component_vector(attachment_frame,
                map_tracking(tracking,rotate(frame.pose.orientation,{0,1,0})));
            const TrackingFrame native_axes{{0,0,0},{0,1,0},{0,0,1},{-1,0,0},1,{0,0,0}};
            const EyePose local{{0,0,0},camcorder_axes(camcorder_tracking_vector(native_axes,forward),
                camcorder_tracking_vector(native_axes,up))};
            target.rotation=camcorder_native_rotation(native_axes,local);
            current_=target;hand_=hand;active_=true;
        }
        enforce_control();
    }
};
}
