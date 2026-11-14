#pragma once
#include "sha256.h"
#include "outlast_native_profile.h"
#include "outlast_native_pe.h"
#include "outlast_native_bindings.h"
#include <cstring>
#include <type_traits>
#include <vector>

namespace olvr {

inline constexpr const char* outlast_retail_sha256=
    "06c1c37c7f1d0bd0296328ff977ff847bee82f878fb4d79f4282a026aef68a96";

class OutlastNativeImage {
    std::vector<unsigned char> bytes_;
    std::size_t sections_=0;
    unsigned section_count_=0;
    struct Section {
        std::string name;
        std::uint32_t rva,size,raw,raw_size,flags;
    };
    std::vector<Section> mapped_;
    OutlastNativeBindings bindings_;
    std::set<std::uint32_t> relocations_;
    std::uint64_t preferred_base_=0;
    bool canonical_=true;
    template<class T> T read(std::size_t offset) const {
        static_assert(std::is_integral_v<T>);
        if (offset>bytes_.size() || sizeof(T)>bytes_.size()-offset)
            throw std::invalid_argument("Native PE field exceeds the file extent");
        T value{};std::memcpy(&value,bytes_.data()+offset,sizeof(value));return value;
    }
    void validate_profile(std::size_t optional) const {
        const auto image_size=read<std::uint32_t>(optional+56);
        const auto headers=read<std::uint32_t>(optional+60);
        const auto alignment=read<std::uint32_t>(optional+36);
        if (read<std::uint64_t>(optional+24)!=0x140000000ull ||
            image_size>128*1024*1024 || alignment<512 || alignment>4096 || (alignment&(alignment-1)) ||
            headers<sections_+section_count_*40 || headers>4096 || headers>bytes_.size() || headers%alignment ||
            read<std::uint32_t>(optional+32)!=4096 || read<std::uint32_t>(optional+16)!=0x121a4c0 ||
            read<std::uint32_t>(optional+108)!=outlast_native_directories.size())
            throw std::invalid_argument("Unsupported native PE entry point, alignment or image layout");
        std::uint64_t last=0;
        unsigned resources=0;
        std::array<bool,outlast_native_sections.size()> found{};
        for (const auto& section:mapped_) {
            const auto end=std::uint64_t(section.rva)+section.size;
            if (!section.size || section.rva<4096 || section.rva%4096 || end>image_size ||
                section.raw_size>bytes_.size() || section.raw_size%alignment || section.raw>bytes_.size()-section.raw_size ||
                (section.raw_size && (section.raw<headers || section.raw%alignment)))
                throw std::invalid_argument("Native section exceeds its virtual or file mapping");
            for (const auto& other:mapped_) {
                if (&section==&other) continue;
                if ((section.rva<std::uint64_t(other.rva)+other.size && other.rva<end) ||
                    (section.raw_size && other.raw_size &&
                     section.raw<std::uint64_t(other.raw)+other.raw_size &&
                     other.raw<std::uint64_t(section.raw)+section.raw_size))
                    throw std::invalid_argument("Overlapping native section mappings");
            }
            last=std::max(last,end);
            if (section.name==".rsrc") {
                if (++resources!=1 || section.flags!=0x40000040 ||
                    read<std::uint32_t>(optional+112+2*8)!=section.rva ||
                    read<std::uint32_t>(optional+116+2*8)>section.size)
                    throw std::invalid_argument("Invalid non-executable resource mapping");
                continue;
            }
            const auto profile=std::find_if(outlast_native_sections.begin(),outlast_native_sections.end(),
                [&](const auto& expected) {return expected.name==section.name;});
            if (profile==outlast_native_sections.end())
                throw std::invalid_argument("Unsupported native section: "+section.name);
            const auto index=std::size_t(profile-outlast_native_sections.begin());
            if (found[index] || section.rva!=profile->rva || section.size!=profile->size ||
                section.flags!=profile->flags)
                throw std::invalid_argument("Native ABI layout mismatch in "+section.name+
                    "; this build needs a validated compatibility profile");
            found[index]=true;
            std::vector<unsigned char> content(section.size,0);
            std::copy_n(bytes_.data()+section.raw,std::min(section.raw_size,section.size),content.data());
            for (const auto range:outlast_native_debug)
                if (range.rva>=section.rva && std::uint64_t(range.rva)+range.size<=end)
                    std::fill_n(content.data()+(range.rva-section.rva),range.size,0);
            if (sha256_bytes(content.data(),content.size())!=profile->sha256)
                throw std::invalid_argument("Native ABI content mismatch in "+section.name+
                    "; this build needs a validated compatibility profile");
        }
        if (resources!=1 || std::find(found.begin(),found.end(),false)!=found.end() ||
            (last+4095)/4096*4096!=image_size)
            throw std::invalid_argument("Missing native ABI sections or incorrect image extent");
        for (unsigned index=0;index<outlast_native_directories.size();++index) {
            if (index==2) continue;
            const auto rva=read<std::uint32_t>(optional+112+index*8);
            const auto size=read<std::uint32_t>(optional+116+index*8);
            if (index==4) {
                if ((rva==0)!=(size==0) || rva%8 || rva>bytes_.size() || size>bytes_.size()-rva)
                    throw std::invalid_argument("Invalid optional certificate file range");
            } else if (rva!=outlast_native_directories[index].rva || size!=outlast_native_directories[index].size)
                throw std::invalid_argument("Unsupported native PE directory "+std::to_string(index));
        }
    }
public:
    explicit OutlastNativeImage(const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if (!input) throw std::runtime_error("Cannot read the selected retail Outlast image");
        const auto size=input.tellg();
        if (size<64 || size>128*1024*1024)
            throw std::invalid_argument("Outlast image extent is outside the supported PE bounds");
        bytes_.resize(std::size_t(size));input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes_.data()),size))
            throw std::runtime_error("Retail Outlast image read was incomplete");
        if (read<std::uint16_t>(0)!=0x5a4d) throw std::invalid_argument("Invalid retail DOS signature");
        const auto pe=std::size_t(read<std::uint32_t>(0x3c));
        if (pe>bytes_.size() || bytes_.size()-pe<24 ||
            read<std::uint32_t>(pe)!=0x4550 || read<std::uint16_t>(pe+4)!=0x8664)
            throw std::invalid_argument("Retail image is not a native x64 PE");
        const auto optional_size=read<std::uint16_t>(pe+20);
        if (optional_size<240 || !(read<std::uint16_t>(pe+22)&2) ||
            (read<std::uint16_t>(pe+22)&0x2000) || read<std::uint16_t>(pe+24)!=0x20b ||
            !read<std::uint64_t>(pe+48))
            throw std::invalid_argument("Retail image has an unexpected PE32+ layout or preferred base");
        section_count_=read<std::uint16_t>(pe+6);
        sections_=pe+24+optional_size;
        if (!section_count_ || section_count_>96 || sections_>bytes_.size() ||
            section_count_>(bytes_.size()-sections_)/40)
            throw std::invalid_argument("Retail section table exceeds the image");
        for (unsigned index=0;index<section_count_;++index) {
            const auto offset=sections_+index*40;
            const auto* name=reinterpret_cast<const char*>(bytes_.data()+offset);
            mapped_.push_back({std::string(name,std::find(name,name+8,'\0')),
                read<std::uint32_t>(offset+12),read<std::uint32_t>(offset+8),
                read<std::uint32_t>(offset+20),read<std::uint32_t>(offset+16),read<std::uint32_t>(offset+36)});
        }
        const OutlastNativePE mapping(bytes_);
        preferred_base_=mapping.preferred_base();
        const auto* entry=mapping.section(mapping.entry());
        if (!entry || (entry->flags&0xe0000000u)!=0x60000000u)
            throw std::invalid_argument("Native entry point has no read-only executable mapping");
        for (const unsigned index:{9u,10u,14u})
            if (mapping.directory(index).rva || mapping.directory(index).size)
                throw std::invalid_argument("Native TLS, load-configuration or managed startup needs a separate validated ABI");
        for (unsigned index=0;index<16;++index) {
            const auto directory=mapping.directory(index);
            if ((directory.rva==0)!=(directory.size==0))
                throw std::invalid_argument("Incomplete native loader directory");
            if (!directory.size) continue;
            if (index==4) {
                if (directory.rva%8 || directory.rva>bytes_.size() ||
                    directory.size>bytes_.size()-directory.rva)
                    throw std::invalid_argument("Invalid native certificate file extent");
            } else mapping.raw_offset(directory.rva,directory.size);
        }
        for (const auto& section:mapped_)
            if ((section.flags&0xa0000000u)==0xa0000000u ||
                (section.name==".rsrc" && section.flags!=0x40000040))
                throw std::invalid_argument("Unsupported executable/writable native mapping");
        relocations_=mapping.absolute_relocations();
        try {validate_profile(pe+24);}
        catch (const std::invalid_argument& profile_error) {
            canonical_=false;
            try {
                const OutlastNativeFunctions functions(mapping);
                bindings_=resolve_outlast_native_bindings(mapping,functions);
            } catch (const std::invalid_argument& contract_error) {
                throw std::invalid_argument(std::string(profile_error.what())+"; native discovery: "+contract_error.what());
            }
        }
    }
    bool canonical() const noexcept {return canonical_;}
    const OutlastNativeBindings& bindings() const noexcept {return bindings_;}
    std::string_view profile() const noexcept {
        return canonical_?outlast_native_profile:"outlast-win64-contracts-1";
    }
    std::uint32_t rva(std::uint32_t source) const {return canonical_?source:bindings_.rva(source);}
    std::string digest() const {return sha256_bytes(bytes_.data(),bytes_.size());}
    std::vector<unsigned char> body(std::uint32_t rva,std::size_t size) const {
        rva=this->rva(rva);
        const OutlastNativePE mapping(bytes_);
        const auto* section=mapping.section(rva,size);
        if (!section || rva-section->rva>=section->size || size>section->size-(rva-section->rva))
            throw std::invalid_argument("Native body crosses its mapped retail section");
        return mapping.body(rva,size);
    }
    std::vector<unsigned char> loaded_body(std::uint32_t source,std::size_t size,std::uintptr_t base) const {
        const auto rva=this->rva(source);
        auto bytes=body(source,size);
        const OutlastNativePE mapping(bytes_);
        for (const auto field:relocations_) {
            if (std::uint64_t(field)+8<=rva || field>=std::uint64_t(rva)+size) continue;
            const auto pointer=mapping.value<std::uint64_t>(field)+(std::uint64_t(base)-preferred_base_);
            const auto begin=std::max(field,rva);
            const auto end=std::min(std::uint64_t(field)+8,std::uint64_t(rva)+size);
            std::memcpy(bytes.data()+begin-rva,reinterpret_cast<const unsigned char*>(&pointer)+begin-field,
                        std::size_t(end-begin));
        }
        return bytes;
    }
};

inline std::vector<unsigned char> outlast_proof_body(
    const std::filesystem::path& path,std::uint32_t rva,std::size_t size,const char* digest) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read the selected native proof input");
    std::vector<unsigned char> bytes;
    if (input.tellg()==std::streampos(size)) {
        bytes.resize(size);input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),size))
            throw std::runtime_error("Private native body fixture read was incomplete");
    } else bytes=OutlastNativeImage(path).body(rva,size);
    if (sha256_bytes(bytes.data(),bytes.size())!=digest)
        throw std::invalid_argument("Native body differs from the exact MCP-recovered SHA256");
    return bytes;
}

}
