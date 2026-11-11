#include "outlast_locomotion_camera.h"
#include <iostream>

using namespace olvr;

int main(int argc,char**) {
    try {
        const bool fault=argc>1;
        unsigned checks=0;
        auto require=[&](bool condition,const char* message) {
            ++checks;if (!condition) throw std::runtime_error(message);
        };
        auto equal=[&](Vec3 actual,Vec3 expected) {
            require(std::abs(actual.x-expected.x)<.003f && std::abs(actual.y-expected.y)<.003f &&
                std::abs(actual.z-expected.z)<.003f,"Native locomotion translation contract changed");
        };
        OutlastLocomotionSample sample{};
        sample.serial=1;sample.camera=1;sample.hero=2;sample.flags=2;sample.lean_observed=true;
        sample.root={-7500,-600,550};sample.bone={-7520,-602,718};
        OutlastLocomotionCamera camera;
        camera.complete(sample,sample.bone);
        const auto anchor=sample.bone;
        for (unsigned frame=1;frame<=120;++frame) {
            ++sample.serial;
            sample.root={-7500-float(frame)*4,-600+float(frame),550+float(frame)/12};
            sample.bone=add(sample.root,{-20+2*std::sin(float(frame)),-2+std::cos(float(frame)),
                                        168+1.5f*std::sin(float(frame)*.7f)});
            const auto post=Vec3{.5f,-.25f,.125f};
            const auto final=add(sample.bone,post);
            camera.complete(sample,final);
            auto view=camera.locate(final);
            require(view && view->stabilized && view->source_serial==sample.serial,
                "Completed native locomotion pose was not linked");
            const Vec3 expected=add(add(anchor,{-float(frame)*4,float(frame),float(frame)/12}),post);
            equal(fault?final:view->origin,expected);
        }
        const auto older=sample;
        const auto older_final=add(sample.bone,{.5f,-.25f,.125f});
        ++sample.serial;sample.bone=add(sample.bone,{3,4,2});
        camera.complete(sample,sample.bone);
        require(camera.locate(older_final)->source_serial==older.serial,
            "Render thread consumed the newer animation sample instead of its actual completed pose");
        require(camera.locate(older_final)->completed_serial==sample.serial,
            "Completed camera history watermark did not describe the actual lookup");
        require(!camera.locate({100,200,300}),"Unobserved native camera pose was guessed");

        for (unsigned state=0;state<6;++state) {
            auto unsupported=sample;
            unsupported.serial=++sample.serial;
            if (state==0) unsupported.special=1;
            if (state==1) unsupported.basic=9;
            if (state==2) unsupported.mode=3;
            if (state==3) unsupported.flags|=4;
            if (state==4) unsupported.lean_intent=-1;
            if (state==5) unsupported.lean_observed=false;
            camera.complete(unsupported,unsupported.bone);
            const auto view=camera.locate(unsupported.bone);
            require(view && !view->stabilized,"Intentional native camera state was suppressed");
            equal(view->origin,unsupported.bone);
        }
        {
            OutlastLocomotionCamera jumping;
            auto ground=sample;ground.root={0,0,10};ground.bone={-20,-2,178};
            jumping.complete(ground,ground.bone);
            for (unsigned frame=0;frame<120;++frame) {
                auto moving=ground;moving.serial=ground.serial+frame+1;
                moving.basic=frame<30?1:2;moving.special=frame<30?0:3;
                moving.mode=frame<30?0:std::numeric_limits<unsigned>::max();
                moving.root={float(frame)*3,0,10+50*std::sin(float(frame)*.03f)};
                moving.bone=add(moving.root,{float(frame%4)-20,-2,120+float(frame%9)});
                jumping.complete(moving,moving.bone);
                const auto view=jumping.locate(moving.bone);
                require(view && view->stabilized,"Running/jumping dropped the anchored headset camera");
                equal(view->origin,add(moving.root,{-20,-2,168}));
                require(outlast_tracked_locomotion(moving.basic,moving.special),
                    "Free running/jumping lost tracked body ownership");
            }
            require(!outlast_tracked_locomotion(9,0) && !outlast_tracked_locomotion(2,4) &&
                !outlast_tracked_locomotion(0,3),"Tracked locomotion overrode an unrelated scripted state");
            OutlastLocomotionCamera unanchored;
            ground.basic=2;ground.special=3;
            unanchored.complete(ground,ground.bone);
            require(!unanchored.locate(ground.bone)->stabilized,"Airborne camera invented an unmeasured standing anchor");
        }
        for (const float tail:{-1.f,-.1f,-.00001f,1.f,.1f,.00001f}) {
            auto lean=sample;lean.serial=++sample.serial;lean.lean_weight=tail;
            camera.complete(lean,lean.bone);
            const auto view=camera.locate(lean.bone);
            require(view && !view->stabilized,"Native lean release tail was removed");
            equal(view->origin,lean.bone);
        }
        sample.special=0;sample.crouched=true;++sample.serial;
        sample.bone=add(sample.root,{-19,-2,60});
        camera.complete(sample,sample.bone);
        equal(camera.locate(sample.bone)->origin,sample.bone);
        ++sample.serial;sample.bone.z+=.5f;
        camera.complete(sample,sample.bone);
        equal(camera.locate(sample.bone)->origin,add(sample.root,{-19,-2,60}));
        sample.crouched=false;++sample.serial;sample.bone=add(sample.root,{-20,-2,168});
        camera.complete(sample,sample.bone);
        equal(camera.locate(sample.bone)->origin,sample.bone);

        for (const std::int32_t yaw:{0,16384,32768,49152,-16384,65536}) {
            OutlastLocomotionCamera turning;
            OutlastLocomotionSample start=sample;start.root={0,0,0};start.bone={10,0,168};start.yaw=0;
            turning.complete(start,start.bone);
            const double radians=double(yaw)*6.2831853071795864769/65536;
            const Vec3 neutral{float(10*std::cos(radians)),float(10*std::sin(radians)),168};
            auto next=start;++next.serial;next.yaw=yaw;next.bone=add(neutral,{1,2,3});
            turning.complete(next,next.bone);
            equal(turning.locate(next.bone)->origin,neutral);
        }
        {
            OutlastLocomotionCamera spawning;
            auto pending=sample;pending.crouched=false;pending.lean_observed=false;
            pending.root={0,0,100};pending.bone={-20,-2,268};
            spawning.complete(pending,pending.bone);
            require(!spawning.locate(pending.bone)->stabilized,
                "Unobserved native lean state was treated as neutral");
            auto settled=pending;++settled.serial;settled.lean_observed=true;
            settled.root.z-=9;settled.bone.z-=2;
            spawning.complete(settled,settled.bone);
            equal(spawning.locate(settled.bone)->origin,{-20,-2,259});
        }
        {
            OutlastLocomotionCamera spawning;
            auto pending=sample;pending.crouched=false;pending.lean_observed=false;
            spawning.complete(pending,pending.bone);
            auto leaning=pending;++leaning.serial;leaning.lean_observed=true;leaning.lean_weight=-1;
            leaning.bone.y+=45;
            spawning.complete(leaning,leaning.bone);
            equal(spawning.locate(leaning.bone)->origin,leaning.bone);
            ++leaning.serial;leaning.lean_weight=0;leaning.bone.z-=4;
            spawning.complete(leaning,leaning.bone);
            equal(spawning.locate(leaning.bone)->origin,leaning.bone);
        }
        {
            OutlastLocomotionCamera native_roundoff;
            auto source=sample;source.serial=586;source.special=2;
            source.bone={-8328.93848f,-630.138428f,677.016113f};
            native_roundoff.complete(source,source.bone);
            const Vec3 reconstructed{-8328.9375f,-630.138367f,677.016541f};
            const auto view=native_roundoff.locate(reconstructed);
            require(view && view->source_serial==586 && !view->stabilized,
                "Actual native inverse-view cross-axis roundoff lost its completed source");
            equal(view->origin,reconstructed);
            require(!native_roundoff.locate(add(reconstructed,{0,0,.1f})),
                "A genuinely different native pose passed the roundoff bound");
        }
        ++sample.serial;sample.hero=3;sample.bone.z+=25;
        camera.complete(sample,sample.bone);
        equal(camera.locate(sample.bone)->origin,sample.bone);
        const auto old=sample.bone;
        for (unsigned i=0;i<65;++i) {
            ++sample.serial;sample.bone.x+=1;
            camera.complete(sample,sample.bone);
        }
        require(!camera.locate(old),"Expired native camera history remained selectable");
        bool invalid=false;
        try {sample.bone.x=std::numeric_limits<float>::quiet_NaN();camera.complete(sample,sample.bone);}
        catch (const std::invalid_argument&) {invalid=true;}
        require(invalid,"Invalid native camera sample was accepted");
        std::cout<<"PASS: native_locomotion_checks="<<checks
            <<"; animated_translation_removed; root_travel_terrain_post_offsets_preserved; "
              "exact_completed_pose_link; native_crouch_lean_tail_scripted_states_preserved; running_jump_root_follow\n";
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
