#pragma once
#include "xr_view_constants.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

namespace olvr {

struct OutlastLocomotionSample {
    std::uint64_t serial=0;
    std::uintptr_t camera=0,hero=0;
    Vec3 root{},bone{};
    std::int32_t yaw=0;
    unsigned basic=0,special=0,mode=0,flags=0;
    bool crouched=false,pawn_crouched=false,lean_observed=false;
    float lean_intent=0,lean_weight=0;
};

struct OutlastLocomotionView {
    Vec3 origin{};
    std::uint64_t source_serial=0;
    bool stabilized=false;
    std::uint64_t completed_serial=0;
    OutlastLocomotionSample native{};
};

inline bool outlast_airborne_locomotion(unsigned basic,unsigned special) {
    return basic==2 && special==3;
}
inline bool outlast_tracked_locomotion(unsigned basic,unsigned special) {
    return ((basic==0 || basic==1) && special<=2) || outlast_airborne_locomotion(basic,special);
}

class OutlastLocomotionCamera {
    struct Completed {
        Vec3 final_origin,correction;
        std::uint64_t serial;
        bool stabilized;
        OutlastLocomotionSample sample;
    };
    std::deque<Completed> completed_;
    std::uintptr_t camera_=0,hero_=0;
    bool anchored_=false,crouched_=false;
    Vec3 local_anchor_{};
    std::optional<Vec3> standing_anchor_;
    std::optional<OutlastLocomotionSample> initial_standing_;

    static Vec3 yaw_rotate(Vec3 value,std::int32_t yaw,bool inverse) {
        const double angle=double(std::uint32_t(yaw)&0xffffu)*(6.2831853071795864769/65536)*
            (inverse?-1:1);
        const double c=std::cos(angle),s=std::sin(angle);
        return {float(c*value.x-s*value.y),float(s*value.x+c*value.y),value.z};
    }
public:
    void complete(const OutlastLocomotionSample& sample,Vec3 final_origin) {
        if (!sample.serial || !sample.camera || !sample.hero || !finite(sample.root) ||
            !finite(sample.bone) || !finite(final_origin) || !std::isfinite(sample.lean_intent) ||
            !std::isfinite(sample.lean_weight))
            throw std::invalid_argument("Invalid completed native locomotion camera");
        if (sample.camera!=camera_ || sample.hero!=hero_) {
            anchored_=false;
            standing_anchor_.reset();
            camera_=sample.camera;hero_=sample.hero;
        }
        // The animation node's release tail is still intentional leaning.
        const bool airborne=outlast_airborne_locomotion(sample.basic,sample.special);
        const bool normal=(sample.basic==0 || sample.basic==1) && sample.special==0 && sample.mode==0 &&
            !(sample.flags&4) && sample.lean_intent==0;
        const bool neutral_lean=sample.lean_observed &&
            std::abs(sample.lean_weight)<=8*std::numeric_limits<float>::epsilon();
        const bool airborne_anchor=airborne && anchored_ && sample.camera==camera_ && sample.hero==hero_ &&
            !(sample.flags&4) && sample.lean_intent==0 && neutral_lean;
        const bool supported=(normal && neutral_lean) || airborne_anchor;
        Vec3 correction{};
        if (supported) {
            if (!anchored_ || sample.camera!=camera_ || sample.hero!=hero_ ||
                (!airborne && sample.crouched!=crouched_)) {
                const auto& reference=initial_standing_ && !sample.crouched &&
                    initial_standing_->camera==sample.camera && initial_standing_->hero==sample.hero?
                    *initial_standing_:sample;
                local_anchor_=!sample.crouched && standing_anchor_?*standing_anchor_:
                    yaw_rotate(add(reference.bone,scale(reference.root,-1)),reference.yaw,true);
                camera_=sample.camera;hero_=sample.hero;crouched_=sample.crouched;anchored_=true;
            }
            if (normal && !sample.crouched && !standing_anchor_) standing_anchor_=local_anchor_;
            initial_standing_.reset();
            const auto neutral=add(sample.root,yaw_rotate(local_anchor_,sample.yaw,false));
            correction=add(neutral,scale(sample.bone,-1));
        } else {
            anchored_=false;
            if (normal && !sample.crouched && !sample.lean_observed) {
                // Keep the pre-animation reference while spawn physics settles the root.
                // Never apply it until the actual native lean blend is observed neutral.
                if (!initial_standing_ || initial_standing_->camera!=sample.camera ||
                    initial_standing_->hero!=sample.hero) initial_standing_=sample;
            } else initial_standing_.reset();
        }
        completed_.push_back({final_origin,correction,sample.serial,supported,sample});
        if (completed_.size()>64) completed_.pop_front();
    }

    std::optional<OutlastLocomotionView> locate(Vec3 native_origin) const {
        if (!finite(native_origin))
            throw std::invalid_argument("Nonfinite native locomotion view");
        const Completed* closest=nullptr;
        double closest_distance=std::numeric_limits<double>::infinity();
        for (auto it=completed_.rbegin();it!=completed_.rend();++it) {
            const float actual[]{native_origin.x,native_origin.y,native_origin.z};
            const float expected[]{it->final_origin.x,it->final_origin.y,it->final_origin.z};
            // The inverse view transform couples all three world coordinates.
            // A small output axis can inherit roundoff from a large input axis.
            const double magnitude=std::max({1.,
                std::abs(double(actual[0]))+std::abs(double(actual[1]))+std::abs(double(actual[2])),
                std::abs(double(expected[0]))+std::abs(double(expected[1]))+std::abs(double(expected[2]))});
            const double epsilon=std::numeric_limits<float>::epsilon();
            const double tolerance=(16*epsilon/(1-16*epsilon))*magnitude;
            bool matches=true;
            double distance=0;
            for (unsigned axis=0;axis<3;++axis) {
                const double difference=double(actual[axis])-expected[axis];
                if (std::abs(difference)>tolerance) matches=false;
                distance+=difference*difference;
            }
            if (matches && distance<closest_distance) {closest=&*it;closest_distance=distance;}
        }
        if (!closest) return std::nullopt;
        return OutlastLocomotionView{add(native_origin,closest->correction),
                                     closest->serial,closest->stabilized,completed_.back().serial,closest->sample};
    }
};

}
