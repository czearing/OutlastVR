#pragma once
#include "xr_view_constants.h"
#include <algorithm>
#include <limits>

namespace olvr {

using Matrix4=std::array<float,16>;

inline Matrix4 matrix_product(const Matrix4& left,const Matrix4& right) {
    Matrix4 result{};
    for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column) {
        double value=0;
        for (unsigned index=0;index<4;++index)
            value+=double(left[row*4+index])*right[index*4+column];
        if (!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())
            throw std::overflow_error("Stereo reconstruction matrix product is nonfinite");
        result[row*4+column]=static_cast<float>(value);
    }
    return result;
}

inline Matrix4 matrix_inverse(const Matrix4& input) {
    double rows[4][8]{};
    for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column) {
        if (!std::isfinite(input[row*4+column]))
            throw std::invalid_argument("Cannot invert a nonfinite reconstruction matrix");
        rows[row][column]=input[row*4+column];
        rows[row][column+4]=row==column;
    }
    for (unsigned column=0;column<4;++column) {
        unsigned pivot=column;
        for (unsigned row=column+1;row<4;++row)
            if (std::abs(rows[row][column])>std::abs(rows[pivot][column])) pivot=row;
        if (std::abs(rows[pivot][column])<1e-12)
            throw std::invalid_argument("Stereo reconstruction matrix is singular");
        for (unsigned index=0;index<8;++index) std::swap(rows[pivot][index],rows[column][index]);
        const auto denominator=rows[column][column];
        for (auto& value:rows[column]) value/=denominator;
        for (unsigned row=0;row<4;++row) if (row!=column) {
            const auto factor=rows[row][column];
            for (unsigned index=0;index<8;++index) rows[row][index]-=factor*rows[column][index];
        }
    }
    Matrix4 result{};
    for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column) {
        const auto value=rows[row][column+4];
        if (!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())
            throw std::overflow_error("Stereo reconstruction inverse is nonfinite");
        result[row*4+column]=static_cast<float>(value);
    }
    return result;
}

inline Matrix4 identity_matrix() {
    return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
}

inline Matrix4 translation_matrix(Vec3 offset) {
    if (!finite(offset)) throw std::invalid_argument("Nonfinite reconstruction pretranslation");
    auto matrix=identity_matrix();
    matrix[3]=offset.x;matrix[7]=offset.y;matrix[11]=offset.z;
    return matrix;
}

struct StereoReconstructionContext {
    Matrix4 original_projection;
    TrackingFrame original_camera;
};

inline RECT stereo_atlas_scissor(const std::array<RECT,2>& eyes,unsigned eye_width,unsigned height) {
    if (!eye_width || eye_width>unsigned(std::numeric_limits<LONG>::max())/2 ||
        !height || height>unsigned(std::numeric_limits<LONG>::max()))
        throw std::invalid_argument("Invalid hardware stereo scissor extent");
    const LONG width=static_cast<LONG>(eye_width),bottom=static_cast<LONG>(height);
    RECT result{};
    bool visible=false;
    for (unsigned eye=0;eye<2;++eye) {
        const auto& source=eyes[eye];
        if (source.left>source.right || source.top>source.bottom)
            throw std::invalid_argument("Invalid hardware stereo scissor bounds");
        RECT clipped{std::clamp(source.left,0L,width),std::clamp(source.top,0L,bottom),
            std::clamp(source.right,0L,width),std::clamp(source.bottom,0L,bottom)};
        if (clipped.left==clipped.right || clipped.top==clipped.bottom) continue;
        clipped.left+=static_cast<LONG>(eye)*width;
        clipped.right+=static_cast<LONG>(eye)*width;
        if (!visible) result=clipped;
        else {
            result.left=std::min(result.left,clipped.left);
            result.top=std::min(result.top,clipped.top);
            result.right=std::max(result.right,clipped.right);
            result.bottom=std::max(result.bottom,clipped.bottom);
        }
        visible=true;
    }
    return result;
}

inline bool screen_reconstruction_translated(const StereoReconstructionContext& context,
                                             const Matrix4& matrix) {
    const auto origin=context.original_camera.origin;
    const auto offset=context.original_camera.pretranslation;
    const float world[]{origin.x,origin.y,origin.z},shift[]{offset.x,offset.y,offset.z};
    bool absolute=true,translated=true;
    for (unsigned axis=0;axis<3;++axis) {
        const auto value=matrix[axis*4+3];
        const float tolerance=.002f+32*std::numeric_limits<float>::epsilon()*
            (std::abs(world[axis])+std::abs(shift[axis])+std::abs(value));
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite native screen reconstruction origin");
        absolute &= std::abs(value-world[axis])<=tolerance;
        translated &= std::abs(value-(world[axis]+shift[axis]))<=tolerance;
    }
    if (!absolute && !translated)
        throw std::invalid_argument("Native screen reconstruction origin matches neither world coordinate system");
    return translated && !absolute;
}

