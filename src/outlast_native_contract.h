#pragma once
#include "outlast_native_pe.h"
#include "sha256.h"
#include <map>
#include <optional>

namespace olvr {

enum class NativeReferenceKind { relative,absolute };
struct NativeImportContract {
    const char* dll;
    const char* name;
    std::uint16_t ordinal;
};
struct NativeImmutableContract {
    std::uint32_t size;
    const char* sha256;
};
struct NativeCodeReference {
    std::uint32_t position,instruction,target,flags;
    std::uint8_t width,instruction_size;
    NativeReferenceKind kind;
    const NativeImportContract* imported=nullptr;
    const NativeImmutableContract* immutable=nullptr;
};
struct NativeFunctionContract {
    std::uint32_t identity,size;
    const char* sha256;
    const NativeCodeReference* references;
    std::size_t reference_count;
    bool leaf=false;
    std::array<unsigned char,16> prefix{};
    std::uint8_t prefix_size=0;
    const NativeCodeRange* source_ranges=nullptr;
    std::size_t range_count=0;
};
struct NativeMatchedRange {std::uint32_t source,target,size;};
struct NativeFunctionMatch {
    std::uint32_t entry;
    std::map<std::uint32_t,std::uint32_t> addresses;
    std::vector<NativeMatchedRange> ranges;
};

class OutlastNativeContracts {
    const OutlastNativePE& image_;
    const OutlastNativeFunctions& functions_;
    std::map<std::size_t,std::vector<const OutlastNativeFunctions::Group*>> sizes_;
    std::map<std::uint32_t,OutlastNativePE::Import> imports_;
    std::set<std::uint32_t> absolute_relocations_;
    static std::optional<std::uint32_t> instruction_rva(const OutlastNativeFunctions::Group& group,
                                                       std::uint32_t offset,unsigned size) {
        std::size_t consumed=0;
        for (const auto range:group.ranges) {
            if (offset>=consumed && offset-consumed<range.size) {
                if (size>range.size-(offset-consumed)) return std::nullopt;
                return range.rva+std::uint32_t(offset-consumed);
            }
            consumed+=range.size;
        }
        return std::nullopt;
    }
    std::optional<NativeFunctionMatch> match(const NativeFunctionContract& contract,
                                            const OutlastNativeFunctions::Group& group,
                                            std::string* failure=nullptr) const {
        const auto reject=[&](const char* reason,std::size_t index)->std::optional<NativeFunctionMatch> {
            if (failure) *failure=std::string(reason)+" at reference "+std::to_string(index);
            return std::nullopt;
        };
        std::vector<unsigned char> bytes;bytes.reserve(group.size);
        for (const auto range:group.ranges) {
            const auto part=image_.body(range.rva,range.size);
            bytes.insert(bytes.end(),part.begin(),part.end());
        }
        NativeFunctionMatch result{group.entry,{{contract.identity,group.entry}},{}};
        std::size_t consumed=0;
        for (std::size_t index=0;index<contract.range_count;++index) {
            const auto source=contract.source_ranges[index];
            std::size_t position=consumed,left=source.size,source_offset=0,target_offset=0;
            for (const auto target:group.ranges) {
                if (position>=target_offset+target.size) {target_offset+=target.size;continue;}
                const auto begin=position-target_offset;
                const auto size=std::min(left,std::size_t(target.size)-begin);
                result.ranges.push_back({source.rva+std::uint32_t(source_offset),
                    target.rva+std::uint32_t(begin),std::uint32_t(size)});
                source_offset+=size;position+=size;left-=size;
                if (!left) break;
                target_offset+=target.size;
            }
            if (left) return reject("Source code range exceeds matched code",index);
            consumed+=source.size;
        }
        for (std::size_t index=0;index<contract.reference_count;++index) {
            const auto& reference=contract.references[index];
            const auto origin=instruction_rva(group,reference.instruction,reference.instruction_size);
            if (!origin) return reject("Instruction crosses an unwind fragment",index);
            std::uint64_t raw=0;std::memcpy(&raw,bytes.data()+reference.position,reference.width);
            std::int64_t target=0;
            if (reference.kind==NativeReferenceKind::relative) {
                std::int64_t displacement=0;
                if (reference.width==1) displacement=std::int8_t(raw);
                else if (reference.width==2) displacement=std::int16_t(raw);
                else displacement=std::int32_t(raw);
                target=std::int64_t(*origin)+reference.instruction_size+displacement;
            } else {
                const auto field=*origin+(reference.position-reference.instruction);
                if (!absolute_relocations_.count(field))
                    return reject("Absolute reference has no loader relocation",index);
                if (raw<image_.preferred_base() || raw-image_.preferred_base()>=image_.extent())
                    return reject("Absolute reference exceeds the image",index);
                target=std::int64_t(raw-image_.preferred_base());
            }
            if (target<0 || std::uint64_t(target)>=image_.extent())
                return reject("Relative reference exceeds the image",index);
            const auto rva=std::uint32_t(target);
            const auto* section=image_.section(rva);
            if (reference.flags) {
                if (!section || (section->flags&0xe0000000u)!=reference.flags)
                    return reject("Reference mapping permissions differ",index);
            } else if (rva!=0) return reject("Image-base reference differs",index);
            if (reference.immutable) {
                const auto size=reference.immutable->size;
                if (!section || rva-section->rva>=section->raw_size ||
                    size>section->raw_size-(rva-section->rva))
                    return reject("Immutable reference has no file-backed extent",index);
                const auto* data=image_.data(rva,size);
                if (sha256_bytes(data,size)!=reference.immutable->sha256)
                    return reject("Immutable native identity differs",index);
            }
            if (reference.imported) {
                const auto imported=imports_.find(rva);
                if (imported==imports_.end() || imported->second.dll!=reference.imported->dll ||
                    imported->second.name!=reference.imported->name ||
                    imported->second.ordinal!=reference.imported->ordinal)
                    return reject("Import identity differs",index);
            }
            if (reference.target) {
                const auto added=result.addresses.emplace(reference.target,rva);
                if (!added.second && added.first->second!=rva)
                    return reject("Reference identity has conflicting addresses",index);
            }
            std::fill_n(bytes.data()+reference.position,reference.width,0);
        }
        if (sha256_bytes(bytes.data(),bytes.size())!=contract.sha256)
            return reject("Normalized native code differs",contract.reference_count);
        return result;
    }
    static void validate(const NativeFunctionContract& contract) {
        if (!contract.identity || !contract.size || !contract.sha256 ||
            std::strlen(contract.sha256)!=64 || (contract.reference_count && !contract.references))
            throw std::invalid_argument("Invalid compiled native function contract");
        for (unsigned index=0;index<64;++index)
            if (!((contract.sha256[index]>='0' && contract.sha256[index]<='9') ||
                  (contract.sha256[index]>='a' && contract.sha256[index]<='f')))
                throw std::invalid_argument("Invalid compiled native digest");
        if ((contract.leaf && (!contract.prefix_size || contract.prefix_size>16 ||
                              contract.prefix_size>contract.size)) ||
            (!contract.leaf && contract.prefix_size))
            throw std::invalid_argument("Invalid compiled native leaf locator");
        if (contract.range_count && !contract.source_ranges)
            throw std::invalid_argument("Missing compiled native source ranges");
        std::uint64_t extent=0;
        for (std::size_t index=0;index<contract.range_count;++index) {
            const auto range=contract.source_ranges[index];
            if (!range.size || std::uint64_t(range.rva)+range.size>0x100000000ull ||
                (index && range.rva<std::uint64_t(contract.source_ranges[index-1].rva)+
                    contract.source_ranges[index-1].size))
                throw std::invalid_argument("Invalid compiled native source range");
            extent+=range.size;
        }
        if (contract.range_count && extent!=contract.size)
            throw std::invalid_argument("Compiled native source ranges differ from the code extent");
        for (std::size_t index=0;index<contract.reference_count;++index) {
            const auto& reference=contract.references[index];
            if (reference.imported && (!reference.imported->dll || !*reference.imported->dll ||
                                      !reference.imported->name))
                throw std::invalid_argument("Invalid compiled native import identity");
            if (reference.immutable && (!reference.immutable->size || reference.immutable->size>512 ||
                !reference.immutable->sha256 || std::strlen(reference.immutable->sha256)!=64 ||
                reference.flags!=0x40000000u))
                throw std::invalid_argument("Invalid compiled immutable native identity");
            const bool relative=reference.kind==NativeReferenceKind::relative;
            if ((relative && reference.width!=1 && reference.width!=2 && reference.width!=4) ||
                (!relative && (reference.kind!=NativeReferenceKind::absolute || reference.width!=8)) ||
                !reference.instruction_size || reference.instruction_size>15 ||
                reference.instruction>=contract.size ||
                reference.instruction_size>contract.size-reference.instruction ||
                reference.position<reference.instruction ||
                reference.position-reference.instruction>reference.instruction_size ||
                reference.width>reference.instruction_size-(reference.position-reference.instruction) ||
                (reference.flags&~0xe0000000u) ||
                (reference.flags && !(reference.flags&0x40000000u)))
                throw std::invalid_argument("Invalid compiled native reference boundary");
            if (contract.leaf)
                for (unsigned byte=0;byte<reference.width;++byte)
                    if (reference.position+byte<contract.prefix_size &&
                        contract.prefix[reference.position+byte])
                        throw std::invalid_argument("Native leaf locator contains unmasked reference bytes");
            for (std::size_t previous=0;previous<index;++previous) {
                const auto& other=contract.references[previous];
                if (reference.position<other.position+other.width &&
                    other.position<reference.position+reference.width)
                    throw std::invalid_argument("Overlapping compiled native reference masks");
            }
        }
    }
public:
    OutlastNativeContracts(const OutlastNativePE& image,const OutlastNativeFunctions& functions)
        :image_(image),functions_(functions) {
        for (const auto& group:functions.groups()) sizes_[group.size].push_back(&group);
        for (auto imported:image.imports()) {
            const auto slot=imported.slot;
            if (!imports_.emplace(slot,std::move(imported)).second)
                throw std::invalid_argument("Overlapping native import slots");
        }
        absolute_relocations_=image.absolute_relocations();
    }
    std::vector<NativeFunctionMatch> find(const NativeFunctionContract& contract) const {
        validate(contract);
        std::vector<NativeFunctionMatch> matches;
        if (contract.leaf) {
            std::array<unsigned char,16> mask{};mask.fill(0xff);
            for (std::size_t index=0;index<contract.reference_count;++index) {
                const auto& reference=contract.references[index];
                for (unsigned byte=0;byte<reference.width;++byte)
                    if (reference.position+byte<mask.size()) mask[reference.position+byte]=0;
            }
            if (!mask[0]) throw std::invalid_argument("Native leaf locator has no initial opcode");
            for (const auto& section:image_.sections()) {
                const auto size=std::min(section.size,section.raw_size);
                if (!(section.flags&0x20000000) || size<contract.size) continue;
                const auto* begin=image_.data(section.rva,size);
                const auto* stop=begin+(size-contract.size)+1;
                for (const auto* cursor=begin;cursor<stop;++cursor) {
                    if (*cursor!=contract.prefix[0]) continue;
                    bool located=true;
                    for (unsigned byte=1;byte<contract.prefix_size;++byte)
                        if ((cursor[byte]&mask[byte])!=contract.prefix[byte]) {located=false;break;}
                    if (!located) continue;
                    const auto entry=section.rva+std::uint32_t(cursor-begin);
                    auto candidate=match(contract,{entry,{{entry,contract.size}},contract.size});
                    if (candidate) matches.push_back(std::move(*candidate));
                }
            }
            return matches;
        }
        const auto candidates=sizes_.find(contract.size);
        if (candidates==sizes_.end()) return matches;
        for (const auto* group:candidates->second) {
            auto candidate=match(contract,*group);
            if (candidate) matches.push_back(std::move(*candidate));
        }
        return matches;
    }
    std::optional<NativeFunctionMatch> at(const NativeFunctionContract& contract,std::uint32_t entry) const {
        validate(contract);
        if (contract.leaf) {
            const auto* section=image_.section(entry,contract.size);
            if (!section || !(section->flags&0x20000000) ||
                entry-section->rva>=section->raw_size || contract.size>section->raw_size-(entry-section->rva))
                return std::nullopt;
            return match(contract,{entry,{{entry,contract.size}},contract.size});
        }
        const auto* group=functions_.containing(entry);
        if (!group || group->entry!=entry || group->size!=contract.size) return std::nullopt;
        return match(contract,*group);
    }
    std::string diagnose(const NativeFunctionContract& contract,std::uint32_t entry) const {
        validate(contract);
        std::string failure;
        if (contract.leaf) {
            const auto* section=image_.section(entry,contract.size);
            if (!section || entry-section->rva>=section->raw_size ||
                contract.size>section->raw_size-(entry-section->rva))
                return "Leaf has no bounded code mapping";
            match(contract,{entry,{{entry,contract.size}},contract.size},&failure);
        } else {
            const auto* group=functions_.containing(entry);
            if (!group || group->entry!=entry || group->size!=contract.size)
                return "Primary native code extent differs";
            match(contract,*group,&failure);
        }
        return failure;
    }
};

}
