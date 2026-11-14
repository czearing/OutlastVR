#include "outlast_native_bindings.h"
#include "outlast_target_roles.h"
#include <iostream>

using namespace olvr;
static unsigned checks=0;
static void require(bool value,const char* message) {
    ++checks;if (!value) throw std::runtime_error(message);
}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2 || (argc==3 && std::wstring(argv[2])==L"--alternate"),
            "Specify a locally owned executable and optional --alternate; the target is never executed");
        std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
        require(bool(input),"Cannot read native discovery fixture");
        const auto size=input.tellg();
        require(size>0 && size<=128*1024*1024,"Invalid native discovery fixture extent");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));input.seekg(0);
        require(bool(input.read(reinterpret_cast<char*>(bytes.data()),size)),"Incomplete native discovery fixture");
        const OutlastNativePE image(bytes);
        const OutlastNativeFunctions functions(image);
        const OutlastNativeContracts discovery(image,functions);
        const auto contract=[](std::uint32_t identity)->const NativeFunctionContract& {
            for (const auto& item:outlast_native_contracts) if (item.identity==identity) return item;
            throw std::runtime_error("Missing native discovery contract");
        };
        if (argc==2) {
            for (const auto& item:outlast_native_contracts) {
                const auto match=discovery.at(item,item.identity);
                if (!match) {
                    const auto* group=functions.containing(item.identity);
                    std::cerr<<"Contract 0x"<<std::hex<<item.identity<<std::dec<<"; expected bytes "<<item.size
                        <<"; actual group "<<(group?group->size:0)<<'\n';
                    auto normalized=item.leaf?image.body(item.identity,item.size):functions.body(image,item.identity);
                    require(normalized.size()==item.size,"Reference group extent differs from the compiled contract");
                    for (std::size_t index=0;index<item.reference_count;++index) {
                        const auto& ref=item.references[index];
                        std::fill_n(normalized.data()+ref.position,ref.width,0);
                    }
                    std::cerr<<"Expected digest "<<item.sha256<<"; actual masked digest "
                        <<sha256_bytes(normalized.data(),normalized.size())<<'\n';
                    std::cerr<<discovery.diagnose(item,item.identity)<<'\n';
                    throw std::runtime_error("Reference contract did not match its own native entry");
                }
                for (const auto& binding:match->addresses)
                    require(binding.first==binding.second,"Reference capture changed the source binding");
            }
            const auto imported=discovery.find(contract(0xe6b4ac));
            require(imported.size()==1 && imported[0].entry==0xe6b4ac,
                "XInput state thunk was confused with another six-byte import thunk");
            const auto bindings=resolve_outlast_native_bindings(image,functions);
            for (const auto& item:outlast_native_contracts)
                require(bindings.rva(item.identity)==item.identity,"Resolved source routine moved unexpectedly");
            require(bindings.rva(0xc47aed)==0xc47aed,"Interior input callsite was not recovered");
            require(bindings.rva(0x201e318)==bindings.rva(0x201e310)+8,"Native gamepad alias differs");
            for (const auto site:outlast_allocation_sites) {
                require(bindings.rva(UINT(site.return_rva))==site.return_rva,
                    "A stereo texture allocation callsite has no resolved native binding");
                require(bindings.original_rva(UINT(site.return_rva))==site.return_rva,
                    "A stereo allocation callsite has no inverse ownership binding");
            }
            auto invalid=contract(0x148340);
            invalid.size=0;
            bool rejected=false;
            try {discovery.find(invalid);} catch (const std::invalid_argument&) {rejected=true;}
            require(rejected,"Malformed compiled native contract was accepted");
        } else {
            const std::pair<std::uint32_t,std::uint32_t> independent_bindings[]{
                {0x148340,0x3dc020},{0x148600,0x3dc2e0},{0x148780,0x3dc460},
                {0x148850,0x3dc530},{0xcbc70,0x307370},{0xcedec0,0x24a6b80},
                {0xc427e0,0x2348000},{0xd03f30,0x24c6630},{0x30b9d0,0x759b90}
            };
            for (const auto binding:independent_bindings) {
                const auto found=discovery.find(contract(binding.first));
                if (found.size()!=1 || found[0].entry!=binding.second) {
                    std::cerr<<"Reference role 0x"<<std::hex<<binding.first<<"; expected alternate 0x"
                        <<binding.second<<std::dec<<"; found "<<found.size()<<'\n';
                    for (const auto& match:found) std::cerr<<"Candidate 0x"<<std::hex<<match.entry<<std::dec<<'\n';
                }
                require(found.size()==1 && found[0].entry==binding.second,
                    "Real alternate native binding did not resolve uniquely to its independently recovered address");
                require(found[0].entry!=binding.first,"Alternate discovery reused a fixed reference address");
                if (binding.first==0x30b9d0) {
                    const auto policy=found[0].addresses.find(0x1e09e58);
                    require(policy!=found[0].addresses.end() && policy->second==0x50f9b08,
                        "Alternate native policy global was not recovered from the verified full native function");
                }
            }
            require(discovery.find(contract(0x8c7670)).empty(),
                "An unresolved alternate renderer constructor was incorrectly admitted");
        }
        std::cout<<"PASS: "<<checks<<" native contract checks; address-independent real code discovery; no game execution\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
