#define XR_NO_PROTOTYPES
#include "openxr_motion_input.h"
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
    bool stale_fault=false,swap_fault=false,all_unsupported=false;
    bool neutral_controls=false,estimated_left=false;
    bool fixture_crouch=false,fixture_notes=false,fixture_menu=false;
    float fixture_use=0;
    std::array<float,2> fixture_grab{};
    bool fixture_nv=false,fixture_reload=false,fixture_jump=false,fixture_sprint=false;
    XrVector2f fixture_move{},fixture_turn{};
    XrVector3f fixture_head{};
    std::array<XrVector3f,2> fixture_offsets{};
    std::array<XrQuaternionf,2> fixture_rotations{{{0,0,0,1},{0,0,0,1}}};
    bool fixture_independent_aim=false;
    std::array<XrQuaternionf,2> fixture_aim_rotations{{{0,0,0,1},{0,0,0,1}}};
    XrTime first_time=0;
    bool left(XrPath path) const {return path==paths.at("/user/hand/left");}
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
    ++fake.suggestions;return fake.all_unsupported?XR_ERROR_PATH_UNSUPPORTED:XR_SUCCESS;
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
    out->isActive=fake.active;return XR_SUCCESS;
}
XrResult XRAPI_CALL float_state(XrSession,const XrActionStateGetInfo* info,XrActionStateFloat* out) {
    query(info,XR_ACTION_TYPE_FLOAT_INPUT);
    out->isActive=fake.active;out->currentState=fake.neutral_controls?0:fake.left(info->subactionPath)?.25f:.75f;
    if (fake.neutral_controls && !fake.left(info->subactionPath) &&
        fake.actions.at(info->action).first=="trigger") out->currentState=fake.fixture_use;
    if (fake.neutral_controls && fake.actions.at(info->action).first=="squeeze")
        out->currentState=fake.fixture_grab[fake.left(info->subactionPath)?0:1];
    return XR_SUCCESS;
}
XrResult XRAPI_CALL bool_state(XrSession,const XrActionStateGetInfo* info,XrActionStateBoolean* out) {
    query(info,XR_ACTION_TYPE_BOOLEAN_INPUT);
    out->isActive=fake.active;out->currentState=!fake.neutral_controls && !fake.left(info->subactionPath);
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
    out->isActive=fake.active;out->currentState={move?.3f:-.4f,fake.bad_axis?1.5f:.6f};
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
        if (argc==2) {
            fake.stale_fault=std::strcmp(argv[1],"--fault-stale")==0;
            fake.swap_fault=std::strcmp(argv[1],"--fault-hands")==0;
            expect(fake.stale_fault || fake.swap_fault,"Unknown motion control");
        } else expect(argc==1,"Invalid motion test arguments");
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
