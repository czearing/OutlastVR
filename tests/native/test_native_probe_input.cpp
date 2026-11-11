#include "native_probe_input.h"
#include <iostream>
#include <stdexcept>

int main() {
    using namespace olvr;
    try {
        for (unsigned world=0;world<=900;++world) {
            if (native_probe_camera_yaw(true,world,true)!=.5f*std::sin(float(world)*.025f) ||
                native_probe_camera_yaw(false,world,false)!=0 ||
                native_probe_camera_yaw(true,world,false)!=0 ||
                std::abs(native_probe_camera_yaw(false,world,true))>.15f)
                throw std::runtime_error("Optical yaw lost shared scene coverage or changed historical combined input");
        }
        for (float yaw:{-3.1f,-1.f,0.f,.7f,2.9f}) {
            const auto q=native_probe_camera_grip(false,yaw);
            const auto old=native_probe_camera_grip(true,yaw);
            const auto s=std::sin(yaw*.5f),c=std::cos(yaw*.5f);
            if (q!=std::array<float,4>{0,s,0,c} ||
                old!=std::array<float,4>{-c*.7071067812f,s*.7071067812f,s*.7071067812f,c*.7071067812f})
                throw std::runtime_error("New optical fixture changed canonical grip yaw or historical combined inputs");
            for (const auto& value:{q,old})
                if (std::abs(value[0]*value[0]+value[1]*value[1]+value[2]*value[2]+value[3]*value[3]-1)>1e-6f)
                    throw std::runtime_error("Optical fixture supplied a nonunit grip quaternion");
        }
        for (const bool combined:{false,true}) {
            const auto q=native_probe_camera_optical_roll(combined);
            const float norm=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
            if (norm!=1 || q[0] || q[2] || q[1]!=(combined?0.f:1.f) || q[3]!=(combined?1.f:0.f))
                throw std::runtime_error("Optical grip roll changed the historical combined pose");
            for (unsigned axis=0;axis<3;++axis) {
                std::array<float,3> v{};v[axis]=1;
                const std::array<float,3> cross{
                    q[1]*v[2]-q[2]*v[1],q[2]*v[0]-q[0]*v[2],q[0]*v[1]-q[1]*v[0]};
                const std::array<float,3> twice{
                    q[1]*cross[2]-q[2]*cross[1],q[2]*cross[0]-q[0]*cross[2],
                    q[0]*cross[1]-q[1]*cross[0]};
                for (unsigned coordinate=0;coordinate<3;++coordinate) {
                    const float expected=coordinate==axis?(combined || axis==1?1.f:-1.f):0.f;
                    if (v[coordinate]+2*q[3]*cross[coordinate]+2*twice[coordinate]!=expected)
                        throw std::runtime_error("Controlled optical roll changed grip forward or failed to invert both side axes");
                }
            }
        }
        for (unsigned flags=0;flags<16;++flags) {
            const bool allowed=native_probe_body_fixture(flags&1,flags&2,flags&4,flags&8);
            if (allowed!=(flags==1 || flags==9 || flags==13))
                throw std::runtime_error("Body admission lost isolated or explicitly combined head-turn ownership");
        }
        unsigned recoveries=0,releases=0,presses=0;
        for (unsigned world=0;world<=900;++world) {
            const auto regrip=native_probe_material_regrip(world);
            if (regrip.hand<0) {
                if (regrip.hand!=-1 || regrip.squeeze || regrip.approach)
                    throw std::runtime_error("Inactive material recovery changed controller input");
                continue;
            }
            ++recoveries;
            releases+=regrip.squeeze==0;
            presses+=regrip.squeeze==1;
            if (!regrip.approach || regrip.hand!=(world<334?1:0) ||
                (world>=320 && world<=420 && (world-320)%20==0))
                throw std::runtime_error("Recovery input overlapped a material witness or changed its hand");
        }
        if (recoveries!=36 || releases!=16 || presses!=20 ||
            native_probe_material_regrip(327).squeeze || !native_probe_material_regrip(328).squeeze ||
            !native_probe_material_regrip(398).squeeze || native_probe_material_regrip(405).squeeze ||
            !native_probe_material_regrip(406).squeeze)
            throw std::runtime_error("Material fixture lost explicit release/regrasp edges");
        for (unsigned flags=0;flags<32;++flags) {
            const bool allowed=native_probe_readback_free_fixture(
                flags&1,flags&2,flags&4,flags&8,flags&16);
            if (allowed!=(flags==3 || flags==21 || flags==25))
                throw std::runtime_error("Readback-free diagnostic admitted an incomplete or mixed fixture");
            const bool combined=native_probe_readback_free_fixture(
                flags&1,flags&2,flags&4,flags&8,flags&16,true);
            if (combined!=(flags==27))
                throw std::runtime_error("Combined body/camera material fixture admitted an incomplete declaration");
        }
        for (const std::uint64_t world:{300u,320u,355u,420u,449u}) {
            const auto isolated=native_probe_window_translation(world,false);
            const auto combined=native_probe_window_translation(world,true);
            if (isolated[0]!=combined[0] || isolated[2]!=combined[2] ||
                std::abs(combined[1]-isolated[1]-1.7f)>2e-7f ||
                std::abs(isolated[0]-.45*std::sin(double(world-300)*.07))>1e-6 ||
                std::abs(isolated[1]-.25*std::sin(double(world-300)*.11))>1e-6 ||
                std::abs(isolated[2]-.45*std::sin(double(world-300)*.05))>1e-6)
                throw std::runtime_error("Combined glass sweep changed roomscale motion or dropped measured floor height");
        }
        for (const std::uint64_t world:{299u,450u}) {
            bool rejected=false;
            try {native_probe_window_translation(world,true);}
            catch (const std::invalid_argument&) {rejected=true;}
            if (!rejected) throw std::runtime_error("Window translation escaped its declared native interval");
        }
        const auto left=native_probe_asymmetric_fov(0),right=native_probe_asymmetric_fov(1);
        if (left[0]!=-1.f || left[1]!=.85f || right[0]!=-.85f || right[1]!=1.f ||
            left[2]!=.9f || left[3]!=-.95f || left[2]!=right[2] || left[3]!=right[3])
            throw std::runtime_error("Asymmetric FOV fixture changed its explicit off-axis eye bounds");
        bool bad_eye_rejected=false;
        try {native_probe_asymmetric_fov(2);}
        catch (const std::invalid_argument&) {bad_eye_rejected=true;}
        if (!bad_eye_rejected) throw std::runtime_error("Asymmetric FOV accepted a third eye");
        for (const bool lean:{false,true}) {
            unsigned events=0,held=0;
            for (unsigned frame=1;frame<=900;++frame) {
                const auto action=native_probe_gait_action(frame,lean);
                if (!action.key) continue;
                ++events;
                if (action.down) {
                    if (held) throw std::runtime_error("Native gait pressed overlapping owned keys");
                    held=action.key;
                } else {
                    if (held!=action.key) throw std::runtime_error("Native gait released an unowned key");
                    held=0;
                }
            }
            if (held || events!=(lean?8u:4u) ||
                native_probe_gait_version!=3 ||
                native_probe_gait_action(60,lean).key!='W' ||
                !native_probe_gait_action(60,lean).down ||
                native_probe_gait_action(80,lean).key!='W' ||
                native_probe_gait_action(80,lean).down ||
                native_probe_gait_action(120,lean).key ||
                native_probe_gait_action(180,lean).key ||
                native_probe_gait_action(500,lean).key!=(lean?unsigned('Q'):0u) ||
                native_probe_gait_action(650,lean).key!=(lean?unsigned('E'):0u))
                throw std::runtime_error("Native gait/lean schedule did not preserve bounded owned input");
        }
        for (std::uint32_t message=0x100;message<=0x107;++message) {
            std::intptr_t bits=0x001d0001;
            if (native_probe_input_message(message,bits) || bits!=0x001d0001)
                throw std::runtime_error("Physical keyboard input reached the headless target");
            for (const auto original:{std::intptr_t(0x001d0001),std::intptr_t(0x011d0001),
                std::intptr_t(0xc01d0001u)}) {
                bits=original|native_probe_key_marker;
                if (!native_probe_input_message(message,bits) || bits!=original)
                    throw std::runtime_error("Owned keyboard input lost its original scan or transition bits");
            }
        }
        std::intptr_t unicode=0;
        if (native_probe_input_message(0x109,unicode))
            throw std::runtime_error("Unowned Unicode input reached the headless target");
        for (std::uint32_t message=0x200;message<=0x20e;++message) {
            std::intptr_t bits=0x123456;
            if (native_probe_input_message(message,bits) || bits!=0x123456)
                throw std::runtime_error("Physical client mouse input reached the headless target");
        }
        for (const auto message:{0x6u,0x7u,0x8u,0x10u,0x112u,0x218u}) {
            std::intptr_t bits=0x123456;
            if (!native_probe_input_message(message,bits) || bits!=0x123456)
                throw std::runtime_error("Headless isolation intercepted focus, close or system messages");
        }
        NativeProbeKeys keys;
        for (const auto message:{0x200u,0x201u,0x202u}) {
            for (const auto coordinates:{std::intptr_t(0),std::intptr_t(0xffffffffu),
                std::intptr_t(0x7fff8000u)}) {
                auto bits=coordinates;
                std::uintptr_t flags=message==0x201?1:0;
                if (keys.accept_message(message,flags,bits) || keys.down(1) || bits!=coordinates)
                    throw std::runtime_error("Physical mouse message reached native input or polling");
                flags|=native_probe_mouse_marker;
                if (!keys.accept_message(message,flags,bits) || flags!=(message==0x201?1u:0u) ||
                    bits!=coordinates || keys.down(1)!=(message==0x201))
                    throw std::runtime_error("Owned mouse lost exact signed coordinates, flags or button state");
                flags=native_probe_mouse_marker;
                if (!keys.accept_message(0x202,flags,bits) || keys.down(1))
                    throw std::runtime_error("Owned mouse release remained pressed");
            }
        }
        for (std::uint32_t message=0x203;message<=0x20e;++message) {
            std::uintptr_t flags=native_probe_mouse_marker;
            std::intptr_t bits=0x123456;
            if (keys.accept_message(message,flags,bits) || bits!=0x123456 || keys.down(1))
                throw std::runtime_error("Owned menu mouse path admitted unsupported mouse messages");
        }
        for (const auto key:{0x57u,0xa0u,0xa1u,0xa2u,0xa3u,0xa4u,0xa5u}) {
            std::intptr_t bits=1;
            if (keys.accept(0x100,key,bits) || keys.down(key))
                throw std::runtime_error("Physical key changed virtual polling state");
            bits=1|native_probe_key_marker;
            if (!keys.accept(0x100,key,bits) || !keys.down(key) || bits!=1)
                throw std::runtime_error("Owned key-down was not visible to native polling");
            const auto modifier=key>=0xa0?0x10u+(key-0xa0)/2:0u;
            if (modifier && !keys.down(modifier))
                throw std::runtime_error("Native modifier aggregation lost an owned key");
            bits=std::intptr_t(0xc0000001u)|native_probe_key_marker;
            if (!keys.accept(0x101,key,bits) || keys.down(key) ||
                (modifier && keys.down(modifier)) || bits!=std::intptr_t(0xc0000001u))
                throw std::runtime_error("Owned key-up did not release native polling state");
        }
        std::cout<<"PASS: 16 body admissions; 36 explicit recovery inputs; 64 isolated/combined readback fixture combinations; measured-floor roomscale poses; "
            "explicit asymmetric FOV and invalid-eye rejection; "
            "owned-only headless key messages; exact scan/extended/release bits; "
            "owned polling and left/right modifiers; physical client pointer input rejected; "
            "genuine focus and close messages preserved\n";
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
