#pragma once
#include <windows.h>
#include "outlast_config.h"
#include <optional>
#include <string_view>
#include <cstddef>

namespace olvr {
struct NativeUiString {
    wchar_t* data=nullptr;
    INT count=0,capacity=0;
};
static_assert(sizeof(NativeUiString)==16 && offsetof(NativeUiString,count)==8);
using NativeHintFormat=NativeUiString*(WINAPI*)(NativeUiString*,const NativeUiString*);
using NativeLocalize=NativeUiString*(WINAPI*)(NativeUiString*,const wchar_t*,const wchar_t*,
                                           const wchar_t*,const wchar_t*,INT);

inline std::optional<std::wstring> outlast_vr_action_hint(std::wstring_view action,const OutlastConfig& config) {
    const std::wstring main=config.comfort.left_handed?L"left ":L"right ";
    const std::wstring other=config.comfort.left_handed?L"right ":L"left ";
    if (action==L"OLA_Use") return main+L"grip";
    if (action==L"OLA_Jump") return main+L"primary action";
    if (action==L"OLA_Crouch" || action==L"OLA_CrouchToggle") return main+L"secondary action";
    if (action==L"OLA_Run") return other+L"stick click";
    if (action==L"OLA_Reload") return other+L"secondary action";
    if (action==L"OLA_ShowEvidenceMenu" || action==L"OLA_ShowRecordingMenu" || action==L"OLA_ShowTabMenu")
        return other+L"primary action";
    if (action==L"OLA_ToggleNightVision") return main+L"stick click";
    if (action==L"OLA_ToggleCamcorder")
        return config.physical_camcorder?L"grip at your chest":L"physical camcorder disabled";
    if (action==L"OLA_LeanLeft" || action==L"OLA_AnalogLeanLeft") return L"lean your head left";
    if (action==L"OLA_LeanRight" || action==L"OLA_AnalogLeanRight") return L"lean your head right";
    if (action==L"OLA_ZoomContinuousIn" || action==L"OLA_ZoomImpulseIn") return main+L"stick up";
    if (action==L"OLA_ZoomContinuousOut" || action==L"OLA_ZoomImpulseOut") return main+L"stick down";
    return std::nullopt;
}

inline std::optional<std::wstring> outlast_vr_hint(std::wstring_view input,const OutlastConfig& config) {
    auto position=input.find(L"{OLA_");
    if (position==input.npos) return std::nullopt;
    std::wstring output;
    output.reserve(input.size());
    std::size_t copied=0;
    bool changed=false;
    while (position!=input.npos) {
        const auto end=input.find(L'}',position+5);
        if (end==input.npos) break;
        if (const auto label=outlast_vr_action_hint(input.substr(position+1,end-position-1),config)) {
            output.append(input.substr(copied,position-copied));
            output.append(L"(").append(*label).append(L")");
            copied=end+1;changed=true;
        }
        position=input.find(L"{OLA_",end+1);
    }
    if (!changed) return std::nullopt;
    output.append(input.substr(copied));
    return output;
}

inline std::optional<std::wstring> outlast_vr_tutorial(std::wstring_view key,const OutlastConfig& config) {
    if (key==L"Tutorial_Peak_Keyboard" || key==L"Tutorial_Peak_Gamepad")
        return L"Lean your head around a corner to peek.";
    if (key==L"Tutorial_LookBehind" || key==L"Tutorial_LookBehind_Analog")
        return L"Turn your head to look behind you.";
    if (key==L"Tutorial_Camera" || key==L"Tutorial_Camera2_Keyboard" || key==L"Tutorial_Camera2_Gamepad")
        return config.physical_camcorder?
            L"Hold either grip at your chest to grab the camcorder. Release the grip to put it away.":
            L"Night vision: {OLA_ToggleNightVision}. The physical camcorder is off.";
    if (key==L"Tutorial_NightVision1" || key==L"Tutorial_NightVision2")
        return config.physical_camcorder?
            L"Night vision: {OLA_ToggleNightVision}. It only affects the camcorder screen.":
            L"Night vision: {OLA_ToggleNightVision}.";
    if (key==L"Tutorial_Run")
        return config.comfort.toggle_sprint?L"Sprint: {OLA_Run}. Click again to stop.":
            L"Sprint: hold {OLA_Run} while moving.";
    return std::nullopt;
}

inline NativeUiString* outlast_format_vr_hint(NativeUiString* output,const NativeUiString* input,
    NativeHintFormat original,NativeHintFormat copy,const OutlastConfig& config) {
    if (!output || !input || !original || !copy || input->count<0 ||
        input->count>1048576 || input->capacity<input->count ||
        (input->count && (!input->data || input->data[input->count-1])))
        throw std::invalid_argument("Invalid native controller hint string or binding");
    const std::wstring_view text=input->count?std::wstring_view(input->data,input->count-1):L"";
    const auto changed=outlast_vr_hint(text,config);
    if (!changed) return original(output,input);
    const INT count=INT(changed->size()+1);
    const NativeUiString borrowed{const_cast<wchar_t*>(changed->c_str()),count,count};
    // The retail constructor allocates the result with the game's own allocator.
    return (changed->find(L"{OLA_")==changed->npos?copy:original)(output,&borrowed);
}
}
