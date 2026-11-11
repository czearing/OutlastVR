#include "outlast_config.h"
#include <iostream>

using namespace olvr;
int main() {
    try {
        auto parse=[](const std::string& text) {std::istringstream input(text);return parse_outlast_config(input);};
        auto require=[](bool value) {if (!value) throw std::runtime_error("Configuration regression");};
        require(!parse("").physical_camcorder);
        require(!parse("[experimental]\nphysical_camcorder=false\n").physical_camcorder);
        require(parse("\xef\xbb\xbf; UTF-8 BOM\r\n[experimental]\r\nphysical_camcorder=true\r\n").physical_camcorder);
        const auto paths=parse("[game]\nexecutable=\"D:\\Steam Library\\Outlast\\OLGame.exe\"\n"
                               "[vr]\nopenxr_loader=custom\\openxr_loader.dll\n");
        require(paths.game_executable==L"D:\\Steam Library\\Outlast\\OLGame.exe");
        require(config_path(L"C:\\Mod",paths.openxr_loader)==L"C:\\Mod\\custom\\openxr_loader.dll");
        for (const auto* invalid:{"[experimental]\nphysical_camcorder=yes", "[experimental]\nphysical_camcorder=",
                "[experimental]\nphysical_camcorder=true\nphysical_camcorder=false",
                "[vr]\nunknown=true","[wrong]","physical_camcorder=true",
                "[game]\nexecutable=\"unterminated","[experimental]\nphysical_camcorder"}) {
            bool rejected=false;
            try {parse(invalid);} catch (const std::invalid_argument&) {rejected=true;}
            require(rejected);
        }
        std::cout<<"PASS: default-off experiment, explicit opt-in, UTF-8 INI, portable paths and invalid config rejection\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
