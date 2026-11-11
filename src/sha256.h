#pragma once
#include "d3d9_resource.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>

namespace olvr {

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_HASH_HANDLE hash_=nullptr;
    bool finished_=false;
    static void check(NTSTATUS status,const char* operation) {
        if (status<0) throw std::runtime_error(std::string(operation)+" NTSTATUS="+std::to_string(status));
    }
public:
    Sha256() {
        check(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0),
            "Open native SHA256 provider");
        const auto status=BCryptCreateHash(algorithm_,&hash_,nullptr,0,nullptr,0,0);
        if (status<0) {
            const auto cleanup=BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;
            if (cleanup<0) std::cerr<<"SHA256 initialization cleanup failed: "<<cleanup<<'\n';
            check(status,"Create native SHA256");
        }
    }
    Sha256(const Sha256&)=delete;
    Sha256& operator=(const Sha256&)=delete;
    ~Sha256() {
        const auto hash_status=hash_?BCryptDestroyHash(hash_):0;
        const auto algorithm_status=algorithm_?BCryptCloseAlgorithmProvider(algorithm_,0):0;
        if (hash_status<0 || algorithm_status<0) {
            std::cerr<<"SHA256 resource cleanup failed: "<<hash_status<<", "<<algorithm_status<<'\n';
            std::terminate();
        }
    }
    void update(const void* data,std::size_t bytes) {
        if (finished_ || (!data && bytes)) throw std::invalid_argument("Invalid SHA256 update");
        auto* current=static_cast<const unsigned char*>(data);
        while (bytes) {
            const auto count=static_cast<ULONG>(std::min<std::size_t>(bytes,std::numeric_limits<ULONG>::max()));
            check(BCryptHashData(hash_,const_cast<unsigned char*>(current),count,0),"Hash native bytes");
            current+=count;bytes-=count;
        }
    }
    std::string finish() {
        if (finished_) throw std::logic_error("SHA256 was already finalized");
        std::array<unsigned char,32> digest{};
        check(BCryptFinishHash(hash_,digest.data(),ULONG(digest.size()),0),"Finish native SHA256");
        finished_=true;
        const char hex[]="0123456789abcdef";
        std::string output;
        output.reserve(64);
        for (const auto byte:digest) {output+=hex[byte>>4];output+=hex[byte&15];}
        return output;
    }
};

inline std::string sha256_bytes(const void* data,std::size_t bytes) {
    Sha256 hash;hash.update(data,bytes);return hash.finish();
}
inline std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read SHA256 input file");
    Sha256 hash;
    std::array<char,65536> buffer{};
    while (input) {
        input.read(buffer.data(),buffer.size());
        if (input.gcount()>0) hash.update(buffer.data(),std::size_t(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("SHA256 file read failed before EOF");
    return hash.finish();
}

}
