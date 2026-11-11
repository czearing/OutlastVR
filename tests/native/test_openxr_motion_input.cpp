#define XR_NO_PROTOTYPES
#include "openxr_motion_input.h"
#include "outlast_comfort.h"
#include <iostream>
#include <map>

using namespace olvr;
namespace motion_mock {
unsigned checks=0;
void expect(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejected(F&& action,const char* message) {
    bool threw=false;
    try {action();} catch (const std::invalid_argument&) {threw=true;}
    expect(threw,message);
}
struct Fake {
    XrInstance instance=reinterpret_cast<XrInstance>(1);
    XrSession session=reinterpret_cast<XrSession>(2);
    XrSpace base=reinterpret_cast<XrSpace>(3);
    XrActionSet set=reinterpret_cast<XrActionSet>(4);
    std::map<std::string,XrPath> paths;
    std::map<XrAction,std::pair<std::string,XrActionType>> actions;
    std::map<XrSpace,std::pair<XrAction,XrPath>> spaces;
    unsigned suggestions=0,attached=0,syncs=0,located=0,destroyed_spaces=0,destroyed_sets=0;
    unsigned haptics=0,stops=0;
    std::array<unsigned,2> hand_haptics{},hand_stops{};
    std::array<XrHapticVibration,2> last_haptic{};
    std::array<bool,2> vibrating{};
    bool focused=true,active=true,lost_left=false,bad_pose=false,bad_axis=false,partial_failure=false;
    std::array<bool,2> hand_active{{true,true}};
    std::array<bool,2> axis_active{{true,true}};
    bool stale_fault=false,swap_fault=false,all_unsupported=false;
    std::string supported_profile;
    unsigned accepted_profiles=0;
    bool neutral_controls=false,estimated_left=false;
    bool face_buttons=true,menu_button=true,stick_buttons=true;
    bool fixture_crouch=false,fixture_notes=false,fixture_menu=false;
    float fixture_use=0;
    std::array<float,2> fixture_grab{};
    bool fixture_nv=false,fixture_reload=false,fixture_jump=false,fixture_sprint=false;
    XrVector2f fixture_move{},fixture_turn{};
    XrVector3f fixture_head{};
    std::array<XrVector3f,2> fixture_offsets{};
    std::array<XrQuaternionf,2> fixture_rotations{{{0,0,0,1},{0,0,0,1}}};
    bool fixture_independent_aim=false;
    bool fixture_aim_tracked=true;
    std::array<XrQuaternionf,2> fixture_aim_rotations{{{0,0,0,1},{0,0,0,1}}};
    XrTime first_time=0;
    bool left(XrPath path) const {return path==paths.at("/user/hand/left");}
    bool active_hand(XrPath path) const {return active && hand_active[left(path)?0:1];}
} fake;

XrResult XRAPI_CALL string_path(XrInstance instance,const char* text,XrPath* out) {
    expect(instance==fake.instance && text && out,"Invalid path resolution contract");
    auto found=fake.paths.find(text);
    if (found==fake.paths.end()) found=fake.paths.emplace(text,XrPath(fake.paths.size()+1)).first;
    *out=found->second;return XR_SUCCESS;
}
XrResult XRAPI_CALL create_set(XrInstance instance,const XrActionSetCreateInfo* info,XrActionSet* out) {
    expect(instance==fake.instance && info->type==XR_TYPE_ACTION_SET_CREATE_INFO &&
        std::strcmp(info->actionSetName,"outlast_motion")==0,"Invalid motion action set");
    *out=fake.set;return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_set(XrActionSet set) {
    expect(set==fake.set && fake.spaces.empty(),"Controller action set destroyed before its spaces");
    ++fake.destroyed_sets;return XR_SUCCESS;
}
XrResult XRAPI_CALL create_action(XrActionSet set,const XrActionCreateInfo* info,XrAction* out) {
    expect(set==fake.set && info->type==XR_TYPE_ACTION_CREATE_INFO &&
        info->countSubactionPaths==2 && fake.left(info->subactionPaths[0]) &&
        !fake.left(info->subactionPaths[1]),"Invalid controller subaction paths");
    *out=reinterpret_cast<XrAction>(100+fake.actions.size());
    fake.actions.emplace(*out,std::make_pair(info->actionName,info->actionType));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create_space(XrSession session,const XrActionSpaceCreateInfo* info,XrSpace* out) {
    expect(session==fake.session && info->type==XR_TYPE_ACTION_SPACE_CREATE_INFO &&
        fake.actions.at(info->action).second==XR_ACTION_TYPE_POSE_INPUT &&
        info->poseInActionSpace.orientation.w==1,"Invalid controller action space");
    if (fake.partial_failure && fake.spaces.size()==2) return XR_ERROR_RUNTIME_FAILURE;
    *out=reinterpret_cast<XrSpace>(200+fake.spaces.size());
    fake.spaces.emplace(*out,std::make_pair(info->action,info->subactionPath));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace space) {
    expect(fake.spaces.erase(space)==1,"Controller space leaked or destroyed twice");
    ++fake.destroyed_spaces;return XR_SUCCESS;
}
XrResult XRAPI_CALL suggest(XrInstance instance,const XrInteractionProfileSuggestedBinding* info) {
    expect(instance==fake.instance && info->type==XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING &&
        info->countSuggestedBindings>=15,"Incomplete controller profile bindings");
    for (uint32_t i=0;i<info->countSuggestedBindings;++i)
        expect(fake.actions.count(info->suggestedBindings[i].action)==1 &&
            info->suggestedBindings[i].binding!=XR_NULL_PATH,"Invalid suggested action binding");
    ++fake.suggestions;
    const auto supported=fake.paths.find(fake.supported_profile);
    if (fake.all_unsupported || (!fake.supported_profile.empty() &&
        (supported==fake.paths.end() || supported->second!=info->interactionProfile)))
        return XR_ERROR_PATH_UNSUPPORTED;
    ++fake.accepted_profiles;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL attach(XrSession session,const XrSessionActionSetsAttachInfo* info) {
    expect(session==fake.session && info->countActionSets==1 && info->actionSets[0]==fake.set &&
        fake.spaces.size()==4,"Motion actions attached without both grip and aim spaces");
    ++fake.attached;return XR_SUCCESS;
}
XrResult XRAPI_CALL sync(XrSession session,const XrActionsSyncInfo* info) {
    expect(session==fake.session && info->countActiveActionSets==1 &&
        info->activeActionSets[0].actionSet==fake.set &&
        info->activeActionSets[0].subactionPath==XR_NULL_PATH,"Motion synchronization scope differs");
    ++fake.syncs;return fake.focused?XR_SUCCESS:XR_SESSION_NOT_FOCUSED;
}
void query(const XrActionStateGetInfo* info,XrActionType type) {
    expect(info->type==XR_TYPE_ACTION_STATE_GET_INFO && fake.actions.at(info->action).second==type,
        "Motion query used the wrong action type");
    expect(fake.left(info->subactionPath) ||
        info->subactionPath==fake.paths.at("/user/hand/right"),"Motion query lost its hand identity");
}
XrResult XRAPI_CALL pose_state(XrSession,const XrActionStateGetInfo* info,XrActionStatePose* out) {
    query(info,XR_ACTION_TYPE_POSE_INPUT);
    out->isActive=fake.active_hand(info->subactionPath);return XR_SUCCESS;
}
XrResult XRAPI_CALL float_state(XrSession,const XrActionStateGetInfo* info,XrActionStateFloat* out) {
    query(info,XR_ACTION_TYPE_FLOAT_INPUT);
    out->isActive=fake.active_hand(info->subactionPath);
    out->currentState=fake.neutral_controls?0:fake.left(info->subactionPath)?.25f:.75f;
    if (fake.neutral_controls && !fake.left(info->subactionPath) &&
        fake.actions.at(info->action).first=="trigger") out->currentState=fake.fixture_use;
    if (fake.neutral_controls && fake.actions.at(info->action).first=="squeeze")
        out->currentState=fake.fixture_grab[fake.left(info->subactionPath)?0:1];
    return XR_SUCCESS;
}
XrResult XRAPI_CALL bool_state(XrSession,const XrActionStateGetInfo* info,XrActionStateBoolean* out) {
    query(info,XR_ACTION_TYPE_BOOLEAN_INPUT);
    out->isActive=fake.active_hand(info->subactionPath);
    out->currentState=!fake.neutral_controls && !fake.left(info->subactionPath);
    const auto& name=fake.actions.at(info->action).first;
    if ((!fake.face_buttons && (name=="primary" || name=="secondary")) ||
        (!fake.menu_button && name=="menu") ||
        (!fake.stick_buttons && name=="stick_click")) out->isActive=XR_FALSE;
    if (fake.neutral_controls) {
        const auto& action=fake.actions.at(info->action).first;
        const bool left=fake.left(info->subactionPath);
        out->currentState=(action=="secondary" && !left && fake.fixture_crouch) ||
            (action=="primary" && left && fake.fixture_notes) || (action=="menu" && left && fake.fixture_menu) ||
            (action=="stick_click" && !left && fake.fixture_nv) ||
            (action=="secondary" && left && fake.fixture_reload) ||
            (action=="primary" && !left && fake.fixture_jump) ||
            (action=="stick_click" && left && fake.fixture_sprint);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL vector_state(XrSession,const XrActionStateGetInfo* info,XrActionStateVector2f* out) {
    query(info,XR_ACTION_TYPE_VECTOR2F_INPUT);
    const bool move=fake.actions.at(info->action).first=="move";
    expect(fake.left(info->subactionPath)==move,"Move and turn sticks switched hands");
    out->isActive=fake.active_hand(info->subactionPath) && fake.axis_active[move?0:1];
    out->currentState={move?.3f:-.4f,fake.bad_axis?1.5f:.6f};
    if (fake.neutral_controls) out->currentState=move?fake.fixture_move:fake.fixture_turn;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSpace space,XrSpace base,XrTime time,XrSpaceLocation* out) {
    expect(base==fake.base && time>0 && out->type==XR_TYPE_SPACE_LOCATION,
        "Controller pose did not use the render reference space and predicted time");
    const auto& binding=fake.spaces.at(space);
    bool left=fake.left(binding.second);
    if (fake.swap_fault) left=!left;
    if (!fake.first_time) fake.first_time=time;
    out->locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (left && fake.lost_left) out->locationFlags&=~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if (left && fake.lost_left) out->locationFlags&=~XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (left && fake.estimated_left)
        out->locationFlags&=~(XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
    out->pose.position={left?-.3f:.3f,1.2f,float(fake.stale_fault?fake.first_time:time)/10000};
    if (fake.neutral_controls) {
        const auto& delta=fake.fixture_offsets[left?0:1];
        out->pose.position={fake.fixture_head.x+(left?-.3f:.3f)+delta.x,
            fake.fixture_head.y-.45f+delta.y,fake.fixture_head.z-.4f+delta.z};
    }
    out->pose.orientation=fake.fixture_rotations[left?0:1];
    if (!fake.fixture_aim_tracked && fake.actions.at(binding.first).first=="aim_pose")
        out->locationFlags=0;
    if (fake.fixture_independent_aim && fake.actions.at(binding.first).first=="aim_pose")
        out->pose.orientation=fake.fixture_aim_rotations[left?0:1];
    if (fake.bad_pose) out->pose.orientation.w=.5f;
    ++fake.located;return XR_SUCCESS;
}
XrResult XRAPI_CALL haptic(XrSession session,const XrHapticActionInfo* info,const XrHapticBaseHeader* header) {
    expect(session==fake.session && fake.actions.at(info->action).second==XR_ACTION_TYPE_VIBRATION_OUTPUT,
        "Haptic request used another action");
    const auto& value=*reinterpret_cast<const XrHapticVibration*>(header);
    expect(value.type==XR_TYPE_HAPTIC_VIBRATION && std::isfinite(value.amplitude) &&
        value.amplitude>=0 && value.amplitude<=1 && value.duration>0 &&
        value.duration<=1000000000 && std::isfinite(value.frequency) && value.frequency>=0,
        "Unbounded or invalid controller vibration");
    const unsigned hand=fake.left(info->subactionPath)?0:1;
    fake.last_haptic[hand]=value;++fake.hand_haptics[hand];
    fake.vibrating[hand]=true;
    ++fake.haptics;return XR_SUCCESS;
}
XrResult XRAPI_CALL stop(XrSession session,const XrHapticActionInfo* info) {
    expect(session==fake.session &&
        fake.actions.at(info->action).second==XR_ACTION_TYPE_VIBRATION_OUTPUT,
        "Haptic stop used the wrong session/action");
    ++fake.hand_stops[fake.left(info->subactionPath)?0:1];
    fake.vibrating[fake.left(info->subactionPath)?0:1]=false;
    ++fake.stops;return XR_SUCCESS;
}
template<class F> void export_function(PFN_xrVoidFunction* out,F function) {
    static_assert(sizeof(*out)==sizeof(function));std::memcpy(out,&function,sizeof(function));
}
XrResult XRAPI_CALL resolve(XrInstance,const char* name,PFN_xrVoidFunction* out) {
#define BIND(text,function) if (std::strcmp(name,"xr" text)==0) {export_function(out,&function);return XR_SUCCESS;}
    BIND("StringToPath",string_path) BIND("CreateActionSet",create_set) BIND("DestroyActionSet",destroy_set)
    BIND("CreateAction",create_action) BIND("CreateActionSpace",create_space) BIND("DestroySpace",destroy_space)
    BIND("SuggestInteractionProfileBindings",suggest) BIND("AttachSessionActionSets",attach)
    BIND("SyncActions",sync) BIND("GetActionStatePose",pose_state) BIND("GetActionStateFloat",float_state)
    BIND("GetActionStateBoolean",bool_state) BIND("GetActionStateVector2f",vector_state)
    BIND("LocateSpace",locate) BIND("ApplyHapticFeedback",haptic) BIND("StopHapticFeedback",stop)
#undef BIND
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
}

#ifndef OLVR_MOTION_MOCK_ONLY
int main(int argc,char** argv) {
    using namespace motion_mock;
    try {
        const std::string mode=argc==2?argv[1]:"";
        const bool pause_test=mode=="--pause-chord";
        const bool sector_test=mode=="--fallback-gestures";
        const bool recovery_test=mode=="--button-recovery";
        const bool snap_recovery=mode=="--snap-recovery";
        if (argc==2) {
            fake.stale_fault=std::strcmp(argv[1],"--fault-stale")==0;
            fake.swap_fault=std::strcmp(argv[1],"--fault-hands")==0;
            expect(fake.stale_fault || fake.swap_fault || pause_test || sector_test || recovery_test || snap_recovery,
                "Unknown motion control");
        } else expect(argc==1,"Invalid motion test arguments");
        if (snap_recovery) {
            unsigned scenarios=0;
            for (const bool left_handed:{false,true}) for (const bool whole_hand:{false,true})
            for (const bool held_before:{false,true}) {
                fake=Fake{};fake.neutral_controls=true;
                const unsigned hand=left_handed?0:1;
                auto& axis=hand?fake.fixture_turn:fake.fixture_move;
                (hand?fake.fixture_move:fake.fixture_turn)={.3f,.6f};
                {
                    OpenXRMotionInput input(&resolve,fake.instance,fake.session);
                    OutlastComfort comfort;
                    OutlastComfortSettings options;
                    options.left_handed=left_handed;options.snap_turn_degrees=30;
                    const EyePose head{{.4f,.1f,.2f},{0,0,0,1}};
                    auto sample=[&](XrTime time) {
                        const auto frame=input.sample(fake.base,time);
                        comfort.update(options,frame,head,true,UINT64(time));
                        return frame;
                    };
                    auto heading=[&](unsigned snaps,const char* message) {
                        const float angle=snaps*.5235987756f;
                        const auto forward=rotate(comfort.pose(head).orientation,{0,0,-1});
                        const auto delta=add(forward,scale(Vec3{std::sin(angle),0,-std::cos(angle)},-1));
                        expect(dot(delta,delta)<.000001f,message);
                    };
                    sample(1000);
                    axis.x=held_before?1.f:0.f;sample(2000);
                    heading(held_before?1:0,"Initial active snap turn was lost");
                    (whole_hand?fake.hand_active:fake.axis_active)[hand]=false;
                    const auto unavailable=sample(3000);
                    const auto own=hand?unavailable.turn:unavailable.move;
                    const auto other=hand?unavailable.move:unavailable.turn;
                    expect(own.x==0 && own.y==0 && other.x==.3f && other.y==.6f,
                        "One inactive stick disabled the independent controller");
                    expect(!(hand?unavailable.turn_active:unavailable.move_active) &&
                        (hand?unavailable.move_active:unavailable.turn_active),
                        "A synthetic neutral axis was reported as active or lost its hand identity");
                    (whole_hand?fake.hand_active:fake.axis_active)[hand]=true;
                    axis.x=1;
                    for (const XrTime time:{4000,4100}) {
                        const auto restored=sample(time);
                        expect((hand?restored.turn:restored.move).x==1,
                            "Snap rearming suppressed ordinary analog input");
                        heading(held_before?1:0,"Inactive stick generated a phantom snap on recovery");
                    }
                    axis.x=0;sample(5000);
                    axis.x=1;sample(6000);
                    heading(held_before?2:1,"An active neutral and fresh deflection did not recover snap turning");
                }
                expect(fake.destroyed_spaces==4 && fake.destroyed_sets==1,
                    "Snap-recovery test leaked controller resources");
                ++scenarios;
            }
            for (const bool left_handed:{false,true}) {
                fake=Fake{};fake.neutral_controls=true;
                const unsigned hand=left_handed?0:1;
                fake.axis_active[1-hand]=false;
                fake.lost_left=left_handed;
                {
                    OpenXRMotionInput input(&resolve,fake.instance,fake.session);
                    OutlastComfort comfort;
                    OutlastComfortSettings options;
                    options.left_handed=left_handed;options.snap_turn_degrees=30;
                    const EyePose head{{0,0,0},{0,0,0,1}};
                    comfort.update(options,input.sample(fake.base,1000),head,true,1000);
                    (hand?fake.fixture_turn:fake.fixture_move).x=1;
                    comfort.update(options,input.sample(fake.base,2000),head,true,2000);
                    const auto forward=rotate(comfort.pose(head).orientation,{0,0,-1});
                    expect(std::abs(forward.x-.5f)<.00001f && std::abs(forward.z+.8660254f)<.00001f,
                        "Unrelated stick inactivity or optical pose loss disabled active snap turning");
                }
            }
            std::cout<<"PASS: snap_action_recovery_scenarios="<<scenarios
                <<"; both_handedness; action_and_controller_activity; held_and_unseen_deflections; "
                <<"active_neutral_rearms; ordinary_axes_preserved; independent_hands; optical_loss_preserved\n";
            return 0;
        }
        if (recovery_test) {
            enum class Kind {primary,secondary,menu,click};
            struct Case {const char* name;unsigned hand;bool Fake::*pressed;Kind kind;float sector;};
            const Case cases[]{
                {"left-primary",0,&Fake::fixture_notes,Kind::primary,0},
                {"right-primary",1,&Fake::fixture_jump,Kind::primary,0},
                {"left-secondary",0,&Fake::fixture_reload,Kind::secondary,0},
                {"right-secondary",1,&Fake::fixture_crouch,Kind::secondary,0},
                {"menu",0,&Fake::fixture_menu,Kind::menu,0},
                {"left-click",0,&Fake::fixture_sprint,Kind::click,0},
                {"right-click",1,&Fake::fixture_nv,Kind::click,0},
                {"left-pad-primary",0,&Fake::fixture_sprint,Kind::primary,1},
                {"right-pad-primary",1,&Fake::fixture_nv,Kind::primary,1},
                {"left-pad-secondary",0,&Fake::fixture_sprint,Kind::secondary,-1},
                {"right-pad-secondary",1,&Fake::fixture_nv,Kind::secondary,-1}};
            unsigned scenarios=0;
            for (const auto& test:cases) for (unsigned interruption=0;interruption<4;++interruption)
                for (const bool held_before:{false,true}) {
                    fake=Fake{};fake.neutral_controls=true;fake.face_buttons=!test.sector;
                    auto& held=fake.*test.pressed;
                    (test.hand?fake.fixture_turn:fake.fixture_move)=test.kind==Kind::click?
                        XrVector2f{.3f,.4f}:XrVector2f{0,test.sector};
                    {
                        OpenXRMotionInput input(&resolve,fake.instance,fake.session);
                        auto pressed=[&](const MotionFrame& frame) {
                            const auto& hand=frame.hands[test.hand];
                            return test.kind==Kind::menu?frame.menu:test.kind==Kind::primary?hand.primary:
                                test.kind==Kind::secondary?hand.secondary:hand.stick_click;
                        };
                        input.sample(fake.base,1000);
                        held=held_before;
                        expect(pressed(input.sample(fake.base,2000))==held_before,
                            "Initial digital action did not match its physical button");
                        if (interruption==0) fake.focused=false;
                        if (interruption==1) input.suspend();
                        if (interruption==2) fake.hand_active[test.hand]=false;
                        if (interruption==3) {
                            if (test.sector) fake.face_buttons=true;
                            else if (test.kind==Kind::menu) fake.menu_button=false;
                            else if (test.kind==Kind::click) fake.stick_buttons=false;
                            else fake.face_buttons=false;
                        }
                        if (interruption!=1)
                            expect(!pressed(input.sample(fake.base,3000)),
                                "An unavailable digital action retained input");
                        held=true;
                        fake.focused=true;fake.hand_active[test.hand]=true;
                        fake.face_buttons=!test.sector;fake.menu_button=fake.stick_buttons=true;
                        for (const XrTime time:{4000,4100}) {
                            const auto frame=input.sample(fake.base,time);
                            expect(!pressed(frame),
                                (std::string("Held ")+test.name+" replayed after recovery; interruption="+
                                    std::to_string(interruption)+"; held_before="+std::to_string(held_before)).c_str());
                            if (test.kind==Kind::click && interruption<2) {
                                const auto axis=test.hand?frame.turn:frame.move;
                                expect(axis.x==.3f && axis.y==.4f,
                                    "Rearming a normal stick click disabled its movement axis");
                            }
                        }
                        held=false;
                        expect(!pressed(input.sample(fake.base,5000)),"Button release generated a press");
                        held=true;
                        expect(pressed(input.sample(fake.base,6000)),
                            "A fresh digital press remained blocked after recovery");
                    }
                    expect(fake.destroyed_spaces==4 && fake.destroyed_sets==1,
                        "Button-recovery test leaked controller resources");
                    ++scenarios;
                }
            fake=Fake{};fake.neutral_controls=true;
            {
                OpenXRMotionInput input(&resolve,fake.instance,fake.session);
                input.sample(fake.base,1000);
                fake.hand_active[0]=false;input.sample(fake.base,2000);
                fake.fixture_notes=fake.fixture_jump=true;
                auto frame=input.sample(fake.base,3000);
                expect(!frame.hands[0].primary && frame.hands[1].primary,
                    "One missing controller disabled the other hand's fresh input");
                fake.hand_active[0]=true;
                expect(!input.sample(fake.base,4000).hands[0].primary,
                    "An inactive hand's press replayed on reconnection");
                fake.fixture_notes=false;input.sample(fake.base,5000);
                fake.fixture_notes=true;fake.lost_left=true;
                frame=input.sample(fake.base,6000);
                expect(frame.hands[0].primary && !frame.hands[0].grip_tracked,
                    "Optical pose loss incorrectly disabled an active controller button");
            }
            expect(fake.destroyed_spaces==4 && fake.destroyed_sets==1,
                "Per-hand recovery test leaked controller resources");
            std::cout<<"PASS: digital_button_recovery_scenarios="<<scenarios
                <<"; focus_suspend_hand_activity_profile; held_and_unseen_presses; "
                <<"fresh_release_rearm; independent_hands; optical_loss_preserves_buttons\n";
            return 0;
        }
        if (pause_test || sector_test) {
            fake.neutral_controls=true;fake.menu_button=!pause_test;fake.face_buttons=pause_test;
            {
                OpenXRMotionInput input(&resolve,fake.instance,fake.session);
                XrTime clock=100;
                bool pause_pad=false;
                auto sample=[&] {
                    if (pause_test && pause_pad) {
                        fake.fixture_sprint=fake.fixture_reload;fake.fixture_nv=fake.fixture_crouch;
                        fake.fixture_move=fake.fixture_turn={0,-1};
                    }
                    return input.sample(fake.base,++clock);
                };
                if (pause_test) for (bool pad:{false,true}) for (unsigned released=0;released<2;++released) {
                    pause_pad=pad;fake.face_buttons=!pad;
                    fake.fixture_reload=fake.fixture_crouch=false;sample();
                    fake.fixture_reload=fake.fixture_crouch=true;
                    expect(sample().menu,"Pause chord did not open the menu");
                    (released?fake.fixture_crouch:fake.fixture_reload)=false;
                    auto tail=sample();
                    expect(!tail.menu && !tail.hands[0].secondary && !tail.hands[1].secondary,
                        "Partially released pause chord leaked crouch/reload");
                    fake.menu_button=true;tail=sample();
                    expect(!tail.hands[0].secondary && !tail.hands[1].secondary,
                        "Changing the menu binding exposed a consumed pause button");
                    fake.menu_button=false;
                    fake.fixture_reload=fake.fixture_crouch=true;
                    expect(!sample().menu,"Pause chord rearmed before both buttons were released");
                    fake.focused=false;sample();fake.focused=true;
                    expect(!sample().menu,"Focus recovery replayed a held pause chord");
                    fake.fixture_reload=false;sample();fake.fixture_reload=true;tail=sample();
                    expect(!tail.menu && !tail.hands[0].secondary && !tail.hands[1].secondary,
                        "A partially released pause gesture rearmed after focus recovery");
                    fake.active=false;sample();fake.active=true;tail=sample();
                    expect(!tail.menu && !tail.hands[0].secondary && !tail.hands[1].secondary,
                        "Controller reconnection replayed a held pause chord");
                    fake.fixture_reload=fake.fixture_crouch=false;sample();
                    fake.fixture_reload=true;
                    expect(sample().hands[0].secondary,"A fresh secondary action stayed blocked after the chord");
                    fake.fixture_reload=false;sample();
                }
                if (sector_test) for (unsigned hand=0;hand<2;++hand) {
                    auto& click=hand?fake.fixture_nv:fake.fixture_sprint;
                    auto& axis=hand?fake.fixture_turn:fake.fixture_move;
                    click=false;axis={};sample();
                    click=true;axis={0,1};
                    expect(sample().hands[hand].primary,"Upward pad click did not select primary");
                    axis={};
                    auto held=sample();
                    expect(held.hands[hand].primary && !held.hands[hand].stick_click,
                        "Held pad sector click became a sprint/filter click at center");
                    axis={0,-1};held=sample();
                    expect(held.hands[hand].primary && !held.hands[hand].secondary,
                        "Held pad click changed from primary to secondary without release");
                    fake.focused=false;sample();fake.focused=true;held=sample();
                    expect(!held.hands[hand].primary && !held.hands[hand].secondary &&
                        !held.hands[hand].stick_click,"Focus recovery replayed a consumed pad gesture");
                    click=false;sample();click=true;
                    expect(sample().hands[hand].secondary,"A fresh downward pad click stayed blocked");
                    fake.face_buttons=true;held=sample();
                    expect(!held.hands[hand].stick_click,
                        "A controller profile change exposed a consumed pad click");
                    fake.face_buttons=false;held=sample();
                    expect(!held.hands[hand].primary && !held.hands[hand].secondary &&
                        !held.hands[hand].stick_click,"A held pad gesture replayed after a profile change");
                    click=false;sample();click=true;sample();
                    fake.active=false;sample();fake.active=true;held=sample();
                    expect(!held.hands[hand].secondary && !held.hands[hand].stick_click,
                        "Controller reconnection replayed a consumed pad gesture");
                    click=false;axis={};sample();click=true;
                    expect(sample().hands[hand].stick_click,"A fresh center click stayed blocked");
                    axis={0,1};held=sample();
                    expect(held.hands[hand].stick_click && !held.hands[hand].primary,
                        "Moving a held center click generated an extra face-button action");
                    click=false;axis={};sample();
                }
            }
            expect(fake.destroyed_spaces==4 && fake.destroyed_sets==1,
                "Gesture-test controller handles leaked");
            std::cout<<"PASS: "<<(pause_test?"pause_chord_release_and_focus_rearm":
                "pad_gesture_press_ownership_and_focus_rearm")<<"; both_hands; cleanup_preserved\n";
            return 0;
        }
        {
            OpenXRMotionInput input(&resolve,fake.instance,fake.session);
            expect(fake.suggestions==4 && fake.attached==1 && fake.actions.size()==11,
                "Controller initialization incomplete");
            for (XrTime time:{1000,2000,3000}) {
                const auto frame=input.sample(fake.base,time);
                expect(frame.focused && frame.time==time,"Predicted controller sample time changed");
                expect(frame.hands[0].grip_tracked && frame.hands[1].aim_tracked,
                    "Both tracked controller spaces missing");
                expect(frame.hands[0].grip.position.x==-.3f && frame.hands[1].grip.position.x==.3f,
                    "Controller hand identity oracle failed");
                expect(frame.hands[0].grip.position.z==float(time)/10000 &&
                    frame.hands[1].aim.position.z==float(time)/10000,"Controller predicted-time oracle failed");
                expect(frame.hands[0].trigger==.25f && frame.hands[1].squeeze==.75f &&
                    !frame.hands[0].primary && frame.hands[1].primary &&
                    frame.move.x==.3f && frame.turn.x==-.4f,"Controller analog/button state differs");
            }
            fake.neutral_controls=true;fake.fixture_jump=true;
            fake.fixture_independent_aim=true;
            fake.fixture_rotations[1]={-.7071067812f,0,0,.7071067812f};
            const auto independent=input.sample(fake.base,3400);
            expect(std::abs(independent.hands[1].grip.orientation.x+.7071067812f)<.00001f &&
                independent.hands[1].aim.orientation.x==0 &&
                independent.hands[1].aim.orientation.w==1,
                "Native fixture silently reused grip instead of independent tracked aim");
            fake.fixture_independent_aim=false;
            fake.fixture_rotations[1]={0,0,0,1};
            const auto jump=input.sample(fake.base,3500);
            expect(jump.hands[1].primary && !jump.hands[0].primary &&
                !jump.hands[0].stick_click,"Native jump fixture did not isolate the right primary action");
            fake.fixture_jump=false;fake.fixture_sprint=true;
            const auto sprint=input.sample(fake.base,3600);
            expect(sprint.hands[0].stick_click && !sprint.hands[1].stick_click &&
                !sprint.hands[1].primary,"Native sprint fixture did not isolate the left stick-click action");
            fake.fixture_sprint=false;
            const auto released=input.sample(fake.base,3700);
            expect(!released.hands[0].stick_click && !released.hands[1].primary,
                "Native jump/sprint fixture retained released inputs");
            fake.face_buttons=false;fake.menu_button=false;
            input.sample(fake.base,3740);
            fake.fixture_sprint=fake.fixture_nv=true;
            fake.fixture_move={0,1};fake.fixture_turn={0,-1};
            const auto pads=input.sample(fake.base,3750);
            expect(pads.hands[0].primary && pads.hands[1].secondary &&
                !pads.hands[0].stick_click && !pads.hands[1].stick_click &&
                pads.move.y==0 && pads.turn.y==0,
                "Face-less controller sectors leaked locomotion or omitted native actions");
            fake.fixture_sprint=fake.fixture_nv=false;input.sample(fake.base,3751);
            fake.fixture_sprint=fake.fixture_nv=true;
            fake.fixture_move={0,-1};
            const auto pause=input.sample(fake.base,3760);
            expect(pause.menu && !pause.hands[0].secondary && !pause.hands[1].secondary,
                "Missing menu binding did not provide an isolated pause chord");
            fake.fixture_sprint=fake.fixture_nv=false;input.sample(fake.base,3761);
            fake.fixture_sprint=fake.fixture_nv=true;
            fake.fixture_move={1,0};fake.fixture_turn={1,0};
            const auto side=input.sample(fake.base,3770);
            expect(side.hands[0].stick_click && side.hands[1].stick_click &&
                !side.hands[0].primary && !side.hands[1].secondary,
                "Face-less controller fallback removed sprint/filter/recenter clicks");
            fake.face_buttons=fake.menu_button=true;
            fake.fixture_sprint=fake.fixture_nv=false;
            fake.fixture_move=fake.fixture_turn={};
            fake.neutral_controls=false;
            fake.estimated_left=true;
            const auto estimated=input.sample(fake.base,3900);
            expect(estimated.hands[0].grip_tracked && estimated.hands[0].aim_tracked &&
                estimated.hands[0].grip.position.z==.39f,
                "Valid runtime-estimated controller pose reverted to native animation");
            fake.estimated_left=false;
            fake.lost_left=true;
            const auto lost=input.sample(fake.base,4000);
            expect(!lost.hands[0].grip_tracked && !lost.hands[0].aim_tracked &&
                lost.hands[1].grip_tracked,"Lost controller tracking reused an earlier pose");
            fake.lost_left=false;fake.active=false;
            const auto inactive=input.sample(fake.base,5000);
            expect(!inactive.hands[1].grip_tracked && inactive.hands[1].trigger==0 &&
                !inactive.hands[1].primary && inactive.move.x==0,"Inactive actions retained stale input");
            fake.active=true;fake.focused=false;
            const auto unfocused=input.sample(fake.base,6000);
            expect(!unfocused.focused && !unfocused.hands[1].grip_tracked &&
                unfocused.turn.x==0,"Unfocused session retained active controls");
            fake.focused=true;
            input.vibrate(0,.5f,50000000,120);
            expect(fake.last_haptic[0].amplitude==.5f && fake.last_haptic[0].duration==50000000 &&
                fake.last_haptic[0].frequency==120 && fake.hand_haptics[0]==1 &&
                fake.hand_haptics[1]==0,"Haptic motor amplitude/time/hand changed");
            input.stop_vibration(0);
            expect(fake.haptics==1 && fake.stops==1,"Haptic lifecycle missing");
            input.stop_vibration(0);
            expect(fake.stops==1,"Inactive rumble emitted redundant XR stop calls");
            input.vibrate(0,.3f,100000000);input.vibrate(1,.7f,100000000);
            fake.lost_left=true;input.sample(fake.base,6500);
            expect(fake.hand_stops[0]==2 && fake.hand_stops[1]==0,
                "Tracking loss failed to stop only the missing hand");
            fake.focused=false;input.sample(fake.base,6600);
            expect(fake.hand_stops[1]==1,"Focus loss retained controller vibration");
            fake.focused=true;fake.lost_left=false;
            rejected([&]{input.sample(fake.base,0);},"Zero predicted time accepted");
            rejected([&]{input.sample(XR_NULL_HANDLE,1);},"Missing reference space accepted");
            rejected([&]{input.vibrate(2,.5f,1);},"Invalid haptic hand accepted");
            rejected([&]{input.vibrate(0,1.1f,1);},"Invalid haptic amplitude accepted");
            rejected([&]{input.vibrate(0,.5f,1000000001);},"Unbounded haptic duration accepted");
            fake.bad_pose=true;
            bool pose_rejected=false;
            try {input.sample(fake.base,7000);} catch (const std::runtime_error&) {pose_rejected=true;}
            expect(pose_rejected,"Malformed tracked pose accepted");fake.bad_pose=false;
            fake.bad_axis=true;
            bool axis_rejected=false;
            try {input.sample(fake.base,8000);} catch (const std::runtime_error&) {axis_rejected=true;}
            expect(axis_rejected,"Malformed active stick accepted");
            input.vibrate(0,.4f,100000000);input.vibrate(1,.6f,100000000);
        }
        expect(fake.destroyed_spaces==4 && fake.destroyed_sets==1,"Motion input teardown leaked resources");
        expect(fake.hand_stops[0]==3 && fake.hand_stops[1]==2,
            "Active rumble was not stopped before controller teardown");
        fake=Fake{};fake.partial_failure=true;
        bool rejected_partial=false;
        try {OpenXRMotionInput input(&resolve,fake.instance,fake.session);}
        catch (const std::runtime_error&) {rejected_partial=true;}
        expect(rejected_partial && fake.destroyed_spaces==2 && fake.destroyed_sets==1,
            "Partial controller initialization did not release owned resources");
        fake=Fake{};fake.all_unsupported=true;
        bool rejected_profiles=false;
        try {OpenXRMotionInput input(&resolve,fake.instance,fake.session);}
        catch (const std::runtime_error&) {rejected_profiles=true;}
        expect(rejected_profiles && fake.attached==0 && fake.destroyed_sets==1,
            "Unsupported controller inventory falsely succeeded");
        for (const auto* profile:{
            "/interaction_profiles/oculus/touch_controller",
            "/interaction_profiles/valve/index_controller",
            "/interaction_profiles/microsoft/motion_controller",
            "/interaction_profiles/htc/vive_controller"}) {
            fake=Fake{};fake.supported_profile=profile;fake.neutral_controls=true;
            fake.fixture_move={.4f,0};fake.fixture_turn={-.6f,0};fake.fixture_use=.8f;
            {
                OpenXRMotionInput input(&resolve,fake.instance,fake.session);
                const auto frame=input.sample(fake.base,9000);
                expect(fake.suggestions==4 && fake.accepted_profiles==1 && fake.attached==1 &&
                    frame.focused && frame.hands[0].grip_tracked && frame.hands[1].aim_tracked &&
                    frame.move.x==.4f && frame.turn.x==-.6f && frame.hands[1].trigger==.8f,
                    "Rejecting optional profiles disabled the one supported controller profile");
            }
            expect(fake.destroyed_spaces==4 && fake.destroyed_sets==1,
                "Partially supported controller initialization leaked owned handles");
        }
        std::cout<<"PASS: OpenXR motion-input checks="<<checks
            <<"; predicted_time_and_hand_identity; inactive_focus_and_tracking_loss; "
            <<"analog_buttons_haptics; partial_cleanup; no_game_or_native_IK_claim; "
            <<"rumble_tracking_focus_and_cleanup_stop; no_redundant_stops\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
#endif
