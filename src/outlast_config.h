#pragma once
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace olvr {

struct OutlastConfig {
    std::filesystem::path game_executable;
    std::filesystem::path openxr_loader;
    bool physical_camcorder=false;
};

inline OutlastConfig parse_outlast_config(std::istream& input) {
    OutlastConfig result;
    std::string line,section;
    std::set<std::string> seen;
    unsigned number=0;
    auto trim=[](const std::string& value) {
        const auto first=value.find_first_not_of(" \t\r");
        return first==std::string::npos?std::string{}:
            value.substr(first,value.find_last_not_of(" \t\r")-first+1);
    };
    auto error=[&](const std::string& message) {
        return std::invalid_argument("OutlastVR.ini line "+std::to_string(number)+": "+message);
    };
    while (std::getline(input,line)) {
        ++number;
        if (number==1 && line.compare(0,3,"\xef\xbb\xbf")==0) line.erase(0,3);
        line=trim(line);
        if (line.empty() || line[0]==';' || line[0]=='#') continue;
        if (line[0]=='[' && line.back()==']') {
            section=line.substr(1,line.size()-2);
            if (section!="game" && section!="vr" && section!="experimental")
                throw error("unknown section "+section);
            continue;
        }
        const auto split=line.find('=');
        if (split==std::string::npos) throw error("expected key=value");
        const auto key=section+"."+trim(line.substr(0,split));
        auto value=trim(line.substr(split+1));
        if (!seen.insert(key).second) throw error("duplicate setting "+key);
        if (key=="experimental.physical_camcorder") {
            if (value!="true" && value!="false") throw error("physical_camcorder must be true or false");
            result.physical_camcorder=value=="true";
        } else if (key=="game.executable" || key=="vr.openxr_loader") {
            if (!value.empty() && value.front()=='"') {
                if (value.size()<2 || value.back()!='"') throw error("unterminated quoted path");
                value=value.substr(1,value.size()-2);
            }
            if (value.find('\0')!=std::string::npos) throw error("path contains a NUL byte");
            auto& destination=key=="game.executable"?result.game_executable:result.openxr_loader;
            destination=std::filesystem::u8path(value);
        } else throw error("unknown setting "+key);
    }
    if (input.bad()) throw std::runtime_error("Cannot finish reading OutlastVR.ini");
    return result;
}

inline OutlastConfig load_outlast_config(const std::filesystem::path& directory) {
    const auto file=directory/L"OutlastVR.ini";
    if (!std::filesystem::exists(file)) return {};
    if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file)>65536)
        throw std::invalid_argument("OutlastVR.ini must be a regular file no larger than 64 KiB");
    std::ifstream input(file,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read OutlastVR.ini");
    return parse_outlast_config(input);
}

inline std::filesystem::path config_path(const std::filesystem::path& directory,
                                         const std::filesystem::path& value) {
    return value.is_absolute()?value:std::filesystem::absolute(directory/value).lexically_normal();
}

inline std::filesystem::path outlast_game_path(const std::filesystem::path& directory,
                                              const OutlastConfig& config) {
    if (!config.game_executable.empty()) return config_path(directory,config.game_executable);
    for (const auto& candidate:{directory/L"OLGame.exe",directory/L"Binaries"/L"Win64"/L"OLGame.exe"})
        if (std::filesystem::is_regular_file(candidate)) return std::filesystem::absolute(candidate);
    throw std::runtime_error("Outlast not found. Extract beside OLGame.exe, set [game] executable in "
                             "OutlastVR.ini, or use --game \"path\\to\\OLGame.exe\".");
}

}
