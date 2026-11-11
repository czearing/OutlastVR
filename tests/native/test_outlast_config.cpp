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
        const auto comfort=parse("[vr]\nsnap_turn_degrees=30\nworld_scale=1.2\n"
            "vignette_strength=0.7\nseated=true\nleft_handed=true\ntoggle_sprint=true\nmovement_reference=off_hand\n").comfort;
        require(comfort.snap_turn_degrees==30 && comfort.world_scale==1.2f &&
            comfort.vignette_strength==.7f && comfort.seated && comfort.toggle_sprint && comfort.primary_hand()==0 &&
            comfort.movement_reference==MovementReference::off_hand);
        for (const auto* invalid:{"[experimental]\nphysical_camcorder=yes", "[experimental]\nphysical_camcorder=",
                "[experimental]\nphysical_camcorder=true\nphysical_camcorder=false",
                "[vr]\nunknown=true","[wrong]","physical_camcorder=true",
                "[game]\nexecutable=\"unterminated","[experimental]\nphysical_camcorder",
                "[vr]\nsnap_turn_degrees=5","[vr]\nsnap_turn_degrees=nan",
                "[vr]\nworld_scale=0","[vr]\nworld_scale=1.0junk",
                "[vr]\nvignette_strength=2","[vr]\nseated=yes","[vr]\nmovement_reference=wrong",
                "[vr]\ntoggle_sprint=yes",
                "[experimental]\nphysical_camcorder=true\n[vr]\nleft_handed=true"}) {
            bool rejected=false;
            try {parse(invalid);} catch (const std::invalid_argument&) {rejected=true;}
            require(rejected);
        }
        std::cout<<"PASS: default-off experiment, explicit opt-in, UTF-8 INI, portable paths and invalid config rejection\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
