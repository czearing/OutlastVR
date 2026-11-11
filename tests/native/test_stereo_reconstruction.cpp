#include "stereo_reconstruction.h"
#include <iostream>
#include <stdexcept>

using namespace olvr;

static std::array<float,4> apply(const Matrix4& matrix,std::array<float,4> vector) {
    std::array<float,4> result{};
    for (unsigned row=0;row<4;++row) for (unsigned column=0;column<4;++column)
        result[row]+=matrix[row*4+column]*vector[column];
    return result;
}

static void close(float value,float expected) {
    if (!std::isfinite(value) || std::abs(value-expected)>.004f)
        throw std::runtime_error("Tracked inverse reconstruction differs from independent physical eye-ray calculation");
}

int main(int argc,char** argv) {
    try {
        const bool fault=argc==2 && std::string(argv[1])=="--fault-screen-input";
        if (argc>2 || (argc==2 && !fault))
            throw std::invalid_argument("Unknown reconstruction control");
        const TrackingFrame frame{{30,-5,7},{0,1,0},{0,0,1},{-1,0,0},100,{-20,2,-3}};
        const EyeFov mono{-.7f,.8f,.65f,-.6f};
        const auto original=eye_constants(frame,{{0,0,0},{0,0,0,1}},mono,.5f,2000);
        StereoReconstructionContext context{};
        context.original_camera=frame;
        std::copy_n(original.begin(),16,context.original_projection.begin());
        auto basis=identity_matrix(),screen=identity_matrix();
        const Vec3 axes[]{frame.right,frame.up,frame.backward};
        const auto component=[](Vec3 value,unsigned i) {return i==0?value.x:i==1?value.y:value.z;};
        for (unsigned row=0;row<3;++row) for (unsigned column=0;column<3;++column)
            basis[row*4+column]=component(axes[column],row);
        const float tx=(std::tan(mono.right)-std::tan(mono.left))*.5f;
        const float ty=(std::tan(mono.up)-std::tan(mono.down))*.5f;
        const auto forward=scale(frame.backward,-1);
        const auto depth_axis=add(forward,add(
            scale(frame.right,(std::tan(mono.right)+std::tan(mono.left))*.5f),
            scale(frame.up,(std::tan(mono.up)+std::tan(mono.down))*.5f)));
        const Vec3 columns[]{scale(frame.right,tx),scale(frame.up,ty),depth_axis,frame.origin};
        for (unsigned row=0;row<3;++row) for (unsigned column=0;column<4;++column)
            screen[row*4+column]=component(columns[column],row);
        std::size_t checks=0;
        if (screen_reconstruction_translated(context,screen))
            throw std::runtime_error("Absolute native screen matrix was classified as translated");
        auto translated_screen=screen;
        translated_screen[3]+=frame.pretranslation.x;
        translated_screen[7]+=frame.pretranslation.y;
        translated_screen[11]+=frame.pretranslation.z;
        if (!screen_reconstruction_translated(context,translated_screen))
            throw std::runtime_error("Translated native screen matrix was classified as absolute");
        auto corrupt_screen=screen;corrupt_screen[3]+=3;
        bool corrupt_rejected=false;
        try {screen_reconstruction_translated(context,corrupt_screen);}
        catch (const std::invalid_argument&) {corrupt_rejected=true;}
        if (!corrupt_rejected) throw std::runtime_error("Unrelated native reconstruction origin was accepted");
        for (float yaw:{-1.2f,0.f,.9f}) for (float roll:{-.6f,0.f,.4f})
            for (unsigned index=0;index<2;++index) {
                const auto sy=std::sin(yaw*.5f),cy=std::cos(yaw*.5f);
                const auto sr=std::sin(roll*.5f),cr=std::cos(roll*.5f);
                const EyePose pose{{index?.232f:.168f,-.1f,.15f},
                    {sy*sr,sy*cr,cy*sr,cy*cr}};
                const EyeFov fov=index?EyeFov{-.6f,1.f,.8f,-.65f}:EyeFov{-.9f,.65f,.75f,-.7f};
                const auto eye=eye_constants(frame,pose,fov,.5f,2000);
                const auto inverse_screen=matrix_product(reconstruction_delta(context,eye),screen);
                const auto rotated_basis=matrix_product(reconstruction_rotation(context,eye),basis);
                for (float x:{-.8f,0.f,.7f}) for (float y:{-.6f,0.f,.8f})
                    for (float depth:{1.f,25.f,100.f}) {
                        const float local_x=((std::tan(fov.right)-std::tan(fov.left))*x+
                            std::tan(fov.right)+std::tan(fov.left))*.5f;
                        const float local_y=((std::tan(fov.up)-std::tan(fov.down))*y+
                            std::tan(fov.up)+std::tan(fov.down))*.5f;
                        const auto direction=map_tracking(frame,rotate(pose.orientation,{local_x,local_y,-1}));
                        const auto origin=add(frame.origin,scale(map_tracking(frame,pose.position),100));
                        const auto expected=add(origin,scale(direction,depth));
                        const auto actual=apply(inverse_screen,{x*depth,y*depth,depth,1});
                        for (unsigned axis=0;axis<3;++axis) {close(actual[axis],component(expected,axis));++checks;}
                        close(actual[3],1);++checks;
                        const auto rotated=apply(rotated_basis,{x,y,-1,0});
                        const auto expected_rotation=map_tracking(frame,rotate(pose.orientation,{x,y,-1}));
                        for (unsigned axis=0;axis<3;++axis) {close(rotated[axis],component(expected_rotation,axis));++checks;}
                    }
            }
        for (const float native_depth:{.999f,1.f,1.02f})
            for (const float near_plane:{.125f,2.f}) {
                const auto native=eye_constants(frame,{{0,0,0},{0,0,0,1}},mono,
                    near_plane,10000,native_depth);
                auto shadow_context=context;
                std::copy_n(native.begin(),16,shadow_context.original_projection.begin());
                for (const float yaw:{-.8f,0.f,.7f}) for (unsigned index=0;index<2;++index) {
                    const EyePose pose{{index?.232f:.168f,-.1f,.15f},
                        {0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)}};
                    const EyeFov fov=index?EyeFov{-.6f,1.f,.8f,-.65f}:EyeFov{-.9f,.65f,.75f,-.7f};
                    const auto eye=eye_constants(frame,pose,fov,near_plane,10000,native_depth);
                    const auto delta=fault?identity_matrix():screen_input_delta(shadow_context,eye);
                    for (const float x:{-.9f,0.f,.9f}) for (const float y:{-.8f,0.f,.8f})
                        for (const float distance:{.25f,2.f,25.f,10000.f}) {
                            const float ray_x=((std::tan(fov.right)-std::tan(fov.left))*x+
                                std::tan(fov.right)+std::tan(fov.left))*.5f;
                            const float ray_y=((std::tan(fov.up)-std::tan(fov.down))*y+
                                std::tan(fov.up)+std::tan(fov.down))*.5f;
                            const auto direction=map_tracking(frame,rotate(pose.orientation,{ray_x,ray_y,-1}));
                            const auto relative=add(scale(map_tracking(frame,pose.position),100),
                                scale(direction,distance));
                            const float native_distance=dot(relative,forward);
                            const float expected_x=2*dot(relative,frame.right)/
                                (std::tan(mono.right)-std::tan(mono.left))-
                                native_distance*(std::tan(mono.right)+std::tan(mono.left))/
                                (std::tan(mono.right)-std::tan(mono.left));
                            const float expected_y=2*dot(relative,frame.up)/
                                (std::tan(mono.up)-std::tan(mono.down))-
                                native_distance*(std::tan(mono.up)+std::tan(mono.down))/
                                (std::tan(mono.up)-std::tan(mono.down));
                            const std::array<float,4> expected{expected_x,expected_y,native_distance,1};
                            const auto actual=apply(delta,{x*distance,y*distance,distance,1});
                            for (unsigned axis=0;axis<4;++axis) {
                                if (!std::isfinite(actual[axis]) || std::abs(actual[axis]-expected[axis])>
                                    .001f+.0001f*std::abs(expected[axis]))
                                    throw std::runtime_error("Native shadow/light screen input differs from the independent physical point:"
                                        " depth_scale="+std::to_string(native_depth)+" near="+std::to_string(near_plane)+
                                        " yaw="+std::to_string(yaw)+" eye="+std::to_string(index)+
                                        " x="+std::to_string(x)+" y="+std::to_string(y)+
                                        " distance="+std::to_string(distance)+" axis="+std::to_string(axis)+
                                        " actual="+std::to_string(actual[axis])+" expected="+std::to_string(expected[axis]));
                                ++checks;
                            }
                        }
                }
            }
        bool rejected=false;
        try { matrix_inverse(Matrix4{}); } catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Singular reconstruction matrix was accepted");
        std::cout<<"PASS: independent tracked world-ray and rotation reconstruction; checks="
            <<checks<<"; asymmetric FOV, IPD, yaw/roll, engine basis and nonzero pretranslation;"
            <<" shadow/light screen input; native .999/infinite/finite depth; .125/2 near planes;"
            <<" physical points from .25 to 10000 units\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
