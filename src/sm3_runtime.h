#pragma once
#include <array>
#include <bitset>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <tuple>

namespace olvr::sm3 {
using Words = std::vector<std::uint32_t>;
inline unsigned kind(std::uint32_t token) { return ((token>>28)&7)|((token>>8)&24); }
inline unsigned index(std::uint32_t token) { return token&2047; }
inline std::uint32_t destination(unsigned type,unsigned number,unsigned mask=15) {
    return 0x80000000u|((type&7)<<28)|((type&24)<<8)|number|(mask<<16);
}
inline std::uint32_t source(unsigned type,unsigned number,unsigned swizzle=0xe4) {
    return destination(type,number,0)|(swizzle<<16);
}
inline std::uint32_t replace(std::uint32_t token,unsigned type,unsigned number) {
    return (token&~(0x70001800u|2047))|((type&7)<<28)|((type&24)<<8)|number;
}
inline void append(Words& target,const Words& values) { target.insert(target.end(),values.begin(),values.end()); }
inline void op(Words& target,unsigned opcode,std::initializer_list<std::uint32_t> operands) {
    target.push_back((static_cast<unsigned>(operands.size())<<24)|opcode);
    target.insert(target.end(),operands.begin(),operands.end());
}
struct Instruction { unsigned opcode; Words tokens; };
inline std::vector<Instruction> parse(const Words& words) {
    if (words.size()<2 || words.size()>262144 || (words[0]!=0xfffe0300u && words[0]!=0xffff0300u))
        throw std::invalid_argument("Expected bounded SM3 bytecode");
    std::vector<Instruction> result;
    std::size_t cursor=1;
    while (cursor<words.size()) {
        const auto token=words[cursor];
        const unsigned opcode=token&65535;
        if (opcode==65535) {
            if (token!=65535 || cursor+1!=words.size()) throw std::invalid_argument("Invalid SM3 terminator");
            return result;
        }
        const unsigned count=opcode==65534 ? (token>>16)&32767 : (token>>24)&15;
        if (cursor+1+count>words.size()) throw std::invalid_argument("Truncated SM3 instruction");
        result.push_back({opcode,Words(words.begin()+cursor,words.begin()+cursor+count+1)});
        cursor+=count+1;
    }
    throw std::invalid_argument("SM3 bytecode has no terminator");
}
struct Declaration { unsigned usage,semantic,type,number,mask; };
struct Constant { std::string name; unsigned set,number,count,matrix_class=0; };
struct Info {
    std::array<std::bitset<2048>,20> used{};
    std::vector<Declaration> declarations;
    std::vector<Constant> constants;
    bool view_projection=false;
    std::vector<unsigned> free(unsigned type,unsigned limit) const {
        std::vector<unsigned> result;
        for (unsigned n=0;n<limit;++n) if (!used[type][n]) result.push_back(n);
        return result;
    }
};
inline unsigned read16(const std::vector<unsigned char>& data,std::size_t offset) {
    if (offset>data.size() || data.size()-offset<2) throw std::invalid_argument("Truncated CTAB field");
    return data[offset]|(unsigned(data[offset+1])<<8);
}
inline std::uint32_t read32(const std::vector<unsigned char>& data,std::size_t offset) {
    if (offset>data.size() || data.size()-offset<4) throw std::invalid_argument("Truncated CTAB field");
    return data[offset]|(std::uint32_t(data[offset+1])<<8)|(std::uint32_t(data[offset+2])<<16)|(std::uint32_t(data[offset+3])<<24);
}
inline Info inspect(const std::vector<Instruction>& instructions) {
    Info info;
    bool saw_ctab=false;
    for (const auto& instruction:instructions) {
        const auto& tokens=instruction.tokens;
        if (instruction.opcode==65534) {
            if (!saw_ctab && tokens.size()>=2 && tokens[1]==0x42415443u) {
                saw_ctab=true;
                std::vector<unsigned char> data((tokens.size()-2)*4);
                if (!data.empty()) std::memcpy(data.data(),tokens.data()+2,data.size());
                if (read32(data,0)!=28) throw std::invalid_argument("Invalid CTAB header");
                const auto count=read32(data,12), offset=read32(data,16);
                if (offset>data.size() || count>(data.size()-offset)/20)
                    throw std::invalid_argument("CTAB entries exceed comment");
                for (unsigned n=0;n<count;++n) {
                    const std::size_t entry=offset+std::size_t(n)*20;
                    const auto name_offset=read32(data,entry);
                    if (name_offset>=data.size()) throw std::invalid_argument("Invalid CTAB name offset");
                    std::string name;
                    std::size_t cursor=name_offset;
                    for (;cursor<data.size() && data[cursor];++cursor) {
                        if (data[cursor]>127) throw std::invalid_argument("Non-ASCII CTAB identifier");
                        name.push_back(static_cast<char>(data[cursor]));
                    }
                    if (cursor==data.size()) throw std::invalid_argument("Unterminated CTAB name");
                    Constant constant{name,read16(data,entry+4),read16(data,entry+6),read16(data,entry+8)};
                    constant.matrix_class=read16(data,read32(data,entry+12));
                    if (constant.set==2) {
                        if (constant.number>=2048 || constant.count>2048-constant.number)
                            throw std::invalid_argument("CTAB constant range exceeds SM3 registers");
                        for (unsigned c=0;c<constant.count;++c) info.used[2].set(constant.number+c);
                    }
                    info.view_projection|=constant.name=="ViewProjectionMatrix" && constant.set==2 &&
                                          constant.number==0 && constant.count==4;
                    info.constants.push_back(constant);
                }
            }
            continue;
        }
        std::size_t begin=1,end=tokens.size();
        if (instruction.opcode==31) {
            if (tokens.size()!=3) throw std::invalid_argument("Malformed SM3 declaration");
            info.declarations.push_back({tokens[1]&15,(tokens[1]>>16)&15,kind(tokens[2]),index(tokens[2]),(tokens[2]>>16)&15});
            begin=2;
        } else if (instruction.opcode==47 || instruction.opcode==48 || instruction.opcode==81) {
            end=std::min<std::size_t>(end,2);
        }
        for (std::size_t p=begin;p<end;++p) {
            const auto type=kind(tokens[p]);
            if (type<info.used.size()) info.used[type].set(index(tokens[p]));
        }
    }
    return info;
}
inline bool header(unsigned opcode) { return opcode==65534 || opcode==31 || opcode==47 || opcode==48 || opcode==81; }
inline std::pair<Words,Words> split(const std::vector<Instruction>& instructions) {
    Words prefix,body;
    bool active=false;
    for (const auto& instruction:instructions) {
        if (!header(instruction.opcode)) active=true;
        append(active?body:prefix,instruction.tokens);
    }
    return {prefix,body};
}
inline void declare(Words& words,unsigned usage,unsigned semantic,unsigned type,unsigned number,unsigned mask) {
    op(words,31,{0x80000000u|usage|(semantic<<16),destination(type,number,mask)});
}
inline void extend(Words& words,unsigned type,unsigned number,unsigned component) {
    for (std::size_t cursor=0;cursor<words.size();) {
        const auto opcode=words[cursor]&65535;
        const auto count=opcode==65534 ? (words[cursor]>>16)&32767 : (words[cursor]>>24)&15;
        if (opcode==31 && kind(words[cursor+2])==type && index(words[cursor+2])==number) {
            words[cursor+2]|=1u<<(16+component);
            return;
        }
        cursor+=count+1;
    }
    throw std::invalid_argument("Packed varying declaration not found");
}
inline Words canonical(const Words& words) {
    Words comments,definitions;
    std::vector<Words> declarations;
    for (std::size_t cursor=0;cursor<words.size();) {
        const auto opcode=words[cursor]&65535;
        const auto count=opcode==65534 ? (words[cursor]>>16)&32767 : (words[cursor]>>24)&15;
        Words tokens(words.begin()+cursor,words.begin()+cursor+count+1);
        if (opcode==65534) append(comments,tokens);
        else if (opcode==31) declarations.push_back(tokens);
        else append(definitions,tokens);
        cursor+=count+1;
    }
    auto key=[](const Words& d) {
        const auto t=kind(d[2]);
        return std::make_pair(t==17?0:t==1?1:t==10?2:t==6?3:4,index(d[2]));
    };
    std::stable_sort(declarations.begin(),declarations.end(),[&](const Words& a,const Words& b){return key(a)<key(b);});
    append(comments,definitions);
    for (const auto& d:declarations) append(comments,d);
    return comments;
}
inline unsigned first(const std::vector<unsigned>& values,const char* message) {
    if (values.empty()) throw std::invalid_argument(message);
    return values[0];
}
struct Pair {
    Words vertex,pixel;
    unsigned sampler=0,guard=0,input_semantic=15,eye_semantic=15,component=0;
    bool packed=false,view_texture=false;
    std::vector<std::pair<unsigned,unsigned>> atlas_sampler_constants;
    std::vector<std::pair<unsigned,unsigned>> atlas_point_samplers;
    int pixel_position_constant=-1,camera_base_constant=-1,camera_delta_constant=-1;
    int scissor_constant=-1;
    int scissor_delta_constant=-1;
    int viewport_constant=-1;
    int coordinate_constant=-1;
};
template<class AllocateTemporary>
inline void route_atlas_texture(Words& output,Words tokens,const std::array<int,16>& bindings,
                                unsigned temporary,AllocateTemporary allocate,int& projection,
                                int& gradient_x,int& gradient_y,std::uint32_t eye=0,
                                const std::array<int,16>* point_samplers=nullptr) {
    const unsigned opcode=tokens[0]&65535;
    if (opcode==66 || opcode==93 || opcode==95) {
        if (tokens.size()!=(opcode==93?6u:4u) || kind(tokens[3])!=10)
            throw std::invalid_argument("Texture instruction has unsupported relative operands");
        const auto sampler=index(tokens[3]);
        if (sampler>=16) throw std::invalid_argument("Pixel texture instruction exceeds sampler range");
        const auto binding=bindings[sampler];
        if (binding>=0) {
            const unsigned mask=opcode==95 || (opcode==66 && (tokens[0]&0x30000))?11:3;
            op(output,1,{destination(0,temporary,mask),tokens[2]});
            if (opcode==66 && (tokens[0]&0x10000)) {
                if (projection<0) projection=static_cast<int>(allocate());
                op(output,6,{destination(0,projection,1),source(0,temporary,0xff)});
                op(output,5,{destination(0,temporary,3),source(0,temporary,0x44),source(0,projection,0)});
                tokens[0]&=~0x10000u;
            }
            if (point_samplers && (*point_samplers)[sampler]>=0) {
                if (projection<0) projection=static_cast<int>(allocate());
                // A one-mip explicit lookup also remains legal inside native divergent LOD/gradient branches.
                op(output,95,{destination(0,projection,15),source(0,temporary,0x04),
                              source(10,(*point_samplers)[sampler])});
                op(output,1,{destination(0,temporary,1),source(0,projection,0)});
            }
            op(output,5,{destination(0,temporary,1),source(0,temporary,0),source(2,binding,0)});
            op(output,11,{destination(0,temporary,1),source(0,temporary,0),source(2,binding,0xaa)});
            op(output,10,{destination(0,temporary,1),source(0,temporary,0),source(2,binding,0xff)});
            if (eye) op(output,4,{destination(0,temporary,1),eye,source(2,binding,0x55),source(0,temporary,0)});
            tokens[2]=source(0,temporary);
            if (opcode==93) {
                if (gradient_x<0) {gradient_x=static_cast<int>(allocate());gradient_y=static_cast<int>(allocate());}
                op(output,1,{destination(0,gradient_x,3),tokens[4]});
                op(output,1,{destination(0,gradient_y,3),tokens[5]});
                op(output,5,{destination(0,gradient_x,1),source(0,gradient_x,0),source(2,binding,0)});
                op(output,5,{destination(0,gradient_y,1),source(0,gradient_y,0),source(2,binding,0)});
                tokens[4]=source(0,gradient_x);tokens[5]=source(0,gradient_y);
            }
        }
    }
    append(output,tokens);
}
struct MonoAtlas {
    Words pixel;
    std::vector<std::pair<unsigned,unsigned>> sampler_constants;
    std::vector<std::pair<unsigned,unsigned>> point_samplers;
};
inline MonoAtlas patch_mono_atlas(const Words& pixel,unsigned samplers,unsigned point_samplers=0) {
    if (pixel.empty() || pixel[0]!=0xffff0300u || !samplers || samplers>65535)
        throw std::invalid_argument("Expected PS3 with a nonempty bounded mono atlas mask");
    if (point_samplers&~samplers)
        throw std::invalid_argument("Mono point selection exceeds the admitted atlas mask");
    const auto instructions=parse(pixel);
    const auto info=inspect(instructions);
    const auto constants=info.free(2,224),temporaries=info.free(0,32);
    std::size_t next_temporary=0;
    auto temporary=[&]() {
        if (next_temporary==temporaries.size()) throw std::invalid_argument("No free mono atlas temporary");
        return temporaries[next_temporary++];
    };
    const auto coordinate=temporary();
    std::array<int,16> bindings{};bindings.fill(-1);
    MonoAtlas result;
    for (unsigned sampler=0;sampler<16;++sampler) if (samplers&(1u<<sampler)) {
        bool declared=false;
        for (const auto& instruction:instructions)
            if (instruction.opcode==31 && kind(instruction.tokens[2])==10 &&
                index(instruction.tokens[2])==sampler) {
                if (((instruction.tokens[1]>>27)&15)!=2)
                    throw std::invalid_argument("Mono atlas sampler is not a declared 2D texture");
                declared=true;
            }
        if (!declared) throw std::invalid_argument("Mono atlas sampler is undeclared");
        if (result.sampler_constants.size()==constants.size())
            throw std::invalid_argument("No free mono atlas constant");
        const auto reg=constants[result.sampler_constants.size()];
        bindings[sampler]=static_cast<int>(reg);
        result.sampler_constants.emplace_back(sampler,reg);
    }
    result.pixel={0xffff0300u};
    std::array<int,16> point_bindings{};point_bindings.fill(-1);
    const auto free_samplers=info.free(10,16);
    std::size_t next_sampler=0;
    for (unsigned sampler=0;sampler<16;++sampler) if (point_samplers&(1u<<sampler)) {
        if (next_sampler==free_samplers.size())
            throw std::invalid_argument("No free mono native-point lookup sampler");
        const auto slot=free_samplers[next_sampler++];
        point_bindings[sampler]=static_cast<int>(slot);
        result.point_samplers.emplace_back(sampler,slot);
        op(result.pixel,31,{0x90000000u,destination(10,slot,15)});
    }
    int projection=-1,gradient_x=-1,gradient_y=-1;
    for (const auto& instruction:instructions) {
        if (header(instruction.opcode)) append(result.pixel,instruction.tokens);
        else route_atlas_texture(result.pixel,instruction.tokens,bindings,coordinate,temporary,
                                 projection,gradient_x,gradient_y,0,&point_bindings);
    }
    result.pixel.push_back(65535);
    return result;
}
struct PixelOptions {
    unsigned atlas_samplers=0;
    bool eye_camera=false;
    bool eye_view=true;
    bool scissor=false;
    bool viewport_transform=false;
    bool eye_local_position=false;
    bool ui_plane=false;
    unsigned point_atlas_samplers=0;
};
inline Pair patch_pair(const Words& vertex,const Words& pixel,PixelOptions options={}) {
    if (options.atlas_samplers>65535) throw std::invalid_argument("Pixel atlas mask exceeds SM3 samplers");
    if (options.point_atlas_samplers&~options.atlas_samplers)
        throw std::invalid_argument("Point selection exceeds the admitted stereo atlas mask");
    if (vertex.empty() || pixel.empty() || vertex[0]!=0xfffe0300u || pixel[0]!=0xffff0300u)
        throw std::invalid_argument("Expected VS3/PS3 pair");
    const auto vi=parse(vertex),pi=parse(pixel);
    const auto v=inspect(vi),p=inspect(pi);
    Pair result;
    unsigned varyings=0;
    for (const auto& d:v.declarations) if (d.type==6 && d.usage!=0 && d.usage!=4) ++varyings;
    result.packed=varyings>=10 || p.used[1].count()>=10;
    if (result.packed) {
        bool found=false;
        for (const auto& pd:p.declarations) {
            if (pd.type!=1 || pd.usage!=5) continue;
            bool produced=false;
            for (const auto& vd:v.declarations)
                produced|=vd.type==6 && vd.usage==5 && vd.semantic==pd.semantic;
            if (!produced) continue;
            for (unsigned c=0;c<4;++c) if (!(pd.mask&(1u<<c))) {
                result.eye_semantic=pd.semantic;
                result.component=c;
                found=true;
                break;
            }
            if (found) break;
        }
        if (!found) throw std::invalid_argument("Shader pair lacks a shared spare varying component");
    }
    for (const auto& d:p.declarations)
        if (d.usage==5 && d.semantic==15) throw std::invalid_argument("Pixel eye semantic conflicts");
    auto [ph,pb]=split(pi);
    unsigned pixel_input=0;
    if (result.packed) {
        bool found=false;
        for (const auto& d:p.declarations)
            if (d.type==1 && d.usage==5 && d.semantic==result.eye_semantic && !(d.mask&(1u<<result.component))) {
                pixel_input=d.number;
                found=true;
                break;
            }
        if (!found) throw std::invalid_argument("Pixel packed varying is unavailable");
        extend(ph,1,pixel_input,result.component);
    } else {
        pixel_input=first(p.free(1,10),"No free pixel varying");
        declare(ph,5,15,1,pixel_input,1);
    }
    unsigned ui_input=0;
    if (options.ui_plane) {
        for (const auto& declaration:p.declarations)
            if (declaration.usage==5 && declaration.semantic==14)
                throw std::invalid_argument("Native UI varying semantic conflicts");
        auto free=p.free(1,10);
        free.erase(std::remove(free.begin(),free.end(),pixel_input),free.end());
        ui_input=first(free,"No free native UI pixel varying");
        declare(ph,5,14,1,ui_input,7);
    }
    result.guard=first(p.free(2,224),"No free seam-guard constant");
    const auto temporary=first(p.free(0,32),"No free seam-guard temporary");
    if (!p.used[17][0]) declare(ph,0,0,17,0,3);
    Words guard_prefix;
    op(guard_prefix,4,{destination(0,temporary,1),source(1,pixel_input,result.component*0x55),source(2,result.guard,0x55),source(2,result.guard,0xaa)});
    op(guard_prefix,2,{destination(0,temporary,2),source(17,0,0),source(2,result.guard,0)|0x01000000u});
    op(guard_prefix,5,{destination(0,temporary,1),source(0,temporary,0),source(0,temporary,0x55)});
    op(guard_prefix,1,{destination(0,temporary,14),source(2,result.guard,0xff)});
    op(guard_prefix,65,{destination(0,temporary,15)});
    if (options.atlas_samplers || options.eye_camera || options.scissor || options.viewport_transform ||
        options.eye_local_position || options.ui_plane) {
        const auto constants=p.free(2,224),temps=p.free(0,32);
        std::size_t constant_cursor=0,temp_cursor=1;
        auto constant=[&]() {
            while (constant_cursor<constants.size() && constants[constant_cursor]==result.guard) ++constant_cursor;
            if (constant_cursor==constants.size()) throw std::invalid_argument("No free eye-aware pixel constant");
            return constants[constant_cursor++];
        };
        auto temp=[&]() {
            if (temp_cursor==temps.size()) throw std::invalid_argument("No free eye-aware pixel temporary");
            return temps[temp_cursor++];
        };
        std::array<int,16> sampler_constants{};
        sampler_constants.fill(-1);
        for (unsigned sampler=0;sampler<16;++sampler) if (options.atlas_samplers&(1u<<sampler)) {
            bool declared=false;
            for (const auto& instruction:pi)
                if (instruction.opcode==31 && kind(instruction.tokens[2])==10 &&
                    index(instruction.tokens[2])==sampler) {
                    if (((instruction.tokens[1]>>27)&15)!=2)
                        throw std::invalid_argument("Selected atlas sampler is not a declared 2D texture");
                    declared=true;
                }
            if (!declared) throw std::invalid_argument("Selected atlas sampler is undeclared");
            const auto reg=constant();
            sampler_constants[sampler]=static_cast<int>(reg);
            result.atlas_sampler_constants.emplace_back(sampler,reg);
        }
        Words prefix,rewritten;
        std::array<int,16> point_samplers{};point_samplers.fill(-1);
        const auto free_samplers=p.free(10,16);
        std::size_t next_sampler=0;
        for (unsigned sampler=0;sampler<16;++sampler) if (options.point_atlas_samplers&(1u<<sampler)) {
            if (next_sampler==free_samplers.size())
                throw std::invalid_argument("No free stereo native-point lookup sampler");
            const auto slot=free_samplers[next_sampler++];
            point_samplers[sampler]=static_cast<int>(slot);
            result.atlas_point_samplers.emplace_back(sampler,slot);
            op(ph,31,{0x90000000u,destination(10,slot,15)});
        }
        int local_position=-1,camera=-1,projection=-1,gradient_x=-1,gradient_y=-1;
        if (p.used[17][0] || options.scissor || options.viewport_transform || options.ui_plane) {
            local_position=static_cast<int>(temp());
            result.pixel_position_constant=static_cast<int>(constant());
            if (options.ui_plane) {
                op(prefix,6,{destination(0,temporary,1),source(1,ui_input,0xaa)});
                op(prefix,5,{destination(0,local_position,3),source(1,ui_input),source(0,temporary,0)});
                op(prefix,4,{destination(0,local_position,1),source(0,local_position,0),
                    source(2,result.pixel_position_constant,0),source(2,result.pixel_position_constant,0x55)});
                op(prefix,4,{destination(0,local_position,2),source(0,local_position,0x55),
                    source(2,result.pixel_position_constant,0xaa),source(2,result.pixel_position_constant,0xff)});
            } else {
                op(prefix,1,{destination(0,local_position,3),source(17,0)});
                op(prefix,4,{destination(0,local_position,1),
                    source(1,pixel_input,result.component*0x55)|0x01000000u,
                    source(2,result.pixel_position_constant,0),source(0,local_position,0)});
                op(prefix,2,{destination(0,local_position,1),source(0,local_position,0),
                    source(2,result.pixel_position_constant,0x55)|0x01000000u});
            }
        }
        if (options.scissor || options.viewport_transform) {
            result.scissor_constant=static_cast<int>(constant());
            result.scissor_delta_constant=static_cast<int>(constant());
            const auto eye_scissor=temp();
            // SM3 permits only one distinct constant register per instruction.
            op(prefix,1,{destination(0,eye_scissor,15),source(2,result.scissor_constant)});
            op(prefix,4,{destination(0,eye_scissor,15),source(1,pixel_input,result.component*0x55),
                source(2,result.scissor_delta_constant),source(0,eye_scissor)});
            op(prefix,2,{destination(0,temporary,3),source(0,local_position),
                source(0,eye_scissor)|0x01000000u});
            op(prefix,1,{destination(0,temporary,12),source(2,result.guard,0xff)});
            op(prefix,65,{destination(0,temporary,15)});
            op(prefix,2,{destination(0,temporary,3),source(0,eye_scissor,0xee),
                source(0,local_position)|0x01000000u});
            op(prefix,1,{destination(0,temporary,12),source(2,result.guard,0xff)});
            op(prefix,65,{destination(0,temporary,15)});
        }
        if (options.eye_camera) {
            bool mapped=false;
            for (const auto& entry:p.constants)
                mapped|=entry.name=="CameraPositionPS" && entry.set==2 && entry.number==6 && entry.count==1;
            if (mapped) {
                camera=static_cast<int>(temp());
                result.camera_base_constant=static_cast<int>(constant());
                result.camera_delta_constant=static_cast<int>(constant());
                op(prefix,4,{destination(0,temporary,1),source(1,pixel_input,result.component*0x55),
                    source(2,result.guard,0x55),source(2,result.guard,0xaa)});
                op(prefix,1,{destination(0,camera,15),source(2,result.camera_base_constant)});
                op(prefix,4,{destination(0,camera,15),source(0,temporary,0),
                    source(2,result.camera_delta_constant),source(0,camera)});
            }
        }
        if (options.atlas_samplers) {
            // Perspective interpolation must not turn the discrete eye label into a fractional texture offset.
            op(prefix,2,{destination(0,temporary,4),source(1,pixel_input,result.component*0x55),
                source(2,result.atlas_sampler_constants.front().second,0)|0x01000000u});
            op(prefix,88,{destination(0,temporary,4),source(0,temporary,0xaa),
                source(2,result.guard,0xaa)|0x01000000u,source(2,result.guard,0xff)});
        }
        for (const auto& instruction:pi) {
            if (header(instruction.opcode)) continue;
            auto tokens=instruction.tokens;
            for (std::size_t n=1;n<tokens.size();++n) {
                if (local_position>=0 && kind(tokens[n])==17 && index(tokens[n])==0)
                    tokens[n]=replace(tokens[n],0,local_position);
                if (camera>=0 && kind(tokens[n])==2 && index(tokens[n])==6) {
                    if (tokens[n]&0x2000) throw std::invalid_argument("Relative pixel camera addressing is unsupported");
                    tokens[n]=replace(tokens[n],0,camera);
                }
            }
            route_atlas_texture(rewritten,std::move(tokens),sampler_constants,temporary,temp,
                                projection,gradient_x,gradient_y,source(0,temporary,0xaa),&point_samplers);
        }
        append(guard_prefix,prefix);
        pb=std::move(rewritten);
    }
    result.pixel={0xffff0300u};
    append(result.pixel,canonical(ph)); append(result.pixel,guard_prefix); append(result.pixel,pb); result.pixel.push_back(65535);
    unsigned position_output=0,positions=0,eye_output=0,ui_output=0;
    for (const auto& d:v.declarations) {
        if (d.type==6 && d.usage==0 && d.semantic==0) {position_output=d.number;++positions;}
        if (d.usage==5 && ((d.type==1 && d.semantic==15) ||
            (!result.packed && d.type==6 && d.semantic==result.eye_semantic)))
            throw std::invalid_argument("Vertex eye semantic conflicts");
    }
    if (positions!=1) throw std::invalid_argument("Vertex shader lacks exactly one position output");
    std::vector<unsigned> mapped;
    if (v.view_projection && options.eye_view) {
        mapped={0,1,2,3};
        for (const auto& name:std::array<std::pair<const char*,unsigned>,2>{{{"CameraPositionVS",4},{"PreViewTranslation",5}}})
            for (const auto& constant:v.constants)
                if (constant.name==name.first && constant.number==name.second) {mapped.push_back(name.second);break;}
    }
    if (options.ui_plane && options.eye_view) throw std::invalid_argument("UI plane must not replace the native view matrix");
    result.view_texture=!mapped.empty() || options.ui_plane;
    const unsigned fetched=options.ui_plane?4:unsigned(mapped.size());
    const auto temps=v.free(0,32);
    if (temps.size()<fetched+2) throw std::invalid_argument("No free vertex transformation temporaries");
    const auto input=first(v.free(1,16),"No free eye vertex input");
    const auto coordinate=first(v.free(2,256),"No free vertex coordinate constant");
    result.coordinate_constant=static_cast<int>(coordinate);
    if (options.viewport_transform) {
        const auto free=v.free(2,256);
        if (free.size()<2) throw std::invalid_argument("No free native atlas viewport constant");
        result.viewport_constant=static_cast<int>(free[1]);
    }
    const auto uv=temps[fetched],position=temps[fetched+1];
    if (result.packed) {
        unsigned found=0;
        for (const auto& d:v.declarations) if (d.type==6 && d.usage==5 && d.semantic==result.eye_semantic) {
            eye_output=d.number;
            ++found;
        }
        if (found!=1) throw std::invalid_argument("Vertex shader does not uniquely produce packed varying");
    } else eye_output=first(v.free(6,12),"No free eye vertex output");
    if (options.ui_plane) {
        for (const auto& declaration:v.declarations)
            if (declaration.usage==5 && declaration.semantic==14)
                throw std::invalid_argument("Native UI vertex varying semantic conflicts");
        auto free=v.free(6,12);
        free.erase(std::remove(free.begin(),free.end(),eye_output),free.end());
        ui_output=first(free,"No free native UI vertex varying");
    }
    if (result.view_texture) result.sampler=first(v.free(10,4),"No free vertex view-texture sampler");
    auto [vh,unused]=split(vi);
    declare(vh,5,15,1,input,15);
    if (result.packed) extend(vh,6,eye_output,result.component);
    else declare(vh,5,result.eye_semantic,6,eye_output,1);
    if (options.ui_plane) declare(vh,5,14,6,ui_output,7);
    if (result.view_texture) op(vh,31,{0x90000000u,destination(10,result.sampler,15)});
    op(vh,81,{destination(2,coordinate,15),0x3e000000u,0x3f000000u,0x3d800000u,0x3e800000u});
    Words prefix,body,suffix;
    if (result.view_texture) {
        op(prefix,1,{destination(0,uv,1),source(2,coordinate,0xaa)});
        op(prefix,4,{destination(0,uv,2),source(1,input,0),source(2,coordinate,0x55),source(2,coordinate,0xff)});
        op(prefix,2,{destination(0,uv,12),source(2,coordinate),source(2,coordinate)|0x01000000u});
        const unsigned last=options.ui_plane?3:mapped.back();
        for (unsigned row=0;row<=last;++row) {
            const auto found=std::find(mapped.begin(),mapped.end(),row);
            if (options.ui_plane || found!=mapped.end())
                op(prefix,95,{destination(0,temps[options.ui_plane?row:unsigned(found-mapped.begin())],15),source(0,uv),source(10,result.sampler)});
            op(prefix,2,{destination(0,uv,1),source(0,uv,0),source(2,coordinate,0)});
        }
    }
    bool active=false;
    for (const auto& instruction:vi) {
        if (instruction.opcode==25 || instruction.opcode==26 || instruction.opcode==28 || instruction.opcode==30)
            throw std::invalid_argument("Vertex subroutine could bypass stereo suffix");
        if (!header(instruction.opcode)) active=true;
        if (!active) continue;
        auto tokens=instruction.tokens;
        if (!header(instruction.opcode)) for (std::size_t n=1;n<tokens.size();++n) {
            const auto type=kind(tokens[n]),number=index(tokens[n]);
            const auto found=std::find(mapped.begin(),mapped.end(),number);
            if (type==2 && found!=mapped.end()) {
                if (tokens[n]&0x2000) throw std::invalid_argument("Relative view constant addressing is unsupported");
                tokens[n]=replace(tokens[n],0,temps[found-mapped.begin()]);
            }
            if (type==6 && number==position_output) {
                if (n!=1) throw std::invalid_argument("Vertex shader reads its position output");
                tokens[n]=replace(tokens[n],0,position);
            }
        }
        append(body,tokens);
    }
    if (options.ui_plane) {
        op(suffix,1,{destination(6,ui_output,7),source(0,position,0xf4)});
        for (unsigned row=0;row<4;++row)
            op(suffix,9,{destination(0,uv,1u<<row),source(0,position),source(0,temps[row])});
        op(suffix,1,{destination(0,position),source(0,uv)});
    }
    if (options.viewport_transform)
        op(suffix,2,{destination(0,uv,2),source(1,input,0),source(2,result.viewport_constant,0x55)});
    else
        op(suffix,2,{destination(0,uv,2),source(1,input,0),source(2,coordinate,0x55)|0x01000000u});
    op(suffix,5,{destination(0,uv,1),source(0,position,0xff),source(0,uv,0x55)});
    op(suffix,4,{destination(6,position_output,1),source(0,position,0),
        options.viewport_transform?source(2,result.viewport_constant,0):source(2,coordinate,0x55),
        source(0,uv,0)});
    if (options.viewport_transform) {
        op(suffix,5,{destination(0,uv,1),source(0,position,0xff),source(2,result.viewport_constant,0xff)});
        op(suffix,4,{destination(6,position_output,2),source(0,position,0x55),
            source(2,result.viewport_constant,0xaa),source(0,uv,0)});
        op(suffix,1,{destination(6,position_output,12),source(0,position)});
    } else op(suffix,1,{destination(6,position_output,14),source(0,position)});
    op(suffix,1,{destination(6,eye_output,1u<<result.component),source(1,input,0)});
    result.vertex={0xfffe0300u};
    append(result.vertex,canonical(vh));append(result.vertex,prefix);append(result.vertex,body);append(result.vertex,suffix);
    result.vertex.push_back(65535);
    return result;
}
} // namespace olvr::sm3