inline Matrix4 reconstruction_delta(const StereoReconstructionContext& original,
                                     const std::array<float,32>& eye,bool translated=false) {
    Matrix4 projection{};
    std::copy_n(eye.begin(),16,projection.begin());
    auto delta=matrix_product(matrix_inverse(projection),original.original_projection);
    if (!translated) {
        const auto offset=original.original_camera.pretranslation;
        delta=matrix_product(translation_matrix(scale(offset,-1)),
            matrix_product(delta,translation_matrix(offset)));
    }
    return delta;
}

inline Matrix4 screen_input_delta(const StereoReconstructionContext& original,
                                  const std::array<float,32>& eye) {
    const auto& projection=original.original_projection;
    const Vec3 forward{projection[12],projection[13],projection[14]};
    const auto length=dot(forward,forward);
    if (!std::isfinite(length) || length<.000001f)
        throw std::invalid_argument("Native linear-depth projection is degenerate");
    auto native=projection;
    Matrix4 tracked{};
    std::copy_n(eye.begin(),16,tracked.begin());
    // Linear screen depth is clip.w, not clip.z. Keeping this affine avoids
    // near-plane reciprocal cancellation in distant shadow coordinates.
    for (auto* matrix:{&native,&tracked}) {
        for (const auto value:*matrix)
            if (!std::isfinite(value))
                throw std::invalid_argument("Nonfinite native linear-depth projection");
        std::copy_n(matrix->begin()+12,4,matrix->begin()+8);
        std::fill_n(matrix->begin()+12,3,0);
        (*matrix)[15]=1;
    }
    return matrix_product(native,matrix_inverse(tracked));
}

