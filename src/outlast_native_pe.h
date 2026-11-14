#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

namespace olvr {

struct NativeCodeRange {
    std::uint32_t rva,size;
};

class OutlastNativePE {
public:
    struct Section {
        std::uint32_t rva,size,raw,raw_size,flags;
    };
    struct Import {
        std::uint32_t slot;
        std::string dll,name;
        std::uint16_t ordinal;
    };
private:
    const std::vector<unsigned char>& bytes_;
    std::vector<Section> sections_;
    std::array<NativeCodeRange,16> directories_{};
    std::uint64_t base_=0;
    std::uint32_t extent_=0,headers_=0,entry_=0;
    template<class T> T read(std::size_t offset) const {
        if (offset>bytes_.size() || sizeof(T)>bytes_.size()-offset)
            throw std::invalid_argument("Native PE field exceeds its file");
        T value{};std::memcpy(&value,bytes_.data()+offset,sizeof(value));return value;
    }
public:
    explicit OutlastNativePE(const std::vector<unsigned char>& bytes):bytes_(bytes) {
        if (bytes.size()<64 || bytes.size()>128u*1024*1024 || read<std::uint16_t>(0)!=0x5a4d)
            throw std::invalid_argument("Invalid bounded native PE input");
        const auto pe=std::size_t(read<std::uint32_t>(0x3c));
        if (pe>bytes.size() || bytes.size()-pe<24 || read<std::uint32_t>(pe)!=0x4550 ||
            read<std::uint16_t>(pe+4)!=0x8664)
            throw std::invalid_argument("Native bindings require an x64 PE");
        const auto optional=pe+24;
        const auto optional_size=read<std::uint16_t>(pe+20);
        if (optional_size<240 || optional>bytes.size() || optional_size>bytes.size()-optional ||
            read<std::uint16_t>(optional)!=0x20b || read<std::uint32_t>(optional+108)!=16)
            throw std::invalid_argument("Invalid native PE32+ optional header");
        base_=read<std::uint64_t>(optional+24);
        entry_=read<std::uint32_t>(optional+16);
        extent_=read<std::uint32_t>(optional+56);headers_=read<std::uint32_t>(optional+60);
        const auto file_alignment=read<std::uint32_t>(optional+36);
        const auto section_alignment=read<std::uint32_t>(optional+32);
        const auto count=read<std::uint16_t>(pe+6);
        const auto table=optional+optional_size;
        if (!base_ || base_>std::numeric_limits<std::uint64_t>::max()-extent_ ||
            !extent_ || extent_>128u*1024*1024 || extent_%4096 || section_alignment!=4096 ||
            file_alignment<512 || file_alignment>4096 || (file_alignment&(file_alignment-1)) ||
            !count || count>96 || table>bytes.size() || count>(bytes.size()-table)/40 ||
            headers_<table+std::size_t(count)*40 || headers_>bytes.size() ||
            headers_%file_alignment)
            throw std::invalid_argument("Invalid native PE image or section-table bounds");
        for (unsigned i=0;i<16;++i)
            directories_[i]={read<std::uint32_t>(optional+112+i*8),read<std::uint32_t>(optional+116+i*8)};
        for (unsigned i=0;i<count;++i) {
            const auto offset=table+i*40;
            Section section{read<std::uint32_t>(offset+12),read<std::uint32_t>(offset+8),
                read<std::uint32_t>(offset+20),read<std::uint32_t>(offset+16),read<std::uint32_t>(offset+36)};
            const auto mapped=std::max(section.size,section.raw_size);
            if (!mapped || section.rva<headers_ || section.rva%4096 ||
                section.rva>extent_ || mapped>extent_-section.rva ||
                section.raw_size>bytes.size() || section.raw>bytes.size()-section.raw_size ||
                section.raw_size%file_alignment ||
                (section.raw_size && (section.raw<headers_ || section.raw%file_alignment)))
                throw std::invalid_argument("Invalid native PE section mapping");
            for (const auto& other:sections_) {
                if ((section.rva<std::uint64_t(other.rva)+std::max(other.size,other.raw_size) &&
                     other.rva<std::uint64_t(section.rva)+mapped) ||
                    (section.raw_size && other.raw_size &&
                     section.raw<std::uint64_t(other.raw)+other.raw_size &&
                     other.raw<std::uint64_t(section.raw)+section.raw_size))
                    throw std::invalid_argument("Overlapping native PE mappings");
            }
            sections_.push_back(section);
        }
    }
    std::uint64_t preferred_base() const noexcept {return base_;}
    std::uint32_t extent() const noexcept {return extent_;}
    std::uint32_t entry() const noexcept {return entry_;}
    const std::vector<Section>& sections() const noexcept {return sections_;}
    NativeCodeRange directory(unsigned index) const {
        if (index>=directories_.size()) throw std::invalid_argument("Invalid native PE directory index");
        return directories_[index];
    }
    const Section* section(std::uint32_t rva,std::size_t size=1) const noexcept {
        if (!size) return nullptr;
        for (const auto& item:sections_) {
            const auto mapped=std::max(item.size,item.raw_size);
            if (rva>=item.rva && rva-item.rva<mapped && size<=mapped-(rva-item.rva)) return &item;
        }
        return nullptr;
    }
    std::size_t raw_offset(std::uint32_t rva,std::size_t size) const {
        if (!size) throw std::invalid_argument("Empty native PE read");
        if (rva<headers_ && size<=headers_-rva) return rva;
        const auto* item=section(rva,size);
        if (!item || rva-item->rva>=item->raw_size || size>item->raw_size-(rva-item->rva))
            throw std::invalid_argument("Native PE read has no complete file-backed mapping");
        return item->raw+(rva-item->rva);
    }
    template<class T> T value(std::uint32_t rva) const {return read<T>(raw_offset(rva,sizeof(T)));}
    const unsigned char* data(std::uint32_t rva,std::size_t size) const {
        return bytes_.data()+raw_offset(rva,size);
    }
    std::string string(std::uint32_t rva) const {
        std::string result;
        for (unsigned index=0;index<512;++index) {
            if (rva>=extent_ || index>=extent_-rva)
                throw std::invalid_argument("Native import string exceeds the image");
            const auto byte=value<unsigned char>(rva+index);
            if (!byte) return result;
            if (byte<32 || byte>126) throw std::invalid_argument("Invalid native import string encoding");
            result.push_back(char(byte));
        }
        throw std::invalid_argument("Unterminated native import string");
    }
    std::vector<Import> imports() const {
        const auto directory=directories_[1];
        if (!directory.rva || directory.size<20)
            throw std::invalid_argument("Native import directory is unavailable");
        raw_offset(directory.rva,directory.size);
        std::vector<Import> result;
        for (std::uint32_t offset=0;directory.size-offset>=20;offset+=20) {
            const auto record=directory.rva+offset;
            const auto lookup=value<std::uint32_t>(record);
            const auto name=value<std::uint32_t>(record+12);
            const auto slots=value<std::uint32_t>(record+16);
            if (!lookup && !name && !slots && !value<std::uint32_t>(record+4) &&
                !value<std::uint32_t>(record+8)) return result;
            if (!lookup || !name || !slots)
                throw std::invalid_argument("Native import descriptor has no named lookup table");
            auto dll=string(name);
            if (dll.empty()) throw std::invalid_argument("Empty native import library name");
            for (auto& byte:dll) if (byte>='A' && byte<='Z') byte=char(byte-'A'+'a');
            for (std::uint32_t index=0;;++index) {
                if (index>=65536 || result.size()>=65536 || lookup>=extent_ || slots>=extent_ ||
                    std::uint64_t(index)*8+8>extent_-lookup || std::uint64_t(index)*8+8>extent_-slots)
                    throw std::invalid_argument("Unbounded native import thunk table");
                const auto thunk=value<std::uint64_t>(lookup+index*8);
                raw_offset(slots+index*8,8);
                if (!thunk) break;
                Import imported{slots+index*8,dll,{},0};
                if (thunk&0x8000000000000000ull) {
                    if (thunk&0x7fffffffffff0000ull)
                        throw std::invalid_argument("Invalid native import ordinal");
                    imported.ordinal=std::uint16_t(thunk);
                } else {
                    if (thunk>=extent_ || extent_-thunk<3)
                        throw std::invalid_argument("Native import name exceeds the image");
                    imported.name=string(std::uint32_t(thunk)+2);
                    if (imported.name.empty()) throw std::invalid_argument("Empty native import symbol");
                }
                result.push_back(std::move(imported));
            }
        }
        throw std::invalid_argument("Unterminated native import descriptor table");
    }
    std::set<std::uint32_t> absolute_relocations() const {
        const auto directory=directories_[5];
        if (!directory.rva && !directory.size) return {};
        if (!directory.rva || directory.size<8)
            throw std::invalid_argument("Invalid native relocation directory");
        raw_offset(directory.rva,directory.size);
        std::set<std::uint32_t> result;
        for (std::uint32_t offset=0;offset<directory.size;) {
            if (directory.size-offset<8)
                throw std::invalid_argument("Truncated native relocation block");
            const auto page=value<std::uint32_t>(directory.rva+offset);
            const auto size=value<std::uint32_t>(directory.rva+offset+4);
            if (page%4096 || page>=extent_ || size<8 || size%4 || size>directory.size-offset)
                throw std::invalid_argument("Invalid native relocation block extent");
            for (std::uint32_t index=8;index<size;index+=2) {
                const auto entry=value<std::uint16_t>(directory.rva+offset+index);
                const auto type=entry>>12;
                if (!type) continue;
                if (type!=3 && type!=10)
                    throw std::invalid_argument("Unsupported native x64 relocation type");
                const auto rva=page+(entry&0xfff);
                if (!section(rva,type==10?8:4))
                    throw std::invalid_argument("Native relocation exceeds a mapped field");
                if (type==10 && !result.insert(rva).second)
                    throw std::invalid_argument("Duplicate absolute native relocation");
            }
            offset+=size;
        }
        return result;
    }
    std::vector<unsigned char> body(std::uint32_t rva,std::size_t size) const {
        const auto offset=raw_offset(rva,size);
        return {bytes_.begin()+offset,bytes_.begin()+offset+size};
    }
    std::vector<unsigned char> initial_data(std::uint32_t rva,std::size_t size) const {
        const auto* mapped=section(rva,size);
        if (!mapped) throw std::invalid_argument("Native initial data exceeds its mapped extent");
        std::vector<unsigned char> result(size,0);
        const auto offset=rva-mapped->rva;
        if (offset<mapped->raw_size)
            std::copy_n(bytes_.data()+mapped->raw+offset,std::min(size,std::size_t(mapped->raw_size-offset)),
                        result.data());
        return result;
    }
};

// Runtime-function records can split a single procedure into multiple chained ranges.
class OutlastNativeFunctions {
public:
    struct Record {std::uint32_t begin,end,unwind;};
    struct Group {std::uint32_t entry;std::vector<NativeCodeRange> ranges;std::size_t size=0;};
private:
    std::vector<Record> records_;
    std::vector<Group> groups_;
    std::vector<std::size_t> owners_;
public:
    explicit OutlastNativeFunctions(const OutlastNativePE& image) {
        const auto directory=image.directory(3);
        if (!directory.rva || !directory.size || directory.size%12)
            throw std::invalid_argument("Invalid native runtime-function directory");
        image.raw_offset(directory.rva,directory.size);
        records_.reserve(directory.size/12);
        for (std::uint32_t offset=0;offset<directory.size;offset+=12) {
            const auto address=directory.rva+offset;
            Record item{image.value<std::uint32_t>(address),image.value<std::uint32_t>(address+4),
                image.value<std::uint32_t>(address+8)};
            const auto* code=item.end>item.begin?image.section(item.begin,item.end-item.begin):nullptr;
            if (!code || !(code->flags&0x20000000) || !item.unwind || item.unwind%4 ||
                (!records_.empty() && item.begin<records_.back().end))
                throw std::invalid_argument("Invalid native runtime-function code range");
            image.raw_offset(item.begin,item.end-item.begin);
            records_.push_back(item);
        }
        const auto unknown=std::numeric_limits<std::size_t>::max();
        owners_.assign(records_.size(),unknown);
        for (std::size_t index=0;index<records_.size();++index) {
            std::vector<std::size_t> path;
            auto current=index;
            while (owners_[current]==unknown) {
                if (path.size()>=64 || std::find(path.begin(),path.end(),current)!=path.end())
                    throw std::invalid_argument("Cyclic or excessive native unwind chain");
                path.push_back(current);
                const auto& item=records_[current];
                const auto header=image.value<std::uint32_t>(item.unwind);
                const auto version=header&7u,flags=(header>>3)&31u,codes=(header>>16)&255u;
                if ((version!=1 && version!=2) || flags>7 || ((flags&4) && (flags&3)))
                    throw std::invalid_argument("Unsupported native unwind header");
                const auto extra=item.unwind+4+((codes+1)&~1u)*2;
                image.raw_offset(item.unwind,extra-item.unwind+((flags&4)?12:(flags&3)?4:0));
                if (!(flags&4)) {
                    if (flags&3) {
                        const auto handler=image.value<std::uint32_t>(extra);
                        const auto* section=image.section(handler);
                        if (!section || !(section->flags&0x20000000))
                            throw std::invalid_argument("Native unwind handler is outside executable code");
                    }
                    owners_[current]=groups_.size();
                    groups_.push_back({item.begin,{},0});
                    break;
                }
                const Record parent{image.value<std::uint32_t>(extra),image.value<std::uint32_t>(extra+4),
                    image.value<std::uint32_t>(extra+8)};
                const auto found=std::lower_bound(records_.begin(),records_.end(),parent.begin,
                    [](const Record& record,std::uint32_t begin) {return record.begin<begin;});
                if (found==records_.end() || found->begin!=parent.begin ||
                    found->end!=parent.end || found->unwind!=parent.unwind)
                    throw std::invalid_argument("Native unwind chain has no exact parent record");
                current=std::size_t(found-records_.begin());
            }
            for (const auto visited:path) owners_[visited]=owners_[current];
        }
        for (std::size_t index=0;index<records_.size();++index) {
            auto& group=groups_[owners_[index]];
            const auto& item=records_[index];
            group.ranges.push_back({item.begin,item.end-item.begin});
            group.size+=item.end-item.begin;
        }
    }
    const std::vector<Group>& groups() const noexcept {return groups_;}
    const std::vector<Record>& records() const noexcept {return records_;}
    const Group* containing(std::uint32_t rva) const noexcept {
        auto found=std::upper_bound(records_.begin(),records_.end(),rva,
            [](std::uint32_t address,const Record& record) {return address<record.begin;});
        if (found==records_.begin()) return nullptr;
        --found;
        if (rva>=found->end) return nullptr;
        return &groups_[owners_[std::size_t(found-records_.begin())]];
    }
    const Group& at(std::uint32_t entry) const {
        const auto* group=containing(entry);
        if (!group || group->entry!=entry)
            throw std::invalid_argument("Native address is not a primary unwind-group entry");
        return *group;
    }
    std::vector<unsigned char> body(const OutlastNativePE& image,std::uint32_t entry) const {
        const auto& group=at(entry);
        std::vector<unsigned char> result;result.reserve(group.size);
        for (const auto range:group.ranges) {
            const auto part=image.body(range.rva,range.size);
            result.insert(result.end(),part.begin(),part.end());
        }
        return result;
    }
};

}
