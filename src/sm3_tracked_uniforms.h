#pragma once
#include "sm3_runtime.h"
#include <map>

namespace olvr::sm3 {

enum class TrackedUniformKind {
    inverse_world, inverse_translated, inverse_projective_world, view_to_world, world_to_view,
    camera_world, camera_translated, screen_to_light, material_camera,
    material_projection, translated_projection, camera_direction
};

struct TrackedUniform {
    bool vertex;
    unsigned original_register,rows,texture_row;
    TrackedUniformKind kind;
    bool column_major=false;
};

struct TrackedPair {
    Pair pair;
    std::vector<TrackedUniform> uniforms;
    unsigned texture_width=8;
    bool vertex_texture=false;
    int pixel_sampler=-1;
};

inline std::uint32_t float_word(float value) {
    std::uint32_t result=0;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}

inline TrackedPair track_uniforms(Pair pair) {
    TrackedPair result;
    result.vertex_texture=pair.view_texture;
    result.pair=std::move(pair);
    unsigned next_row=8;
    std::vector<unsigned> zero_motion_registers;
    for (const bool vertex:{true,false}) {
        const auto info=inspect(parse(vertex?result.pair.vertex:result.pair.pixel));
        const bool material_camera=!vertex && std::any_of(info.constants.begin(),info.constants.end(),
            [](const Constant& c){return c.set==2 && c.name=="CameraWorldPos";});
#ifndef OLVR_UNTRACKED_PARTICLE_CAMERA
        const bool particle_camera=vertex && std::any_of(info.constants.begin(),info.constants.end(),
            [](const Constant& c){return c.set==2 && c.name=="ParticleUpRightResultScalars";});
#endif
        for (const auto& constant:info.constants) {
            if (constant.set!=2) continue;
            if (!vertex && (constant.name=="MotionBlurMaskScaleAndBias" ||
                constant.name=="DynamicVelocityParameters")) {
                if (constant.count!=1) throw std::invalid_argument("Unsupported motion policy uniform");
                zero_motion_registers.push_back(constant.number);
            }
            TrackedUniformKind kind{};
            unsigned rows=0;
            if (!vertex && constant.name=="ViewProjectionMatrix") {
                kind=material_camera?TrackedUniformKind::material_projection:
                    TrackedUniformKind::translated_projection;rows=4;
            } else if (material_camera && constant.name=="CameraWorldPos") {
                kind=TrackedUniformKind::material_camera;rows=1;
            } else if (constant.name=="ScreenToShadowMatrix" || constant.name=="ScreenToLight") {
                kind=TrackedUniformKind::screen_to_light;rows=4;
            } else if (constant.name=="ScreenToWorld" || constant.name=="ScreenToWorldMatrix" ||
                constant.name=="ScreenToWorldOffset") {
                kind=TrackedUniformKind::inverse_world;rows=4;
            } else if (constant.name=="InvViewProjectionMatrix") {
                kind=TrackedUniformKind::inverse_projective_world;rows=4;
            } else if (constant.name=="ScreenToTranslatedWorld" ||
                       constant.name=="ScreenToTranslatedWorldMatrix") {
                kind=TrackedUniformKind::inverse_translated;rows=4;
            } else if (constant.name=="ViewToWorldMatrix") {
                kind=TrackedUniformKind::view_to_world;rows=3;
            } else if (constant.name=="WorldToViewMatrix") {
                kind=TrackedUniformKind::world_to_view;rows=3;
            } else if (vertex && constant.name=="CameraPositionVS" && !result.pair.view_texture) {
                kind=TrackedUniformKind::camera_translated;rows=1;
#ifndef OLVR_UNTRACKED_PARTICLE_CAMERA
            } else if (vertex && constant.name=="CameraWorldPosition") {
                kind=TrackedUniformKind::camera_world;rows=1;
            } else if (particle_camera && (constant.name=="CameraRight" || constant.name=="CameraUp")) {
                kind=TrackedUniformKind::camera_direction;rows=1;
#endif
            } else if (!vertex && (constant.name=="FogCameraPosition" ||
                       constant.name=="WorldSpaceCameraPositionAndDistance" ||
                       (constant.name=="CameraPositionPS" && result.pair.camera_base_constant<0))) {
                kind=TrackedUniformKind::camera_world;rows=1;
            }
            if (!rows) continue;
            if (rows==4 && constant.count==3) rows=3;
            if (constant.count!=rows || constant.number+rows>(vertex?256u:224u))
                throw std::invalid_argument("Unsupported tracked uniform extent: "+constant.name);
            result.uniforms.push_back({vertex,constant.number,rows,next_row,kind,constant.matrix_class==3});
            next_row+=rows;
        }
    }
    if (result.uniforms.empty() && zero_motion_registers.empty()) return result;
    while (result.texture_width<next_row) result.texture_width*=2;
    for (const bool vertex:{true,false}) {
        auto& program=vertex?result.pair.vertex:result.pair.pixel;
        const auto instructions=parse(program);
        auto info=inspect(instructions);
        auto [headers,body]=split(instructions);
        if (!vertex) for (const auto number:zero_motion_registers) {
            for (const auto& instruction:instructions)
                if (instruction.opcode==81 && kind(instruction.tokens[1])==2 &&
                    index(instruction.tokens[1])==number)
                    throw std::invalid_argument("Motion policy conflicts with an existing shader definition");
            op(headers,81,{destination(2,number),0,0,0,0});
        }
        Words prefix,rewritten;
        std::vector<int> mapping(vertex?256:224,-1);
        std::vector<const TrackedUniform*> bindings;
        for (const auto& uniform:result.uniforms)
            if (uniform.vertex==vertex) bindings.push_back(&uniform);
        if (vertex) {
            for (std::size_t cursor=0;cursor<headers.size();) {
                const auto opcode=headers[cursor]&65535;
                const auto count=opcode==65534?(headers[cursor]>>16)&32767:(headers[cursor]>>24)&15;
                if (opcode==81 && kind(headers[cursor+1])==2 &&
                    int(index(headers[cursor+1]))==result.pair.coordinate_constant) {
                    headers[cursor+2]=float_word(1.f/result.texture_width);
                    headers[cursor+4]=float_word(.5f/result.texture_width);
                }
                cursor+=count+1;
            }
        }
        if (bindings.empty()) {
            program={vertex?0xfffe0300u:0xffff0300u};
            append(program,canonical(headers));append(program,body);program.push_back(65535);
            continue;
        }
        const auto sampler=vertex && result.pair.view_texture?result.pair.sampler:
            first(info.free(10,vertex?4:16),"No free tracked uniform texture sampler");
        if (vertex) {
            result.vertex_texture=true;
            result.pair.sampler=sampler;
        } else result.pixel_sampler=static_cast<int>(sampler);
        if (!vertex || !result.pair.view_texture)
            op(headers,31,{0x90000000u,destination(10,sampler)});
        const auto coordinate=first(info.free(2,vertex?256:224),"No free tracked texture coordinate constant");
        info.used[2].set(coordinate);
        op(headers,81,{destination(2,coordinate),
            float_word(1.f/result.texture_width),float_word(.5f),
            float_word((bindings.front()->texture_row+.5f)/result.texture_width),float_word(.25f)});
        auto reserve_temp=[&](unsigned count) {
            for (unsigned start=0;start+count<=32;++start) {
                bool available=true;
                for (unsigned i=0;i<count;++i) available &= !info.used[0][start+i];
                if (available) {
                    for (unsigned i=0;i<count;++i) info.used[0].set(start+i);
                    return start;
                }
            }
            throw std::invalid_argument("No contiguous temporaries for tracked uniform");
        };
        const auto uv=reserve_temp(1);
        int material_input=-1,material_value=-1;
        const TrackedUniform* material_camera=nullptr;
        for (const auto* binding:bindings)
            if (binding->kind==TrackedUniformKind::material_camera) material_camera=binding;
        if (material_camera) {
            for (const auto& declaration:info.declarations)
                if (declaration.type==1 && declaration.usage==5 && declaration.semantic==5) {
                    if (material_input>=0 || !(declaration.mask&7))
                        throw std::invalid_argument("Material world-position varying is ambiguous");
                    material_input=int(declaration.number);material_value=int(reserve_temp(1));
                    op(prefix,1,{destination(0,material_value,declaration.mask),source(1,material_input)});
                }
            if (material_input<0 && std::any_of(bindings.begin(),bindings.end(),
                [](const TrackedUniform* b){return b->kind==TrackedUniformKind::material_projection;}))
                throw std::invalid_argument("Material projection lacks its world-position varying");
        }
        unsigned eye=0;
        bool found=false;
        for (const auto& declaration:info.declarations)
            if (declaration.type==1 && declaration.usage==5 &&
                declaration.semantic==(vertex?result.pair.input_semantic:result.pair.eye_semantic)) {
                eye=declaration.number;found=true;break;
            }
        if (!found) throw std::invalid_argument("Tracked shader has no paired eye input");
        op(prefix,1,{destination(0,uv,1),source(2,coordinate,0xaa)});
        op(prefix,4,{destination(0,uv,2),source(1,eye,vertex?0:result.pair.component*0x55),
            source(2,coordinate,0x55),source(2,coordinate,0xff)});
        op(prefix,2,{destination(0,uv,12),source(2,coordinate),source(2,coordinate)|0x01000000u});
        for (const auto* binding:bindings) {
            const auto temporary=reserve_temp(binding->rows);
            for (unsigned row=0;row<binding->rows;++row) {
                if (mapping[binding->original_register+row]>=0)
                    throw std::invalid_argument("Tracked uniform register ranges overlap");
                mapping[binding->original_register+row]=static_cast<int>(temporary+row);
                op(prefix,vertex?95:66,{destination(0,temporary+row),source(0,uv),source(10,sampler)});
                op(prefix,2,{destination(0,uv,1),source(0,uv,0),source(2,coordinate,0)});
            }
        }
        if (material_input>=0) {
            const auto mapped=mapping[material_camera->original_register];
            unsigned mask=0;
            for (const auto& d:info.declarations) if (int(d.number)==material_input && d.type==1) mask=d.mask&7;
            op(prefix,2,{destination(0,material_value,mask),source(0,material_value),
                source(2,material_camera->original_register)});
            op(prefix,2,{destination(0,material_value,mask),source(0,material_value),
                source(0,mapped)|0x01000000u});
        }
        const Words wrapped{vertex?0xfffe0300u:0xffff0300u};
        Words body_program=wrapped;
        append(body_program,body);body_program.push_back(65535);
        for (const auto& instruction:parse(body_program)) {
            auto tokens=instruction.tokens;
            if (instruction.opcode>=20 && instruction.opcode<=24 && tokens.size()==4 &&
                kind(tokens[3])==2 && index(tokens[3])<mapping.size() &&
                mapping[index(tokens[3])]>=0) {
                if ((tokens[0]&0x10000000u) || (tokens[3]&0x2000u))
                    throw std::invalid_argument("Predicated/relative tracked matrix operation is unsupported");
                const unsigned rows=instruction.opcode==20 || instruction.opcode==22?4:
                    instruction.opcode==24?2:3;
                const unsigned dot=instruction.opcode<=21?9:8;
                if (kind(tokens[2])==2 && index(tokens[2])<mapping.size() &&
                    mapping[index(tokens[2])]>=0) {
                    if (tokens[2]&0x2000u)
                        throw std::invalid_argument("Relative tracked matrix vector is unsupported");
                    tokens[2]=replace(tokens[2],0,static_cast<unsigned>(mapping[index(tokens[2])]));
                }
                else if (material_input>=0 && kind(tokens[2])==1 &&
                    int(index(tokens[2]))==material_input)
                    tokens[2]=replace(tokens[2],0,unsigned(material_value));
                for (unsigned row=0;row<rows;++row) {
                    if (!((tokens[1]>>(16+row))&1)) continue;
                    const auto original=index(tokens[3])+row;
                    if (original>=mapping.size() || mapping[original]<0)
                        throw std::invalid_argument("Tracked matrix operation exceeds its mapped rows");
                    const auto destination_row=(tokens[1]&~0xf0000u)|(1u<<(16+row));
                    const Words expanded{
                        (tokens[0]&~0x0f00ffffu)|0x03000000u|dot,destination_row,tokens[2],
                        replace(tokens[3],0,static_cast<unsigned>(mapping[original]))};
                    append(rewritten,expanded);
                }
                continue;
            }
            for (std::size_t operand=2;operand<tokens.size();++operand)
                if (material_input>=0 && kind(tokens[operand])==1 &&
                    int(index(tokens[operand]))==material_input)
                    tokens[operand]=replace(tokens[operand],0,unsigned(material_value));
                else if (kind(tokens[operand])==2 && index(tokens[operand])<mapping.size() &&
                    mapping[index(tokens[operand])]>=0) {
                    if (tokens[operand]&0x2000u)
                        throw std::invalid_argument("Relative tracked uniform addressing is unsupported");
                    tokens[operand]=replace(tokens[operand],0,
                        static_cast<unsigned>(mapping[index(tokens[operand])]));
                }
            append(rewritten,tokens);
        }
        program=wrapped;
        append(program,canonical(headers));append(program,prefix);append(program,rewritten);program.push_back(65535);
    }
    return result;
}

}