inline std::array<RECT,2> stereo_world_scissors(const StereoReconstructionContext& context,
    const std::array<Matrix4,2>& eye_to_native,RECT source,RECT viewport) {
    if (viewport.left>=viewport.right || viewport.top>=viewport.bottom ||
        source.left>source.right || source.top>source.bottom)
        throw std::invalid_argument("Invalid native world scissor or logical viewport");
    source.left=std::max(source.left,viewport.left);
    source.top=std::max(source.top,viewport.top);
    source.right=std::min(source.right,viewport.right);
    source.bottom=std::min(source.bottom,viewport.bottom);
    if (source.left>=source.right || source.top>=source.bottom)
        return {RECT{source.left,source.top,source.left,source.top},
            RECT{source.left,source.top,source.left,source.top}};
    if (source.left==viewport.left && source.right==viewport.right &&
        source.top==viewport.top && source.bottom==viewport.bottom) return {viewport,viewport};
    const auto& projection=context.original_projection;
    const Vec3 forward{projection[12],projection[13],projection[14]};
    const Vec3 depth{projection[8],projection[9],projection[10]};
    const auto length=dot(forward,forward);
    if (!std::isfinite(length) || length<.000001f)
        throw std::invalid_argument("World scissor requires a native perspective projection");
    const double scale=dot(depth,forward)/length;
    const double near_plane=-(projection[11]-scale*projection[15])/scale;
    if (!std::isfinite(near_plane) || near_plane<=0)
        throw std::invalid_argument("World scissor requires a positive native near plane");
    const double width=double(viewport.right)-viewport.left,height=double(viewport.bottom)-viewport.top;
    std::array<RECT,2> result{};
    for (unsigned eye=0;eye<2;++eye) {
        const auto& transform=eye_to_native[eye];
        double left=viewport.right,right=viewport.left,top=viewport.bottom,bottom=viewport.top;
        for (const auto value:transform)
            if (!std::isfinite(value))
                throw std::invalid_argument("Nonfinite tracked world scissor transform");
        using Plane=std::array<double,4>;
        std::array<Plane,5> planes{};
        unsigned count=0;
        const auto bound=[&](unsigned axis,double limit,double sign) {
            auto& plane=planes[count++];
            for (unsigned column=0;column<4;++column)
                plane[column]=sign*(transform[axis*4+column]-limit*transform[8+column]);
        };
        // Viewport-truncated sides are open, not false bounds on offscreen light.
        if (source.left>viewport.left) bound(0,(source.left-viewport.left)*2/width-1,1);
        if (source.right<viewport.right) bound(0,(source.right-viewport.left)*2/width-1,-1);
        if (source.bottom<viewport.bottom) bound(1,1-(source.bottom-viewport.top)*2/height,1);
        if (source.top>viewport.top) bound(1,1-(source.top-viewport.top)*2/height,-1);
        auto& depth_plane=planes[count++];
        for (unsigned column=0;column<4;++column) depth_plane[column]=transform[8+column];
        depth_plane[3]-=near_plane;
        struct Point {double x,y;};
        std::array<Point,32> polygon{};
        polygon[0]={-1,-1};polygon[1]={1,-1};polygon[2]={1,1};polygon[3]={-1,1};
        unsigned vertices=4;
        const auto clip=[&](double a,double b,double c) {
            const auto magnitude=std::max({std::abs(a),std::abs(b),std::abs(c)});
            if (!std::isfinite(magnitude))
                throw std::overflow_error("Nonfinite projected world-scissor half-plane");
            if (!vertices || !magnitude) return;
            a/=magnitude;b/=magnitude;c=c/magnitude+1e-10;
            std::array<Point,32> output{};
            unsigned size=0;
            const auto append=[&](Point point) {
                if (size==output.size()) throw std::overflow_error("World-scissor polygon exceeded its bound");
                output[size++]=point;
            };
            auto previous=polygon[vertices-1];
            double previous_distance=a*previous.x+b*previous.y+c;
            for (unsigned index=0;index<vertices;++index) {
                const auto current=polygon[index];
                const double distance=a*current.x+b*current.y+c;
                if ((distance>=0)!=(previous_distance>=0)) {
                    const auto fraction=previous_distance/(previous_distance-distance);
                    append({previous.x+(current.x-previous.x)*fraction,
                        previous.y+(current.y-previous.y)*fraction});
                }
                if (distance>=0) append(current);
                previous=current;previous_distance=distance;
            }
            polygon=output;vertices=size;
        };
        // With reciprocal eye depth u>=0, each 3D plane becomes
        // a*x+b*y+c+d*u>=0. Eliminate u before clipping the eye's 2D viewport.
        for (unsigned index=0;index<count;++index) {
            const auto& plane=planes[index];
            if (plane[3]<=0) clip(plane[0],plane[1],plane[2]);
            if (plane[3]<0) for (unsigned other=0;other<count;++other) {
                const auto& lower=planes[other];
                if (lower[3]>0) clip(lower[3]*plane[0]-plane[3]*lower[0],
                    lower[3]*plane[1]-plane[3]*lower[1],
                    lower[3]*plane[2]-plane[3]*lower[2]);
            }
        }
        for (unsigned index=0;index<vertices;++index) {
            const auto point=polygon[index];
            const auto px=(point.x+1)*width*.5+viewport.left;
            const auto py=(1-point.y)*height*.5+viewport.top;
            left=std::min(left,px);right=std::max(right,px);
            top=std::min(top,py);bottom=std::max(bottom,py);
        }
        if (left>right || top>bottom) result[eye]={viewport.left,viewport.top,viewport.left,viewport.top};
        else {
            const auto horizontal=[&](double value) {
                return static_cast<LONG>(std::clamp(value,double(viewport.left),double(viewport.right)));
            };
            const auto vertical=[&](double value) {
                return static_cast<LONG>(std::clamp(value,double(viewport.top),double(viewport.bottom)));
            };
            // Round outward for native half-pixel raster conventions and float uploads.
            result[eye]={horizontal(std::floor(left)-1),vertical(std::floor(top)-1),
                horizontal(std::ceil(right)+1),vertical(std::ceil(bottom)+1)};
        }
    }
    return result;
}

inline Matrix4 reconstruction_rotation(const StereoReconstructionContext& original,
                                        const std::array<float,32>& eye) {
    const auto& matrix=eye;
    Vec3 forward{matrix[12],matrix[13],matrix[14]};
    const auto length=std::sqrt(dot(forward,forward));
    if (!std::isfinite(length) || length<.000001f)
        throw std::invalid_argument("Tracked reconstruction perspective is invalid");
    forward=scale(forward,1/length);
    auto axis=[&](unsigned row) {
        Vec3 value{matrix[row*4],matrix[row*4+1],matrix[row*4+2]};
        value=add(value,scale(forward,-dot(value,forward)));
        const auto size=std::sqrt(dot(value,value));
        if (!std::isfinite(size) || size<.000001f)
            throw std::invalid_argument("Tracked reconstruction rotation is degenerate");
        return scale(value,1/size);
    };
    const Vec3 tracked[]{axis(0),axis(1),scale(forward,-1)};
    const Vec3 native[]{original.original_camera.right,original.original_camera.up,
                        original.original_camera.backward};
    auto component=[](Vec3 value,unsigned index) {
        return index==0?value.x:index==1?value.y:value.z;
    };
    auto result=identity_matrix();
    for (unsigned row=0;row<3;++row) for (unsigned column=0;column<3;++column) {
        float value=0;
        for (unsigned index=0;index<3;++index)
            value+=component(tracked[index],row)*component(native[index],column);
        result[row*4+column]=value;
    }
    return result;
}

}
