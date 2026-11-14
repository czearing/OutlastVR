#include "outlast_native_image.h"
#include <iostream>

using namespace olvr;
static unsigned checks=0;
static void require(bool value,const char* text) {
    ++checks;if (!value) throw std::runtime_error(text);
}
struct Fixture {
    std::filesystem::path path;
    Fixture() {
        std::array<wchar_t,MAX_PATH> folder{},file{};
        if (!GetTempPathW(DWORD(folder.size()),folder.data()) ||
            !GetTempFileNameW(folder.data(),L"olv",0,file.data()))
            throw std::runtime_error("Cannot create owned compatibility fixture");
        path=file.data();
    }
    ~Fixture() {
        if (!DeleteFileW(path.c_str())) std::cerr<<"Cannot remove owned compatibility fixture: "<<GetLastError()<<'\n';
    }
};
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2,"Specify a compatible Outlast executable; it will only be read");
        require(sha256_bytes(nullptr,0)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "Empty SHA256 differs from the published known answer");
        require(sha256_bytes("abc",3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA256 differs from the published abc known answer");
        Sha256 split;split.update("a",1);split.update("bc",2);
        require(split.finish()==sha256_bytes("abc",3),"Streaming SHA256 differs from its known answer");
        bool refused=false;
        try {split.update("d",1);} catch (const std::invalid_argument&) {refused=true;}
        require(refused,"A finalized SHA256 accepted new data");
        OutlastNativeImage image(argv[1]);
        const auto input_digest=image.digest();
        require(sha256_file(argv[1])==input_digest,"Streaming file digest differs from the parsed input");
        const auto indexed=image.body(0xbf5b60,627),sphere=image.body(0x148780,194);
        require(sha256_bytes(indexed.data(),indexed.size())==
            "ff6939df22849635bde7285b764f3d7f5c6fa5f53f5a0d991dcbcd98049ff263",
            "Retail indexed function differs from the MCP-recovered body");
        require(sha256_bytes(sphere.data(),sphere.size())==
            "2b2f605c831e9b73bbc6766c6cc24e5dca2fb2a0743797512c9f2cb0b4d2846b",
            "Retail sphere function differs from the MCP-recovered body");
        for (unsigned fault=0;fault<3;++fault) {
            refused=false;
            try {
                if (fault==0) image.body(0xffffffffu,16);
                if (fault==1) image.body(0xbf5b60,0);
                if (fault==2) image.body(0xbf5b60,std::numeric_limits<std::size_t>::max());
            } catch (const std::invalid_argument&) {refused=true;}
            require(refused,"Invalid native-body mapping was accepted");
        }
        refused=false;
        try {OutlastNativeImage wrong(argv[0]);} catch (const std::invalid_argument&) {refused=true;}
        require(refused,"An unrelated executable passed native ABI validation");
        std::ifstream source(argv[1],std::ios::binary|std::ios::ate);
        std::vector<unsigned char> baseline(std::size_t(source.tellg()));
        source.seekg(0);
        require(bool(source.read(reinterpret_cast<char*>(baseline.data()),std::streamsize(baseline.size()))),
            "Cannot read the reference compatibility fixture");
        const auto read32=[&](std::size_t offset) {
            std::uint32_t value=0;std::memcpy(&value,baseline.data()+offset,4);return value;
        };
        const auto put32=[](auto& bytes,std::size_t offset,std::uint32_t value) {
            std::memcpy(bytes.data()+offset,&value,4);
        };
        const auto pe=read32(0x3c),optional=pe+24;
        std::uint16_t optional_size=0;std::memcpy(&optional_size,baseline.data()+pe+20,2);
        const auto table=optional+optional_size;
        const auto row=[&](unsigned section) {
            constexpr const char* names[]{".text",".rdata",".data",".pdata",".rsrc",".reloc"};
            for (unsigned index=0;index<6;++index) {
                const auto offset=table+index*40;
                const auto* name=reinterpret_cast<const char*>(baseline.data()+offset);
                if (std::string(name,std::find(name,name+8,'\0'))==names[section]) return offset;
            }
            throw std::runtime_error("Missing native fixture section");
        };
        const auto raw=[&](unsigned section) {return read32(row(section)+20);};
        const auto debug=raw(1)+0x162de70-0x1460000;
        Fixture fixture;
        auto variant=[&](auto mutate,bool accepted,const char* message,UINT sphere_rva=0) {
            auto bytes=baseline;mutate(bytes);
            {
                std::ofstream output(fixture.path,std::ios::binary|std::ios::trunc);
                if (!output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())))
                    throw std::runtime_error("Cannot write owned compatibility fixture");
            }
            bool admitted=false;
            try {
                OutlastNativeImage candidate(fixture.path);admitted=true;
                require(candidate.body(0xbf5b60,627)==indexed,
                    "Accepted file-layout variant changed the native hook body");
                require(candidate.digest()!=input_digest,"Positive compatibility fixture did not change file identity");
                if (sphere_rva) {
                    require(!candidate.canonical(),"Shifted native layout bypassed contract discovery");
                    require(candidate.rva(0x148780)==sphere_rva,"Shifted sphere hook retained its fixed source address");
                    require(candidate.body(0x148780,194)==sphere,"Translated sphere body differs from independent source evidence");
                    require(candidate.bindings().original_rva(sphere_rva+16)==0x148790,
                        "Relocated native caller did not map back to its source instruction");
                }
            } catch (const std::invalid_argument&) {}
            require(admitted==accepted,message);
        };
        variant([&](auto& b) {b[pe+8]^=1;},true,"COFF timestamp unnecessarily restricted compatibility");
        variant([&](auto& b) {b[optional+64]^=1;},true,"PE checksum unnecessarily restricted compatibility");
        variant([](auto& b) {b[0x40]^=1;},true,"DOS stub unnecessarily restricted compatibility");
        variant([&](auto& b) {b[raw(4)+read32(row(4)+8)-1]^=1;},true,
            "Non-executable resources unnecessarily restricted compatibility");
        variant([&](auto& b) {b[debug+4]^=1;},true,"Debug timestamp unnecessarily restricted compatibility");
        variant([&](auto& b) {b[raw(1)+0x1b5da98-0x1460000+8]^=1;},true,
            "PDB identifier unnecessarily restricted compatibility");
        variant([&](auto& b) {b[raw(0)+read32(row(0)+8)]^=1;},true,
            "Unmapped file padding unnecessarily restricted compatibility");
        variant([](auto& b) {b.insert(b.end(),{'o','v','e','r','l','a','y'});},true,
            "Packaging overlay unnecessarily restricted compatibility");
        variant([&](auto& b) {
            const auto headers=read32(optional+60);
            b.insert(b.begin()+headers,512,0);
            for (unsigned section=0;section<6;++section) put32(b,row(section)+20,raw(section)+512);
            put32(b,debug+512+24,read32(debug+24)+512);
        },true,"Equivalent native sections at different file offsets were rejected");
        variant([&](auto& b) {std::swap_ranges(b.begin()+row(0),b.begin()+row(0)+40,b.begin()+row(1));},true,
            "Equivalent native sections in a different table order were rejected");
        variant([&](auto& b) {
            const auto certificate=std::uint32_t((b.size()+7)/8*8);
            b.resize(certificate+8,0);
            put32(b,optional+112+4*8,certificate);put32(b,optional+116+4*8,8);
        },true,"Certificate-table packaging unnecessarily restricted compatibility");
        for (const unsigned section:{0u,1u,2u})
            variant([&](auto& b) {b[raw(section)+64]^=1;},true,
                "Unrelated native bytes still imposed a whole-section identity fence");
        variant([&](auto& b) {b[raw(0)+0xbf5b60-0x1000]^=1;},false,
            "A changed indexed rendering contract was admitted");
        variant([&](auto& b) {b[raw(0)+0x8c7670-0x1000]^=1;},false,
            "A changed view-layout constructor contract was admitted");
        variant([&](auto& b) {b[raw(1)+0x1a1aa58-0x1460000]^=1;},false,
            "A conflicting indexed/view dispatch table was admitted");
        variant([&](auto& b) {b[raw(2)+0x1e09e58-0x1dfb000]^=1;},false,
            "An incompatible native policy global was admitted");
        variant([&](auto& b) {put32(b,raw(3),0xffffffffu);},false,
            "A malformed native unwind range was admitted");
        variant([&](auto& b) {put32(b,raw(5)+4,7);},false,
            "A malformed native loader relocation block was admitted");
        const OutlastNativePE original(baseline);
        const OutlastNativeFunctions original_functions(original);
        const auto moved_sphere=(read32(row(0)+12)+read32(row(0)+8)+15)&~15u;
        variant([&](auto& b) {
            const auto& group=original_functions.at(0x148780);
            require(group.ranges.size()==1 && group.size==sphere.size(),
                "Shift fixture requires the independently verified single-range sphere body");
            require(moved_sphere-0x1000+sphere.size()<=read32(row(0)+16),
                "Shift fixture has no owned file-backed section padding");
            const auto destination=raw(0)+moved_sphere-0x1000;
            std::copy(sphere.begin(),sphere.end(),b.begin()+destination);
            b[raw(0)+0x148780-0x1000]=0xcc;
            put32(b,row(0)+8,moved_sphere-0x1000+UINT(sphere.size()));
            const auto directory=original.directory(3);
            const auto begin=original.raw_offset(directory.rva,directory.size);
            const auto& records=original_functions.records();
            auto index=std::find_if(records.begin(),records.end(),[](const auto& record) {return record.begin==0x148780;});
            require(index!=records.end(),"Shift fixture has no source unwind record");
            const auto offset=begin+std::size_t(index-records.begin())*12;
            std::memmove(b.data()+offset,b.data()+offset+12,begin+directory.size-offset-12);
            put32(b,begin+directory.size-12,moved_sphere);
            put32(b,begin+directory.size-8,moved_sphere+UINT(sphere.size()));
            put32(b,begin+directory.size-4,index->unwind);
            for (const auto& contract:outlast_native_contracts)
                for (std::size_t r=0;r<contract.reference_count;++r) {
                    const auto& reference=contract.references[r];
                    if (reference.target!=0x148780) continue;
                    const auto source_at=[&](std::uint32_t flat) {
                        for (std::size_t part=0;part<contract.range_count;++part) {
                            const auto range=contract.source_ranges[part];
                            if (flat<range.size) return range.rva+flat;
                            flat-=range.size;
                        }
                        throw std::runtime_error("Shift fixture reference has no source code range");
                    };
                    const auto field=original.raw_offset(source_at(reference.position),reference.width);
                    if (reference.kind==NativeReferenceKind::absolute) {
                        const auto value=original.preferred_base()+moved_sphere;
                        std::memcpy(b.data()+field,&value,8);
                    } else {
                        require(reference.width==4,"Shift fixture cannot rewrite a short external branch");
                        const auto value=std::int32_t(std::int64_t(moved_sphere)-
                            source_at(reference.instruction)-reference.instruction_size);
                        std::memcpy(b.data()+field,&value,4);
                    }
                }
            for (const auto field:original.absolute_relocations())
                if (original.value<std::uint64_t>(field)==original.preferred_base()+0x148780) {
                    const auto value=original.preferred_base()+moved_sphere;
                    std::memcpy(b.data()+original.raw_offset(field,8),&value,8);
                }
        },true,"Valid shifted code/hook/caller layout was rejected",moved_sphere);
        variant([&](auto& b) {put32(b,optional+16,read32(optional+16)+1);},false,"Changed native entry point was admitted");
        variant([&](auto& b) {b[optional+25]^=1;},false,"Changed preferred image base was admitted");
        variant([&](auto& b) {put32(b,row(0)+8,0xffffffffu);},false,"Overflowing virtual section extent was admitted");
        variant([&](auto& b) {put32(b,row(1)+20,raw(0));},false,"Overlapping raw section ranges were admitted");
        variant([&](auto& b) {put32(b,row(1)+12,0x1000);},false,"Overlapping virtual sections were admitted");
        variant([&](auto& b) {put32(b,row(0)+20,std::uint32_t(b.size()));},false,"Out-of-file native section was admitted");
        variant([&](auto& b) {put32(b,optional+112+8,0x1000);},false,"Changed native import directory was admitted");
        variant([&](auto& b) {put32(b,optional+112+9*8,0x1000);},false,"Unprofiled TLS callback directory was admitted");
        variant([&](auto& b) {
            b[raw(0)+0xbf5b60-0x1000]^=1;put32(b,optional+112+6*8,0xbf5b60);
        },false,"Candidate-controlled debug ranges hid changed required native code");
        variant([&](auto& b) {put32(b,optional+112+4*8,std::uint32_t(b.size()+8));
            put32(b,optional+116+4*8,8);},false,"Out-of-file certificate range was admitted");
        variant([&](auto& b) {put32(b,row(4)+36,0x60000040);},false,"Executable resources bypassed ABI validation");
        variant([&](auto& b) {put32(b,0x3c,0xffffffffu);},false,"Out-of-file PE header was admitted");
        variant([](auto& b) {b.resize(63);},false,"Truncated PE header was admitted");
        std::cout<<"PASS: native image identity/mapping; independent_checks="<<checks
            <<"; known SHA256 answers, exact MCP bodies, compatible metadata/file-layout variants, "
              "changed ABI and malformed PE refusal; no game launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
