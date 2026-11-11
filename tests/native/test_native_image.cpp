#include "outlast_native_image.h"
#include <iostream>

using namespace olvr;
static unsigned checks=0;
static void require(bool value,const char* text) {
    ++checks;if (!value) throw std::runtime_error(text);
}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2,"Specify the exact retail Outlast executable; it will only be read");
        require(sha256_bytes(nullptr,0)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "Empty SHA256 differs from the published known answer");
        require(sha256_bytes("abc",3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA256 differs from the published abc known answer");
        Sha256 split;split.update("a",1);split.update("bc",2);
        require(split.finish()==sha256_bytes("abc",3),"Streaming SHA256 differs from its known answer");
        bool refused=false;
        try {split.update("d",1);} catch (const std::invalid_argument&) {refused=true;}
        require(refused,"A finalized SHA256 accepted new data");
        require(sha256_file(argv[1])==outlast_retail_sha256,"Streaming file digest differs from the REA target digest");
        OutlastNativeImage image(argv[1]);
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
        require(refused,"An unsupported executable passed the exact retail digest gate");
        std::cout<<"PASS: native image identity/mapping; independent_checks="<<checks
            <<"; published SHA256 answers, streaming equivalence, exact MCP body digests, invalid mappings and unsupported executable refusal; no game launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
