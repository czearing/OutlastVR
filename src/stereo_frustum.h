#pragma once
#include "xr_view_constants.h"
#include <limits>

namespace olvr {

struct StereoPlane { Vec3 normal; float distance; };

class StereoFrustum {
    std::array<std::array<StereoPlane,6>,2> planes_{};
public:
    explicit StereoFrustum(const StereoViewConstants& views) {
        for (unsigned eye=0;eye<2;++eye) {
            const auto& view=views[eye];
            float world[16]{};
            for (unsigned row=0;row<4;++row) {
                std::copy_n(view.begin()+row*4,4,world+row*4);
                world[row*4+3]+=view[row*4]*view[20]+view[row*4+1]*view[21]+view[row*4+2]*view[22];
            }
            for (unsigned index=0;index<6;++index) {
                float row[4]{};
                for (unsigned column=0;column<4;++column) {
                    if (index==4) row[column]=-world[8+column];
                    else {
                        const unsigned axis=index<2?0:index<4?1:2;
                        const bool lower=index==0 || index==2;
                        row[column]=(lower?-world[axis*4+column]:world[axis*4+column])-world[12+column];
                    }
                }
                const float length=std::sqrt(row[0]*row[0]+row[1]*row[1]+row[2]*row[2]);
                if (!std::isfinite(length) || !std::isfinite(row[3]))
                    throw std::invalid_argument("Nonfinite tracked culling plane");
                if (length<.000001f) {
                    if (index!=5 || row[3]>0)
                        throw std::invalid_argument("Degenerate tracked culling plane");
                    planes_[eye][index]={{0,0,0},1};
                } else planes_[eye][index]={{row[0]/length,row[1]/length,row[2]/length},-row[3]/length};
            }
        }
    }
    const std::array<StereoPlane,6>& planes(unsigned eye) const {
        if (eye>1) throw std::invalid_argument("Invalid culling eye");
        return planes_[eye];
    }
    bool sphere(Vec3 center,float radius) const {
        if (radius<0)
            throw std::invalid_argument("Invalid native culling sphere");
        return sphere_margin(center,radius);
    }
    bool sphere_margin(Vec3 center,float radius) const {
        if (!finite(center) || !std::isfinite(radius))
            throw std::invalid_argument("Invalid native culling sphere");
        return native_sphere_margin(center,radius);
    }
    bool native_sphere_margin(Vec3 center,float radius,bool* fully_inside=nullptr) const {
        // Retail uses ordered greater-than rejection, including for NaN bounds.
        // Apply rounding slack only to finite inputs; infinity must not inflate it.
        const bool finite_bound=finite(center) && std::isfinite(radius);
        bool any_visible=false,any_inside=false;
        for (const auto& eye:planes_) {
            bool visible=true,inside=true;
            for (const auto& plane:eye) {
                const float value=dot(plane.normal,center)-plane.distance;
                const float error=finite_bound ? .002f+8*std::numeric_limits<float>::epsilon()*
                    (std::abs(plane.normal.x*center.x)+std::abs(plane.normal.y*center.y)+
                     std::abs(plane.normal.z*center.z)+std::abs(plane.distance)) : 0;
                if (value>radius+error) {visible=false;inside=false;break;}
                if (fully_inside && value>-radius-error) inside=false;
            }
            if (visible && !fully_inside) return true;
            any_visible |= visible;any_inside |= inside;
        }
        if (fully_inside) *fully_inside=any_inside;
        return any_visible;
    }
    bool box(Vec3 center,Vec3 extent,bool* fully_inside=nullptr) const {
        if (!finite(center) || !finite(extent))
            throw std::invalid_argument("Invalid native culling box");
        extent={std::abs(extent.x),std::abs(extent.y),std::abs(extent.z)};
        bool any_visible=false,any_inside=false;
        for (const auto& eye:planes_) {
            bool visible=true,inside=true;
            for (const auto& plane:eye) {
                const float radius=std::abs(plane.normal.x)*extent.x+
                    std::abs(plane.normal.y)*extent.y+std::abs(plane.normal.z)*extent.z;
                const float value=dot(plane.normal,center)-plane.distance;
                const float error=.002f+8*std::numeric_limits<float>::epsilon()*
                    (std::abs(plane.normal.x*center.x)+std::abs(plane.normal.y*center.y)+
                     std::abs(plane.normal.z*center.z)+std::abs(plane.distance));
                if (value>radius+error) {visible=false;inside=false;break;}
                if (value>-radius-error) inside=false;
            }
            any_visible |= visible;any_inside |= inside;
        }
        if (fully_inside) *fully_inside=any_inside;
        return any_visible;
    }
};

}
