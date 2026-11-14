#pragma once
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <cmath>
#include <locale>
#include <array>
#include <string_view>
#include <variant>
#include <algorithm>
#include <type_traits>

namespace olvr {

enum class MovementReference {game,head,main_hand,off_hand};
struct OutlastComfortSettings {
    float snap_turn_degrees=0,world_scale=1,vignette_strength=0;
    bool seated=false,left_handed=false,toggle_sprint=false;
    MovementReference movement_reference=MovementReference::game;
    bool hit_distortion=false;
    bool body_follow=true,bloom=true,film_grain=true;
    float body_follow_deadzone=35,hand_pickup_radius=.30f;
    unsigned primary_hand() const {return left_handed?0u:1u;}
};
struct OutlastConfig {
    std::filesystem::path game_executable;
    std::filesystem::path openxr_loader;
    bool physical_camcorder=false;
    bool instant_pickups=true;
    OutlastComfortSettings comfort;
};

struct OutlastSetting {
    using Member=std::variant<bool OutlastComfortSettings::*,float OutlastComfortSettings::*,
        MovementReference OutlastComfortSettings::*,bool OutlastConfig::*,std::filesystem::path OutlastConfig::*>;
    std::string_view key,label;
    Member member;
    float minimum=0,maximum=1,step=1;
    std::string_view unit{};
    bool live=false;
    constexpr bool in_menu() const {
        return live && (std::holds_alternative<bool OutlastComfortSettings::*>(member) ||
            std::holds_alternative<float OutlastComfortSettings::*>(member) ||
            std::holds_alternative<MovementReference OutlastComfortSettings::*>(member));
    }
    bool boolean() const {
        return std::holds_alternative<bool OutlastComfortSettings::*>(member) ||
            std::holds_alternative<bool OutlastConfig::*>(member);
    }
    bool path() const {return std::holds_alternative<std::filesystem::path OutlastConfig::*>(member);}
    bool choice() const {return std::holds_alternative<MovementReference OutlastComfortSettings::*>(member);}
    float value(const OutlastConfig& config) const {
        return std::visit([&](auto field)->float {
            using T=decltype(field);
            if constexpr (std::is_same_v<T,std::filesystem::path OutlastConfig::*>)
                return (config.*field).empty()?0.f:1.f;
            else if constexpr (std::is_same_v<T,bool OutlastConfig::*>) return config.*field;
            else return static_cast<float>(config.comfort.*field);
        },member);
    }
    void assign(OutlastConfig& config,float value) const {
        std::visit([&](auto field) {
            using T=decltype(field);
            if constexpr (std::is_same_v<T,std::filesystem::path OutlastConfig::*>)
                throw std::logic_error("Cannot assign a numeric value to a configuration path");
            else if constexpr (std::is_same_v<T,bool OutlastConfig::*>) config.*field=value!=0;
            else if constexpr (std::is_same_v<T,bool OutlastComfortSettings::*>) config.comfort.*field=value!=0;
            else if constexpr (std::is_same_v<T,MovementReference OutlastComfortSettings::*>)
                config.comfort.*field=static_cast<MovementReference>(int(value));
            else config.comfort.*field=value;
        },member);
    }
};

inline constexpr std::array outlast_settings{
    OutlastSetting{"vr.hit_distortion","Hit rotation / zoom",&OutlastComfortSettings::hit_distortion,0,1,1,{},true},
    OutlastSetting{"vr.bloom","Bloom",&OutlastComfortSettings::bloom,0,1,1,{},true},
    OutlastSetting{"vr.film_grain","Film grain",&OutlastComfortSettings::film_grain,0,1,1,{},true},
    OutlastSetting{"vr.body_follow","Body follows head",&OutlastComfortSettings::body_follow,0,1,1,{},true},
    OutlastSetting{"vr.body_follow_deadzone","Body follow deadzone",&OutlastComfortSettings::body_follow_deadzone,
        10,90,5,"deg",true},
    OutlastSetting{"vr.hand_pickup_radius","Hand pickup distance",&OutlastComfortSettings::hand_pickup_radius,
        .1f,.6f,.05f,"m",true},
    OutlastSetting{"vr.vignette_strength","Movement vignette",&OutlastComfortSettings::vignette_strength,
        0,1,.1f,"%",true},
    OutlastSetting{"vr.snap_turn_degrees","Snap turn",&OutlastComfortSettings::snap_turn_degrees,15,90,15,"deg"},
    OutlastSetting{"vr.seated","Seated mode",&OutlastComfortSettings::seated},
    OutlastSetting{"vr.left_handed","Left handed",&OutlastComfortSettings::left_handed},
    OutlastSetting{"vr.toggle_sprint","Toggle sprint",&OutlastComfortSettings::toggle_sprint},
    OutlastSetting{"vr.movement_reference","Movement reference",&OutlastComfortSettings::movement_reference,0,3},
    OutlastSetting{"vr.world_scale","World scale",&OutlastComfortSettings::world_scale,.5f,2,.1f},
    OutlastSetting{"experimental.physical_camcorder","Physical camcorder",&OutlastConfig::physical_camcorder},
    OutlastSetting{"experimental.instant_pickups","Instant pickups",&OutlastConfig::instant_pickups},
    OutlastSetting{"game.executable","Game executable",&OutlastConfig::game_executable},
    OutlastSetting{"vr.openxr_loader","OpenXR loader",&OutlastConfig::openxr_loader},
};
inline constexpr std::array<std::string_view,4> outlast_movement_references{"game","head","main_hand","off_hand"};
inline constexpr unsigned outlast_setting_index(std::string_view key) {
    for (unsigned i=0;i<outlast_settings.size();++i) if (outlast_settings[i].key==key) return i;
    throw std::logic_error("Unknown configuration setting");
}
inline constexpr auto outlast_menu_settings=[] {
    constexpr auto count=[] {
        unsigned result=0;
        for (const auto& setting:outlast_settings) if (setting.in_menu()) ++result;
        return result;
    }();
    std::array<unsigned,count> result{};
    unsigned row=0;
    for (unsigned i=0;i<outlast_settings.size();++i)
        if (outlast_settings[i].in_menu()) result[row++]=i;
    return result;
}();

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
            if (std::none_of(outlast_settings.begin(),outlast_settings.end(),[&](const auto& setting) {
                return setting.key.substr(0,setting.key.find('.'))==section;
            }))
                throw error("unknown section "+section);
            continue;
        }
        const auto split=line.find('=');
        if (split==std::string::npos) throw error("expected key=value");
        const auto key=section+"."+trim(line.substr(0,split));
        auto value=trim(line.substr(split+1));
        if (!seen.insert(key).second) throw error("duplicate setting "+key);
        const auto setting=std::find_if(outlast_settings.begin(),outlast_settings.end(),
            [&](const auto& entry) {return entry.key==key;});
        if (setting==outlast_settings.end()) throw error("unknown setting "+key);
        if (setting->boolean()) {
            if (value!="true" && value!="false") throw error("expected true or false");
            setting->assign(result,value=="true");
        } else if (!setting->choice() && !setting->path()) {
            std::istringstream number(value);number.imbue(std::locale::classic());
            float parsed=0;
            if (!(number>>parsed) || !(number>>std::ws).eof() || !std::isfinite(parsed))
                throw error("expected a finite number");
            if ((parsed<setting->minimum || parsed>setting->maximum) &&
                !(key=="vr.snap_turn_degrees" && parsed==0)) {
                std::ostringstream range;range.imbue(std::locale::classic());
                range<<key<<" must be "<<(key=="vr.snap_turn_degrees"?"0 or ":"")
                    <<setting->minimum<<".."<<setting->maximum;
                throw error(range.str());
            }
            setting->assign(result,parsed);
        } else if (setting->choice()) {
            const auto choice=std::find(outlast_movement_references.begin(),outlast_movement_references.end(),value);
            if (choice==outlast_movement_references.end())
                throw error("movement_reference must be game, head, main_hand or off_hand");
            setting->assign(result,float(choice-outlast_movement_references.begin()));
        } else {
            if (!value.empty() && value.front()=='"') {
                if (value.size()<2 || value.back()!='"') throw error("unterminated quoted path");
                value=value.substr(1,value.size()-2);
            }
            if (value.find('\0')!=std::string::npos) throw error("path contains a NUL byte");
            result.*std::get<std::filesystem::path OutlastConfig::*>(setting->member)=std::filesystem::u8path(value);
        }
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
