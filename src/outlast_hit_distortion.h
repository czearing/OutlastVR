#pragma once
#include "sm3_runtime.h"

namespace olvr {

inline sm3::Words outlast_without_hit_distortion(const sm3::Words& program) {
    const auto instructions=sm3::parse(program);
    const auto info=sm3::inspect(instructions);
    unsigned hurt=2048,circle=2048;
    for (const auto& constant:info.constants) {
        if (constant.name!="HurtParams" && constant.name!="HurtCircleParams") continue;
        if (constant.set!=2 || constant.count!=1)
            throw std::invalid_argument("Unsupported native hit parameter binding");
        (constant.name=="HurtParams"?hurt:circle)=constant.number;
    }
    if (hurt==2048) return program;
    if (circle==2048) throw std::invalid_argument("Native hit shader has no circle parameters");
    // Keep the shared hurt mask and damage color; bypass only GetHurtUV's final blend.
    constexpr unsigned angle=1,mask=2,amount=4;
    std::array<std::array<unsigned,4>,32> temporary{};
    struct Difference {unsigned number=0,channel=0,version=0;bool valid=false;};
    std::array<std::array<Difference,4>,32> difference{};
    std::array<std::array<unsigned,4>,32> version{};
    const auto component=[&](std::uint32_t token,unsigned lane) {
        const auto channel=(token>>(16+lane*2))&3;
        const auto type=sm3::kind(token),number=sm3::index(token);
        if (token&0x2000) throw std::invalid_argument("Relative native hit operands are unsupported");
        if (type==0) {
            if (number>=temporary.size()) throw std::invalid_argument("Native hit temporary exceeds SM3");
            return temporary[number][channel];
        }
        if (type==2 && number==hurt) return channel==0?amount:channel==1?angle:0u;
        return type==2 && number==circle?mask:0u;
    };
    bool uses_angle=false;
    std::size_t offset=1;
    for (const auto& instruction:instructions) {
        const auto& tokens=instruction.tokens;
        if (sm3::header(instruction.opcode)) {offset+=tokens.size();continue;}
        if (tokens.size()<3)
            throw std::invalid_argument("Unsupported native hit shader before its UV blend");
        if ((instruction.opcode>=38 && instruction.opcode<=48) ||
            (instruction.opcode>=25 && instruction.opcode<=30))
            throw std::invalid_argument("Native hit shader requires unsupported control-flow analysis");
        const auto target=sm3::index(tokens[1]),write_mask=(tokens[1]>>16)&15;
        const bool temp=sm3::kind(tokens[1])==0;
        if (temp && target>=temporary.size()) throw std::invalid_argument("Native hit destination exceeds SM3");
        const bool pair=write_mask==3 || write_mask==6 || write_mask==12;
        if (temp && instruction.opcode==18 && tokens.size()==5 && pair) {
            bool matches=true;
            for (unsigned lane=0;lane<4;++lane) if (write_mask&(1u<<lane))
                matches&=component(tokens[2],lane)==(mask|amount) &&
                    component(tokens[3],lane)==0 && component(tokens[4],lane)==angle;
            if (matches) {
                sm3::Words result(program.begin(),program.begin()+offset);
                sm3::op(result,1,{tokens[1],tokens[3]});
                result.insert(result.end(),program.begin()+offset+tokens.size(),program.end());
                return result;
            }
        }
        // MAD compilers retain original-rotated; require the same unmodified rotated value.
        if (instruction.opcode==4 && tokens.size()==5 && pair) {
            bool matches=sm3::kind(tokens[3])==0 && sm3::kind(tokens[4])==0 &&
                sm3::index(tokens[3])<32 && sm3::index(tokens[4])<32 &&
                ((tokens[3]|tokens[4])&0x0f002000)==0;
            for (unsigned lane=0;lane<4 && matches;++lane) if (write_mask&(1u<<lane)) {
                const auto& d=difference[sm3::index(tokens[3])][(tokens[3]>>(16+lane*2))&3];
                const auto number=sm3::index(tokens[4]),channel=(tokens[4]>>(16+lane*2))&3;
                matches=component(tokens[2],lane)==(mask|amount) && d.valid &&
                    d.number==number && d.channel==channel && d.version==version[number][channel];
            }
            if (matches) {
                sm3::Words result(program.begin(),program.begin()+offset);
                sm3::op(result,2,{tokens[1],tokens[3],tokens[4]});
                result.insert(result.end(),program.begin()+offset+tokens.size(),program.end());
                return result;
            }
        }
        std::array<unsigned,4> output{};
        std::array<Difference,4> new_difference{};
        for (unsigned lane=0;lane<4;++lane) {
            if (!(write_mask&(1u<<lane))) continue;
            for (std::size_t source=2;source<tokens.size();++source) {
                if (instruction.opcode==8 || instruction.opcode==9 || instruction.opcode==90 ||
                    instruction.opcode==36) {
                    const unsigned count=instruction.opcode==9?4:
                        instruction.opcode==90?(source==4?1:2):3;
                    for (unsigned channel=0;channel<count;++channel)
                        output[lane]|=component(tokens[source],channel);
                } else output[lane]|=component(tokens[source],instruction.opcode==37?0:lane);
            }
            uses_angle|=(output[lane]&angle)!=0;
            const bool subtract=(instruction.opcode==2 && tokens.size()==4 &&
                component(tokens[2],lane)==0) ||
                (instruction.opcode==4 && tokens.size()==5 && component(tokens[2],lane)==0 &&
                    component(tokens[3],lane)==0);
            const auto negative=tokens.back();
            if (subtract && component(negative,lane)==angle && sm3::kind(negative)==0 &&
                ((negative>>24)&15)==1) {
                const auto number=sm3::index(negative),channel=(negative>>(16+lane*2))&3;
                new_difference[lane]={number,channel,version[number][channel],true};
            }
        }
        if (temp) for (unsigned lane=0;lane<4;++lane)
            if (write_mask&(1u<<lane)) {
                temporary[target][lane]=output[lane];
                difference[target][lane]=new_difference[lane];
                ++version[target][lane];
            }
        offset+=tokens.size();
    }
    if (uses_angle) throw std::invalid_argument("Native hit UV blend could not be isolated safely");
    return program;
}

}
