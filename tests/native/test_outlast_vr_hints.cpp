#include "outlast_vr_hints.h"
#include <algorithm>
#include <iostream>

namespace {
unsigned checks=0,copies=0,native_formats=0;
void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
olvr::NativeUiString* WINAPI copy(olvr::NativeUiString* output,const olvr::NativeUiString* input) {
    ++copies;
    output->data=new wchar_t[input->count];
    output->count=output->capacity=input->count;
    std::copy_n(input->data,input->count,output->data);
    return output;
}
olvr::NativeUiString* WINAPI original(olvr::NativeUiString* output,const olvr::NativeUiString* input) {
    ++native_formats;
    return copy(output,input);
}
}
int main() {
    try {
        using namespace olvr;
        OutlastConfig config{};
        for (const bool left:{false,true}) {
            config.comfort.left_handed=left;
            const auto text=outlast_vr_hint(L"Use {OLA_Use}, jump {OLA_Jump}, notes {OLA_ShowEvidenceMenu}.",config);
            require(text && text->find(left?L"(left grip)":L"(right grip)")!=text->npos,
                "Interaction hint does not match the dominant grip");
            require(text->find(left?L"(left primary action)":L"(right primary action)")!=text->npos &&
                text->find(left?L"(right primary action)":L"(left primary action)")!=text->npos,
                "Handedness did not swap gameplay hint actions");
            for (const auto action:{L"OLA_AnalogLeanLeft",L"OLA_AnalogLeanRight",L"OLA_Crouch",
                    L"OLA_CrouchToggle",L"OLA_Jump",L"OLA_LeanLeft",L"OLA_LeanRight",L"OLA_Reload",
                    L"OLA_Run",L"OLA_ShowEvidenceMenu",L"OLA_ShowRecordingMenu",L"OLA_ShowTabMenu",
                    L"OLA_ToggleCamcorder",L"OLA_ToggleNightVision",L"OLA_Use",L"OLA_ZoomContinuousIn",
                    L"OLA_ZoomContinuousOut",L"OLA_ZoomImpulseIn",L"OLA_ZoomImpulseOut"})
                require(outlast_vr_action_hint(action,config).has_value(),"A shipped action token retained Xbox hints");
        }
        config.comfort.left_handed=false;
        require(!outlast_vr_hint(L"Keep this subtitle unchanged.",config),"Narrative text was rewritten");
        require(!outlast_vr_hint(L"{OLA_Unknown}",config),"Unknown action did not retain the native fallback");
        require(!outlast_vr_hint(L"{OLA_Use",config),"Malformed action was replaced");
        require(outlast_vr_hint(L"{OLA_Unknown} {OLA_Use} {OLA_Use}",config)==
            std::optional<std::wstring>(L"{OLA_Unknown} (right grip) (right grip)"),
            "Repeated actions or unknown-token boundaries were corrupted");
        for (const auto key:{L"Tutorial_Camera",L"Tutorial_Camera2_Keyboard",L"Tutorial_Camera2_Gamepad"}) {
            require(outlast_vr_tutorial(key,config)->find(L"physical camcorder is off")!=std::wstring::npos,
                "Default tutorial described the experimental camcorder as enabled");
            config.physical_camcorder=true;
            require(outlast_vr_tutorial(key,config)->find(L"either grip")!=std::wstring::npos,
                "Physical camcorder hint did not describe its actual grab gesture");
            config.physical_camcorder=false;
        }
        require(!outlast_vr_tutorial(L"UnknownTutorial",config),"Unknown tutorial did not retain localization");
        require(outlast_vr_tutorial(L"Tutorial_Peak_Gamepad",config)==
            outlast_vr_tutorial(L"Tutorial_Peak_Keyboard",config),"Peeking still depended on Xbox controls");
        config.comfort.toggle_sprint=true;
        require(outlast_vr_tutorial(L"Tutorial_Run",config)->find(L"Click again")!=std::wstring::npos,
            "Toggle sprint tutorial told the player to hold the stick");
        for (const std::wstring source:{L"Press {OLA_Use}.",L"{OLA_Unknown} {OLA_Use}",L"Subtitle"}) {
            const NativeUiString input{const_cast<wchar_t*>(source.c_str()),INT(source.size()+1),INT(source.size()+1)};
            NativeUiString output{};
            copies=native_formats=0;
            require(outlast_format_vr_hint(&output,&input,&original,&copy,config)==&output,
                "Native string return ABI changed");
            require(output.data!=input.data && output.count==INT(std::wcslen(output.data)+1),
                "Hint returned borrowed storage instead of a native-owned string");
            require(copies==1 && native_formats==(source==L"Press {OLA_Use}."?0u:1u),
                "Known VR actions still performed native Xbox-binding lookups");
            require(source==input.data,"Hint formatter mutated its borrowed input");
            delete[] output.data;
        }
        NativeUiString empty{},output{};
        bool rejected=false;
        empty.count=-1;
        try {outlast_format_vr_hint(&output,&empty,&original,&copy,config);}
        catch (const std::invalid_argument&) {rejected=true;}
        require(rejected,"Invalid native string size was accepted");
        std::cout<<"PASS: native_vr_hint_checks="<<checks
            <<"; handedness; grip_actions; native_owned_copy; unknown_passthrough; no_xbox_lookup\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
