#include "outlast_native_camcorder.h"
#include <iostream>
#include <map>
#include <string>
#include <fstream>
#include <filesystem>

using namespace olvr;
namespace {
unsigned checks=0;
void require(bool value,const char* message) {
    ++checks;if (!value) throw std::runtime_error(message);
}
bool close(Vec3 a,Vec3 b,float tolerance=.0001f) {
    const auto d=add(a,scale(b,-1));return dot(d,d)<tolerance*tolerance;
}
template<class T> void write(void* object,std::size_t offset,T value) {
    std::memcpy(static_cast<unsigned char*>(object)+offset,&value,sizeof(value));
}
using Object=std::array<unsigned char,0x600>;
struct Rig {
    Object asset{},component{};
    std::vector<unsigned char> reference=std::vector<unsigned char>(160);
    NativeSkeleton skeleton{};
    Rig() {
        const Quaternion root{3.0616168e-17f,0,0,-.9999999404f};
        const Quaternion screen{6.12323294e-17f,-2.67655042e-24f,.9999999404f,-4.37113847e-8f};
        const Vec3 p{.677226305f,.483697951f,2.386303186f},q{3.616254568f,-3.538652182f,.54374218f};
        write(reference.data(),0x10,root);write(reference.data(),0x20,p);
        write(reference.data(),0x60,screen);write(reference.data(),0x70,q);
        write(asset.data(),0xc4,reference.data());write(asset.data(),0xcc,INT(2));
        skeleton.asset=asset.data();skeleton.component=component.data();
        skeleton.parents={0,0};
        skeleton.atoms={{bone_normalize(root),p,1},{bone_normalize(bone_multiply(root,screen)),
            add(p,rotate(root,q)),1}};
        skeleton.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    }
};
std::map<std::wstring,UINT64> names;
std::array<INT,20> body_roles{};
void* WINAPI make_name(void* result,const wchar_t* text,INT) {
    auto inserted=names.emplace(text,UINT64(names.size()+1));
    write(result,0,inserted.first->second);return result;
}
UINT64 name(const wchar_t* text) {UINT64 result=0;make_name(&result,text,1);return result;}
INT WINAPI bone_index(void*,UINT64 value) {
    if (value==name(L"Hero-Spine2")) return body_roles[4];
    if (value==name(L"Hero-L-Hand")) return body_roles[9];
    if (value==name(L"Hero-R-Hand")) return body_roles[13];
    if (value==name(L"Hero-L-Hand_aux")) return 28;
    if (value==name(L"Hero-R-Hand_aux")) return 52;
    return -1;
}
struct Graph {
    std::array<unsigned char,0x4000> hero{};
    Rig body{},camera{},shadow{};
    Object native_shadow{};
    std::array<void*,160> vtable{};
    std::array<std::array<unsigned char,416>,2> records{};
    unsigned attachments=0;
    NativeCamcorderMesh* visibility_observer=nullptr;
    unsigned visibility_callbacks=0;
    static Graph* current;
    static float* WINAPI matrix(float* output,const Vec3* size,const INT* rotation,const Vec3* position) {
        auto angle=[](INT value) {return float((UINT(value)>>2)&0x3fff)*(6.2831853071795864769f/16384);};
        const auto p=angle(rotation[0]),y=angle(rotation[1]),r=angle(rotation[2]);
        const auto sp=std::sin(p),cp=std::cos(p),sy=std::sin(y),cy=std::cos(y),sr=std::sin(r),cr=std::cos(r);
        const float value[]{cp*cy*size->x,cp*sy*size->x,sp*size->x,0,
            (sp*sr*cy-cr*sy)*size->y,(sp*sr*sy+cy*cr)*size->y,-cp*sr*size->y,0,
            -(cr*sp*cy+sy*sr)*size->z,(cy*sr-cr*sp*sy)*size->z,cp*cr*size->z,0,
            position->x,position->y,position->z,1};
        std::copy_n(value,16,output);return output;
    }
    static void WINAPI update(void* component,void*,void* owner,const float* transform,INT skip_children) {
        require(owner==current->hero.data() && skip_children==1,"Incorrect native transform ownership/recursion gate");
        write(component,0x78,owner);
        std::memcpy(static_cast<unsigned char*>(component)+0xc0,transform,64);
    }
    static void WINAPI hidden(void* component,UINT value) {
        const auto old=skeleton_field<UINT>(component,0x178);
        write(component,0x178,(old&~4u)|(value?4u:0));
        if (current->visibility_observer) {
            ++current->visibility_callbacks;
            current->visibility_observer->observe_stock_visibility(current->hero.data());
        }
    }
    static void WINAPI owner_no_see(void* component,UINT value) {
        const auto old=skeleton_field<UINT>(component,0x178);
        write(component,0x178,(old&~0x100000u)|(value?0x100000u:0));
        if (current->visibility_observer) {
            ++current->visibility_callbacks;
            current->visibility_observer->observe_stock_visibility(current->hero.data());
        }
    }
    static void WINAPI attach(void* parent,void* component,UINT64 bone,const Vec3* position,
                              const INT* rotation,const Vec3* scale) {
        auto& g=*current;++g.attachments;
        for (auto* candidate:{g.body.component.data(),g.native_shadow.data()}) {
            if (candidate==parent) continue;
            auto* records=skeleton_field<unsigned char*>(candidate,0x3ec);
            auto count=skeleton_field<INT>(candidate,0x3f4);
            for (INT i=0;i<count;++i)
                if (skeleton_field<void*>(records,std::size_t(i)*52)==component) {
                    std::memmove(records+i*52,records+(i+1)*52,std::size_t(count-i-1)*52);
                    write(candidate,0x3f4,--count);break;
                }
        }
        const auto count=skeleton_field<INT>(parent,0x3f4);
        require(count<8,"Mock attachment overflow");
        auto* record=skeleton_field<unsigned char*>(parent,0x3ec)+count*52;
        write(record,0,component);write(record,8,bone);write(record,16,*position);
        std::memcpy(record+28,rotation,12);write(record,40,*scale);
        write(parent,0x3f4,count+1);write(component,0x78,g.hero.data());
    }
    explicit Graph(const std::filesystem::path& fixture) {
        current=this;
        std::ifstream source(fixture,std::ios::binary);
        UINT64 magic=0;UINT count=0;
        source.read(reinterpret_cast<char*>(&magic),8);source.read(reinterpret_cast<char*>(&count),4);
        require(magic==0x314B49524C4FULL && count==73,"Camera acceptance requires the retained actual native hero rig");
        source.read(reinterpret_cast<char*>(body_roles.data()),80);
        body.skeleton.parents.resize(count);body.skeleton.atoms.resize(count);
        source.read(reinterpret_cast<char*>(body.skeleton.parents.data()),std::streamsize(count*4));
        source.read(reinterpret_cast<char*>(body.skeleton.world.data()),64);
        source.read(reinterpret_cast<char*>(body.skeleton.atoms.data()),std::streamsize(count*32));
        require(bool(source),"Truncated native hero fixture");
        auto reference_path=fixture;reference_path.replace_extension(".reference.bin");
        std::ifstream reference_source(reference_path,std::ios::binary);
        body.reference.resize(count*80);
        reference_source.read(reinterpret_cast<char*>(body.reference.data()),std::streamsize(body.reference.size()));
        require(bool(reference_source),"Missing actual native reference palm/socket records");
        write(body.asset.data(),0xc4,body.reference.data());write(body.asset.data(),0xcc,INT(count));
        vtable[0x4c0/8]=reinterpret_cast<void*>(&hidden);
        vtable[0x4a0/8]=reinterpret_cast<void*>(&owner_no_see);
        for (auto* rig:{&body,&camera,&shadow}) {
            write(rig->component.data(),0,vtable.data());
            write(rig->component.data(),0x238,rig->asset.data());
            write(rig->component.data(),0x2d8,rig->skeleton.atoms.data());
            write(rig->component.data(),0x2e0,INT(rig->skeleton.atoms.size()));
            write(rig->component.data(),0xc0,rig->skeleton.world);
        }
        write(camera.component.data(),0x68,name(L"CameraMeshComponent"));
        write(shadow.component.data(),0x68,name(L"CameraMeshShadowProxyComponent"));
        write(camera.asset.data(),0x48,name(L"handycam"));
        write(shadow.asset.data(),0x48,name(L"handycamShadow"));
        write(camera.component.data(),0x178,UINT(0x100004));
        write(hero.data(),0x444,body.component.data());
        write(hero.data(),0x2ce0,native_shadow.data());
        write(hero.data(),0x2cf0,camera.component.data());
        write(hero.data(),0x2cf8,shadow.component.data());
        for (auto* parent:{body.component.data(),native_shadow.data()}) {
            write(parent,0x70,hero.data());
            write(parent,0x78,hero.data());
        }
        write(body.component.data(),0x3ec,records[0].data());
        write(native_shadow.data(),0x3ec,records[1].data());
        write(body.component.data(),0x3f8,INT(8));
        write(native_shadow.data(),0x3f8,INT(8));
        const Vec3 zero{},one{1,1,1};const INT rotation[3]{};
        attach(body.component.data(),camera.component.data(),name(L"Hero-R-Hand_aux"),&zero,rotation,&one);
        attach(native_shadow.data(),shadow.component.data(),name(L"Hero-R-Hand_aux"),&zero,rotation,&one);
        attachments=0;
    }
};
Graph* Graph::current=nullptr;
}
int main(int argc,char** argv) {
    try {
        if (argc<2 || argc>3)
            throw std::invalid_argument("Native camera acceptance requires the actual hero fixture and optional native camera trajectory");
        const TrackingFrame tracking{{0,0,0},{0,1,0},{0,0,1},{-1,0,0},100,{0,0,0}};
        Rig rig;
        const Vec3 lens{9.208990097f,.509961605f,2.846994519f};
        const Vec3 lcd{3.93251705f,-9.53543067f,2.85399204f};
        for (float yaw:{-2.7f,-1.2f,0.f,.8f,2.3f})
            for (float pitch:{-.9f,0.f,.7f})
                for (float roll:{-1.1f,0.f,1.4f}) {
                    const auto rotation=bone_normalize(bone_multiply(
                        {0,0,std::sin(yaw*.5f),std::cos(yaw*.5f)},bone_multiply(
                        {0,std::sin(pitch*.5f),0,std::cos(pitch*.5f)},
                        {std::sin(roll*.5f),0,0,std::cos(roll*.5f)})));
                    const Vec3 origin{-9271.5f,24,133};
                    NativeSkeleton transform{};
                    transform.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
                    transform.atoms={{rotation,origin,1}};
                    rig.skeleton.world=skeleton_bone_matrix(transform,0);
                    const auto pose=outlast_native_camcorder_pose(rig.skeleton,tracking);
                    require(close(pose.native_lens,add(origin,rotate(rotation,lens)),.001f),
                        "Native optical center is not inverse-reference skinned onto the actual model");
                    const auto screen_world=add(tracking.origin,
                        scale(map_tracking(tracking,pose.screen.position),tracking.units_per_meter));
                    require(close(screen_world,add(origin,rotate(rotation,lcd)),.002f),
                        "Live display is not on the retail LCD plane");
                    require(close(map_tracking(tracking,rotate(pose.lens.orientation,{0,0,-1})),
                        rotate(rotation,{1,0,0})),"Lens optical direction disagrees with the real model");
                    require(close(map_tracking(tracking,rotate(pose.screen.orientation,{0,1,0})),
                        rotate(rotation,{0,0,1})),"Retail LCD up axis changed");
                    require(std::abs(pose.screen_half_extents.x-.042497046f)<1e-6f &&
                        std::abs(pose.screen_half_extents.y-.0247671664f)<1e-6f,
                        "Live display extends beyond the actual LCD bounds");
                }
        Graph graph(argv[1]);
        NativeControllerFingers controller_fingers(graph.body.skeleton,body_roles[9],body_roles[13]);
        for (unsigned side=0;side<2;++side) {
            const auto wrist=body_roles[side?13:9];
            const auto anchor=outlast_grip_anchor(graph.body.skeleton,wrist);
            const auto rotation=outlast_grip_wrist_rotation(graph.body.skeleton,tracking,{0,0,0,1},anchor);
            auto curled=graph.body.skeleton;
            curled.atoms[std::size_t(wrist)].rotation=rotation;
            controller_fingers.apply(curled,graph.body.skeleton.atoms,side,0,0,false);
            const auto open=curled;
            controller_fingers.apply(curled,graph.body.skeleton.atoms,side,.1f,.1f,false);
            for (unsigned finger=1;finger<5;++finger) {
                const auto tip=std::size_t(wrist)+finger*3+3;
                const auto bend=skeleton_vector(curled.world,
                    add(curled.atoms[tip].position,scale(open.atoms[tip].position,-1)));
                require(dot(bend,tracking.right)*(side?-1.f:1.f)>.03f,
                    "Native fingers start curling away from the palm side of the OpenXR grip");
            }
            Vec3 knuckles{};
            for (unsigned finger=1;finger<5;++finger)
                knuckles=add(knuckles,scale(skeleton_field<Vec3>(graph.body.reference.data(),
                    (std::size_t(wrist)+finger*3+1)*0x50+0x20),.25f));
            const auto forward=bone_unit(skeleton_vector(graph.body.skeleton.world,rotate(rotation,knuckles)));
            require(dot(forward,tracking.up)<-.98f,
                "Native metacarpals face wristward instead of outward from the controller grip");
        }
        {
            const auto source=graph.body.skeleton.atoms;
            auto mirrored=graph.body.skeleton;
            NativeCameraFingerGrip fingers(mirrored,body_roles[9],body_roles[13]);
            fingers.apply(mirrored,source);
            auto reflect=[](Vec3 value) {return Vec3{value.x,value.y,-value.z};};
            for (unsigned i=0;i<15;++i) {
                const auto left=std::size_t(body_roles[9])+i+1,right=std::size_t(body_roles[13])+i+1;
                const auto lp=std::size_t(mirrored.parents[left]),rp=std::size_t(mirrored.parents[right]);
                const auto lref=bone_normalize(skeleton_field<Quaternion>(
                    graph.body.reference.data(),left*0x50+0x10));
                const auto rref=bone_normalize(skeleton_field<Quaternion>(
                    graph.body.reference.data(),right*0x50+0x10));
                for (const Vec3 axis:{Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,1}}) {
                    auto expected=reflect(rotate(lref,axis));
                    expected=rotate(bone_inverse(rref),expected);
                    expected=rotate(source[right].rotation,expected);
                    expected=reflect(rotate(bone_inverse(source[rp].rotation),expected));
                    const auto actual=rotate(bone_inverse(mirrored.atoms[lp].rotation),
                        rotate(mirrored.atoms[left].rotation,axis));
                    require(close(actual,expected,.00002f),"Anatomical finger rotation reflection failed");
                }
                const auto original_position=scale(rotate(bone_inverse(source[lp].rotation),
                    add(source[left].position,scale(source[lp].position,-1))),1/source[lp].scale);
                const auto actual_position=scale(rotate(bone_inverse(mirrored.atoms[lp].rotation),
                    add(mirrored.atoms[left].position,scale(mirrored.atoms[lp].position,-1))),
                    1/mirrored.atoms[lp].scale);
                require(close(original_position,actual_position,.0001f),"Mirrored grip changed native left finger lengths");
            }
            for (std::size_t i=0;i<source.size();++i)
                if (i<=std::size_t(body_roles[9]) || i>std::size_t(body_roles[9])+15)
                    require(std::memcmp(&source[i],&mirrored.atoms[i],sizeof(BoneAtom))==0,
                        "Left camera grip modified an unrelated native bone");
            const auto saved=skeleton_field<Vec3>(graph.body.reference.data(),13*0x50+0x20);
            write(graph.body.reference.data(),13*0x50+0x20,add(saved,{1,0,0}));
            bool rejected=false;
            try {NativeCameraFingerGrip invalid(mirrored,body_roles[9],body_roles[13]);}
            catch (const std::runtime_error&) {rejected=true;}
            write(graph.body.reference.data(),13*0x50+0x20,saved);
            require(rejected,"Anatomical mirror accepted a corrupted actual reference finger");
        }
        NativeCamcorderMesh mesh({make_name,bone_index,Graph::attach,Graph::matrix,Graph::update});
        CamcorderFrame frame{};frame.visible=true;frame.pose={{.11f,1.38f,-.16f},{0,0,0,1}};
        mesh.control(graph.hero.data(),tracking,frame);
        require(mesh.owns(graph.hero.data(),graph.camera.component.data()),"Retail camera was not admitted");
        require(!mesh.owns(graph.hero.data(),graph.shadow.component.data()),"Shadow proxy became the visible model");
        require((skeleton_field<UINT>(graph.camera.component.data(),0x178)&0x100004)==0,
            "Real retail camera remained hidden from its owner");
        require(mesh.holster(graph.hero.data(),tracking).has_value(),"Real chest camera cannot be grabbed");
        const auto attached=graph.attachments;
        mesh.control(graph.hero.data(),tracking,frame);
        require(graph.attachments==attached,"Unchanged camera attachment was rebuilt");
        auto check_held=[&](int hand) {
            frame.held=true;frame.hand=hand;
            mesh.control(graph.hero.data(),tracking,frame);
            require(mesh.hand()==hand && mesh.attachment_bone()==name(hand?L"Hero-R-Hand":L"Hero-L-Hand"),
                "Retail camera is attached to the wrong native hand");
            require(!mesh.holster(graph.hero.data(),tracking),"Held native camera remained a chest-grab target");
            mesh.update_transform(graph.body.skeleton);
            const auto bone=body_roles[hand?13:9];
            const auto anchor=outlast_grip_anchor(graph.body.skeleton,bone);
            const auto hand_world=skeleton_bone_matrix(graph.body.skeleton,bone);
            const auto world=skeleton_field<std::array<float,16>>(graph.camera.component.data(),0xc0);
            const auto contact=add(skeleton_vector(world,mesh.mesh_grip_point()),
                {world[12],world[13],world[14]});
            const auto expected=add(skeleton_vector(hand_world,anchor.center),
                {hand_world[12],hand_world[13],hand_world[14]});
            require(close(contact,expected,.002f),"Native camcorder slipped off the immutable palm contact");
            const auto reused=graph.attachments;
            mesh.update_transform(graph.body.skeleton);
            require(graph.attachments==reused,"Unchanged animated stock mount rebuilt the native graph");
            const auto original_atoms=graph.body.skeleton.atoms;
            const auto aim=bone_normalize(Quaternion{.13f,-.22f,float(checks%13)*.027f,.93f});
            frame.pose.orientation=aim;
            mesh.control(graph.hero.data(),tracking,frame);
            mesh.update_transform(graph.body.skeleton);
            const auto aimed_world=skeleton_field<std::array<float,16>>(graph.camera.component.data(),0xc0);
            for (unsigned i=0;i<16;++i)
                require(std::abs(aimed_world[i]-world[i])<.0001f,
                    "Independent aim pivoted the camera inside an unchanged native holding hand");
            require(close(bone_unit(skeleton_vector(aimed_world,{1,0,0})),
                bone_unit(skeleton_vector(hand_world,scale(anchor.z,-1))),.001f),
                "Camera points backward instead of along the immutable grip -Z");
            require(close(bone_unit(skeleton_vector(aimed_world,{0,0,1})),
                bone_unit(skeleton_vector(hand_world,scale(cross(anchor.z,anchor.x),-1))),.001f),
                "Camera rolls upside down relative to immutable grip +Y");
            const auto authored_contact=mesh.mesh_grip_point();
            require(std::abs(authored_contact.x+.812240687f)<.00001f &&
                std::abs(authored_contact.y-(hand?4.82349342f:-4.82349342f))<.00001f &&
                std::abs(authored_contact.z+5.27160731f)<.00001f,
                "Camera reused the unraised reference-pose contact or the wrong holding side");
            require(std::memcmp(graph.body.skeleton.atoms.data(),original_atoms.data(),
                original_atoms.size()*sizeof(BoneAtom))==0,
                "Aiming or grabbing the native camera modified the tracked hand");
            const auto aimed_contact=add(skeleton_vector(aimed_world,mesh.mesh_grip_point()),
                {aimed_world[12],aimed_world[13],aimed_world[14]});
            require(close(aimed_contact,expected,.002f),"Camera aim rotated around a point outside the palm");
            MotionFrame late{};late.focused=true;
            late.hands[unsigned(hand)].grip_tracked=true;
            late.hands[unsigned(hand)].aim_tracked=true;
            const auto latest=bone_normalize(Quaternion{-.2f,.3f,.1f,.9f});
            late.hands[unsigned(hand)].aim.orientation={latest.x,latest.y,latest.z,latest.w};
            mesh.update_held_tracking(tracking,late);
            mesh.update_transform(graph.body.skeleton);
            const auto late_world=skeleton_field<std::array<float,16>>(graph.camera.component.data(),0xc0);
            for (unsigned i=0;i<16;++i)
                require(std::abs(late_world[i]-world[i])<.0001f,
                    "Late pointing-ray motion overrode the fixed native camera grip");
        };
        for (int hand:{0,1,0,1}) check_held(hand);
        {
            const auto saved=graph.body.skeleton.atoms;
            for (unsigned hand=0;hand<2;++hand)
                for (float yaw:{-2.7f,-1.2f,0.f,.8f,2.3f})
                    for (float pitch:{-.9f,0.f,.7f})
                        for (float roll:{-1.1f,0.f,1.4f}) {
                            const auto controller=bone_normalize(bone_multiply(
                                {0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)},bone_multiply(
                                {std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)},
                                {0,0,std::sin(roll*.5f),std::cos(roll*.5f)})));
                            const auto wrist=body_roles[hand?13:9];
                            const auto anchor=outlast_grip_anchor(graph.body.skeleton,wrist);
                            graph.body.skeleton.atoms[std::size_t(wrist)].rotation=
                                outlast_grip_wrist_rotation(graph.body.skeleton,tracking,controller,anchor);
                            check_held(int(hand));
                            const auto world=skeleton_field<std::array<float,16>>(graph.camera.component.data(),0xc0);
                            require(close(bone_unit(skeleton_vector(world,{1,0,0})),
                                map_tracking(tracking,rotate(controller,{0,0,-1})),.001f),
                                "Real camera optical forward disagrees with its physical grip controller");
                            require(close(bone_unit(skeleton_vector(world,{0,0,1})),
                                map_tracking(tracking,rotate(controller,{0,1,0})),.001f),
                                "Real camera optical up disagrees with its physical grip controller");
                        }
            graph.body.skeleton.atoms=saved;
        }
        for (auto* component:{graph.camera.component.data(),graph.shadow.component.data()})
            write(component,0x78,static_cast<void*>(nullptr));
        mesh.control(graph.hero.data(),tracking,frame);
        require(skeleton_field<void*>(graph.camera.component.data(),0x78)==graph.hero.data() &&
            skeleton_field<void*>(graph.shadow.component.data(),0x78)==graph.hero.data(),
            "Native visibility detach was not restored through the admitted engine update");
        write(graph.camera.component.data(),0x78,graph.native_shadow.data());
        bool foreign_owner=false;
        try {mesh.control(graph.hero.data(),tracking,frame);}
        catch (const std::runtime_error&) {foreign_owner=true;}
        require(foreign_owner,"Retail camera control stole a component owned by another actor");
        write(graph.camera.component.data(),0x78,graph.hero.data());
        unsigned trajectory_poses=0;
        if (argc==3) {
            std::ifstream input(argv[2],std::ios::binary);
            UINT64 magic=0;UINT count=0;
            input.read(reinterpret_cast<char*>(&magic),8);
            input.read(reinterpret_cast<char*>(&count),4);
            input.read(reinterpret_cast<char*>(&trajectory_poses),4);
            require(magic==0x363130524D4143ULL && count==73 && trajectory_poses>=32 && trajectory_poses<=2048,
                "Invalid retained native camera animation trajectory");
            for (unsigned pose=0;pose<trajectory_poses;++pose) {
                input.read(reinterpret_cast<char*>(graph.body.skeleton.atoms.data()),count*sizeof(BoneAtom));
                require(bool(input),"Truncated retained native camera animation trajectory");
                check_held(1);check_held(0);
            }
            require(input.peek()==std::char_traits<char>::eof(),"Unexpected trailing camera trajectory data");
        }
        graph.visibility_observer=&mesh;
        mesh.release_control();
        graph.visibility_observer=nullptr;
        require(skeleton_field<INT>(graph.body.component.data(),0x3f4)==1 &&
            skeleton_field<INT>(graph.native_shadow.data(),0x3f4)==1,"Native camera graph was not restored");
        require(graph.visibility_callbacks>=2,"Restoration did not exercise reentrant native visibility callbacks");
        require(skeleton_field<UINT>(graph.camera.component.data(),0x178)==0x100004,
            "Native visibility was not restored on relinquishing controls");
        mesh.reset();
        mesh.control(graph.hero.data(),tracking,frame);
        require(mesh.hand()==frame.hand,"Restored native graph could not bind again after GC/reset");
        mesh.release_control();
        mesh.control(graph.hero.data(),tracking,frame);
        write(graph.camera.component.data(),0x178,UINT(4));
        mesh.observe_stock_visibility(graph.hero.data());
        require(!(skeleton_field<UINT>(graph.camera.component.data(),0x178)&(4|0x100000)),
            "A stock visibility notification hid the currently controlled retail camera");
        mesh.control(graph.hero.data(),tracking,frame);
        mesh.release_control();
        require(skeleton_field<UINT>(graph.camera.component.data(),0x178)==4,
            "A newer native visibility request was overwritten by startup flags");
        mesh.reset();
        require(!mesh.pose(tracking) && !mesh.component(),"GC/reset retained a native camera pointer or pose");
        write(graph.camera.asset.data(),0x48,name(L"not_the_retail_camera"));
        bool rejected=false;
        try {mesh.control(graph.hero.data(),tracking,frame);} catch (const std::runtime_error&) {rejected=true;}
        require(rejected,"A substitute mesh was admitted as the real retail camera");
        std::cout<<"PASS: native_camera_checks="<<checks
            <<"; native_animation_poses="<<trajectory_poses
            <<"; real_asset_admission; actual_lens_LCD_geometry; 45_rigid_poses; both_hands;"
              " attachment_reuse; reentrant_visibility_restoration; GC_rebind; GC_reset; substitute_rejected\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
