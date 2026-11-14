#pragma once
#include "outlast_native_contracts_generated.h"
#include <iterator>

namespace olvr {

struct NativeMappedRange {
    std::uint32_t source,target,size;
};
struct OutlastNativeBindings {
    std::map<std::uint32_t,std::uint32_t> addresses;
    std::vector<NativeMappedRange> ranges;
    std::uint32_t rva(std::uint32_t source) const {
        const auto found=addresses.find(source);
        if (found!=addresses.end()) return found->second;
        for (const auto range:ranges)
            if (source>=range.source && source-range.source<range.size)
                return range.target+(source-range.source);
        throw std::invalid_argument("Unresolved native ABI address "+std::to_string(source));
    }
    std::optional<std::uint32_t> original_rva(std::uint32_t target) const {
        for (const auto range:ranges)
            if (target>=range.target && target-range.target<range.size)
                return range.source+(target-range.target);
        return std::nullopt;
    }
};

inline OutlastNativeBindings resolve_outlast_native_bindings(const OutlastNativePE& image,
                                                             const OutlastNativeFunctions& functions) {
    const OutlastNativeContracts discovery(image,functions);
    std::map<std::uint32_t,std::vector<NativeFunctionMatch>> domains;
    for (const auto& contract:outlast_native_contracts) {
        auto matches=discovery.find(contract);
        if (matches.empty())
            throw std::invalid_argument("Required native code/layout contract is missing: "+
                                        std::to_string(contract.identity));
        domains.emplace(contract.identity,std::move(matches));
    }
    // D3D9's view and indexed entries must belong to the same native dispatch table.
    std::set<std::uint32_t> views,indexed;
    std::set<std::pair<std::uint32_t,std::uint32_t>> dispatch_pairs;
    const auto relocations=image.absolute_relocations();
    for (const auto& section:image.sections()) {
        if ((section.flags&0xe0000000u)!=0x40000000u) continue;
        const auto size=std::min(section.size,section.raw_size);
        if (size<29*8) continue;
        for (std::uint32_t offset=0;offset<=size-29*8;offset+=8) {
            const auto slot=section.rva+offset;
            if (!relocations.count(slot) || !relocations.count(slot+28*8)) continue;
            const auto view=image.value<std::uint64_t>(slot);
            const auto draw=image.value<std::uint64_t>(slot+28*8);
            for (const auto& v:domains.at(0xbf7d40))
                if (view==image.preferred_base()+v.entry)
                    for (const auto& d:domains.at(0xbf5b60))
                        if (draw==image.preferred_base()+d.entry) {
                            views.insert(v.entry);indexed.insert(d.entry);
                            dispatch_pairs.emplace(v.entry,d.entry);
                        }
        }
    }
    for (const auto identity:{0xbf7d40u,0xbf5b60u}) {
        auto& candidates=domains.at(identity);
        const auto& allowed=identity==0xbf7d40u?views:indexed;
        candidates.erase(std::remove_if(candidates.begin(),candidates.end(),
            [&](const auto& candidate) {return !allowed.count(candidate.entry);}),candidates.end());
        if (candidates.empty()) throw std::invalid_argument("Native D3D9 dispatch relationship differs");
    }
    bool changed=true;
    while (changed) {
        changed=false;
        std::map<std::uint32_t,std::set<std::uint32_t>> allowed;
        for (const auto& domain:domains) {
            std::map<std::uint32_t,std::set<std::uint32_t>> values;
            for (const auto& candidate:domain.second)
                for (const auto binding:candidate.addresses) values[binding.first].insert(binding.second);
            for (const auto& value:values) {
                const auto found=allowed.find(value.first);
                if (found==allowed.end()) allowed.emplace(value.first,value.second);
                else {
                    std::set<std::uint32_t> common;
                    std::set_intersection(found->second.begin(),found->second.end(),
                        value.second.begin(),value.second.end(),std::inserter(common,common.end()));
                    found->second=std::move(common);
                }
            }
        }
        for (auto& domain:domains) {
            auto& candidates=domain.second;
            const auto before=candidates.size();
            candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const auto& candidate) {
                for (const auto binding:candidate.addresses)
                    if (!allowed.at(binding.first).count(binding.second)) return true;
                if (domain.first==0xbf7d40u || domain.first==0xbf5b60u) {
                    const bool view=domain.first==0xbf7d40u;
                    const auto& other=domains.at(view?0xbf5b60u:0xbf7d40u);
                    if (std::none_of(other.begin(),other.end(),[&](const auto& peer) {
                        return dispatch_pairs.count(view?std::make_pair(candidate.entry,peer.entry):
                                                         std::make_pair(peer.entry,candidate.entry))!=0;
                    })) return true;
                }
                return false;
            }),candidates.end());
            if (candidates.empty())
                throw std::invalid_argument("Conflicting native reference identities: "+std::to_string(domain.first));
            changed|=before!=candidates.size();
        }
    }
    OutlastNativeBindings result;
    for (const auto& contract:outlast_native_contracts) {
        const auto& candidates=domains.at(contract.identity);
        if (candidates.size()!=1)
            throw std::invalid_argument("Ambiguous required native binding: "+std::to_string(contract.identity)+
                                        " ("+std::to_string(candidates.size())+" candidates)");
        const auto& candidate=candidates.front();
        for (const auto binding:candidate.addresses) {
            const auto added=result.addresses.emplace(binding);
            if (!added.second && added.first->second!=binding.second)
                throw std::invalid_argument("Conflicting resolved native addresses");
        }
        for (const auto range:candidate.ranges) result.ranges.push_back({range.source,range.target,range.size});
    }
    for (const auto base:{0x201e310u,0x201e370u}) {
        const auto target=result.rva(base);
        const auto* section=image.section(target,16);
        if (!section || (section->flags&0xe0000000u)!=0xc0000000u)
            throw std::invalid_argument("Native array layout has no writable extent");
        const auto added=result.addresses.emplace(base+8,target+8);
        if (!added.second && added.first->second!=target+8)
            throw std::invalid_argument("Native array/count alias differs");
    }
    if (result.rva(0x121a4c0)!=image.entry())
        throw std::invalid_argument("Native entry point does not match its verified code contract");
    if (!dispatch_pairs.count({result.rva(0xbf7d40),result.rva(0xbf5b60)}))
        throw std::invalid_argument("Resolved native rendering entries do not share a dispatch table");
    for (const auto source:outlast_native_initial_globals) {
        const auto rva=result.rva(source.rva);
        const auto* section=image.section(rva,source.size);
        if (!section || (section->flags&0xe0000000u)!=0xc0000000u)
            throw std::invalid_argument("Required native global has incompatible mapping permissions");
        const auto data=image.initial_data(rva,source.size);
        if (std::any_of(data.begin(),data.end(),[](auto value) {return value!=0;}))
            throw std::invalid_argument("Required native global has incompatible initial data");
    }
    return result;
}

}
