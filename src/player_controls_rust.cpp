#include "player_controls_rust.h"
#include "input_ffi.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
namespace mag {
namespace {
void check(int status) {if(status)throw std::runtime_error("Rust input status "+std::to_string(status));}
uint64_t now() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
MagInputFrameKey key(InputFrameKey k) {return {1,k.sample_sequence,k.player_slot,0};}
MagInputOffer offer(ControlOffer value){return {value.pulse_id,value.physical_duration_ns,uint32_t(value.down),value.source_mask};}
MagInputPolicy policy_record(const InputPolicy& value){return {uint32_t(value.mode),uint32_t(value.look_owner==LookOwner::Mouse),uint32_t(value.kbm_used_this_process)|(uint32_t(value.stick_rearmed)<<1)|(uint32_t(value.controller_connected)<<2),0};}
InputPolicy policy(MagInputPolicy value){return {InputMode(value.mode),value.mouse_owner?LookOwner::Mouse:LookOwner::Controller,bool(value.flags&1),bool(value.flags&2),bool(value.flags&4)};}
struct State {
    std::mutex mutex;
    uint64_t handle{},generation{},sample_time{};
    ControlsProfile profile;
    InputCaptureState capture;
    std::optional<ControllerAxes> axes;
    std::optional<PhysicalInputSample> sample;
    std::optional<HostInputFrame> frame;
    std::vector<PhysicalInputEvent> diagnostics;
    bool mouse_motion_seen{},last_neutral=true;
    State(){check(mag_input_create(1,nullptr,0,&handle));sync_capture();}
    ~State(){mag_input_destroy(handle);}
    void sync_capture(){const auto previous=generation;check(mag_input_capture(handle,uint32_t(capture.focused)|(uint32_t(capture.supported_context)<<1)|(uint32_t(capture.mouse_captured)<<2)|(uint32_t(capture.controller_connected)<<3),&generation));if(previous!=generation)frame.reset();}
    InputPolicy read_policy(){MagInputPolicy result{};check(mag_input_policy_read(handle,&result));return policy(result);}
    void reset(uint32_t source){check(mag_input_reset(handle,source,&generation));if(source==3){frame.reset();sample.reset();diagnostics.clear();}else std::erase_if(diagnostics,[source](auto event){return uint32_t(event.control.source)==source;});if(source==1 || source==3)mouse_motion_seen=false;if(source==2 || source==3)axes.reset();}
};
State& state(){static State value;return value;}
void positive(double value) {
    if (!std::isfinite(value) || value<=0) throw std::invalid_argument("look gains must be finite and positive");
}
double mode_gain(AimMode mode, double hip, double ads, double scoped) {
    positive(hip); positive(ads); positive(scoped);
    switch(mode) {
    case AimMode::Hip: return hip;
    case AimMode::ADS: return ads;
    case AimMode::Scoped: return scoped;
    }
    throw std::invalid_argument("unknown aiming mode");
}
double angular(double value) {
    if (!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())
        throw std::invalid_argument("look displacement exceeds native angular range");
    return value;
}
void deadzone(double inner, double outer) {
    if (!std::isfinite(inner) || !std::isfinite(outer) || inner<0 || outer<0 || inner+outer>=1)
        throw std::invalid_argument("invalid controller deadzone");
}
double linear_axis(double value, double inner, double outer) {
    return std::copysign(std::clamp((std::abs(value)-inner)/(1-inner-outer),0.0,1.0),value);
}
}
InputPolicy mag_controls_update_policy(const InputPolicy& previous,const PolicyEvents& event){
    MagInputPolicy result{};const bool active=event.focused&&event.gameplay;
    check(mag_input_policy_reduce(policy_record(previous),event.requested_mode?uint32_t(*event.requested_mode):UINT32_MAX,
        uint32_t(active)|(uint32_t(event.controller_connected)<<1)|(uint32_t(event.stick_neutral)<<2)|(uint32_t(event.fresh_stick_deflection)<<3)|(uint32_t(event.gameplay_kbm)<<4)|(uint32_t(event.mouse_motion&&event.mouse_captured)<<5),&result));return policy(result);
}
bool mag_controls_assist_allowed(const InputPolicy& value){return value.mode==InputMode::Controller || (value.mode==InputMode::Auto&&!value.kbm_used_this_process);}
bool mag_controls_gameplay_source_allowed(const InputPolicy& policy,InputSource source){if(uint32_t(source)>2)return false;return policy.mode==InputMode::Auto || (policy.mode==InputMode::Controller?source==InputSource::Controller:policy.mode==InputMode::KBM&&source!=InputSource::Controller);}
MouseLookDelta mag_controls_mouse_look(double dx, double dy, const MouseLookSettings& settings, AimMode mode) {
    positive(settings.radians_per_count); positive(settings.x_gain); positive(settings.y_gain);
    const double gain=mode_gain(mode,settings.hip_gain,settings.ads_gain,settings.scoped_gain);
    return {angular(double(dx)*settings.radians_per_count*settings.x_gain*gain),
            angular(double(dy)*settings.radians_per_count*settings.y_gain*gain*(settings.invert_y ? 1 : -1))};
}
ControllerLookRequest mag_controls_controller_look(double x, double y, const ControllerLookSettings& settings, AimMode mode) {
    if (!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("invalid controller sample");
    positive(settings.x_gain); positive(settings.y_gain);
    deadzone(settings.inner_x,settings.outer_x); deadzone(settings.inner_y,settings.outer_y);
    const double gain=mode_gain(mode,settings.hip_gain,settings.ads_gain,settings.scoped_gain);
    x=std::clamp(x,-1.0,1.0)*(settings.invert_x ? -1 : 1);
    y=std::clamp(y,-1.0,1.0)*(settings.invert_y ? -1 : 1);
    const bool native=settings.response==ControllerResponse::Native;
    if (!native && settings.response!=ControllerResponse::Linear) throw std::invalid_argument("unknown controller response");
    return {native ? x : linear_axis(x,settings.inner_x,settings.outer_x),
            native ? y : linear_axis(y,settings.inner_y,settings.outer_y),
            angular(settings.x_gain*gain),angular(settings.y_gain*gain),native};
}
void mag_controls_validate_profile(const ControlsProfile& profile) {
    for(auto mode:{AimMode::Hip,AimMode::ADS,AimMode::Scoped}) {
        (void)mag_controls_mouse_look(1,1,profile.mouse,mode);
        (void)mag_controls_controller_look(0,0,profile.controller,mode);
    }
    InputPolicy mode; PolicyEvents event; event.requested_mode=profile.mode;
    (void)mag_controls_update_policy(mode,event);
    if(profile.bindings.size()>1024) throw std::invalid_argument("too many bindings");
    std::map<std::tuple<ActionId,bool,uint32_t>,unsigned> binding_counts;
    for(size_t i=0;i<profile.bindings.size();++i) {
        const auto& b=profile.bindings[i];
        if(!b.action || !b.contexts || (b.contexts & ~uint32_t(31)) ||
           (b.behavior!=BindingBehavior::Hold && b.behavior!=BindingBehavior::Toggle) ||
           !mag_controls_gameplay_source_allowed(InputPolicy{},b.control.source)) throw std::invalid_argument("invalid binding");
        for(uint32_t context=1;context<=16;context<<=1) if(b.contexts & context)
            if(++binding_counts[{b.action,b.control.source==InputSource::Controller,context}]>2)
                throw std::invalid_argument("only primary and secondary bindings are supported per input profile and context");
        for(size_t j=0;j<i;++j) {
            const auto& a=profile.bindings[j];
            if(a.control==b.control && (a.contexts & b.contexts)) throw std::invalid_argument("overlapping physical bindings");
        }
    }
}
void mag_controls_set_shared_native_actions(std::span<const ActionId> ids){auto& s=state();std::lock_guard lock(s.mutex);check(mag_input_shared_native_actions(s.handle,ids.data(),uint32_t(ids.size())));}
void mag_controls_configure(const ControlsProfile& profile){
    mag_controls_validate_profile(profile);auto& s=state();std::lock_guard lock(s.mutex);
    std::vector<MagInputBinding> bindings;
    InputPolicy permission;permission.mode=profile.mode;
    for(auto b:profile.bindings)if((b.control.source!=InputSource::Controller || profile.controller_remap) && mag_controls_gameplay_source_allowed(permission,b.control.source))bindings.push_back({uint32_t(b.control.source),b.control.code,b.action,b.contexts,uint32_t(b.behavior)});
    check(mag_input_configure(s.handle,bindings.data(),uint32_t(bindings.size())));
    s.profile=profile;s.frame.reset();s.sample.reset();s.diagnostics.clear();s.sync_capture();
    MagInputPolicy ignored{};check(mag_input_policy(s.handle,uint32_t(profile.mode),uint32_t(s.capture.controller_connected)<<1|4,&ignored));
}
void mag_controls_set_capture_state(const InputCaptureState& capture){auto& s=state();std::lock_guard lock(s.mutex);if(capture.time_ns<s.capture.time_ns)throw std::invalid_argument("input time moved backwards");s.capture=capture;s.sync_capture();}
void mag_controls_set_window_capture(bool focused,bool mouse){auto& s=state();std::lock_guard lock(s.mutex);s.capture.focused=focused;s.capture.mouse_captured=mouse;s.sync_capture();}
void mag_controls_set_supported_context(bool supported){auto& s=state();std::lock_guard lock(s.mutex);s.capture.supported_context=supported;s.sync_capture();}
void mag_controls_set_controller_connection(bool connected){auto& s=state();std::lock_guard lock(s.mutex);s.capture.controller_connected=connected;s.sync_capture();if(!connected)s.axes.reset();}
void mag_controls_push_event(const PhysicalInputEvent& event){auto& s=state();std::lock_guard lock(s.mutex);check(mag_input_event(s.handle,event.device,uint32_t(event.control.source),event.control.code,uint32_t(event.down),event.time_ns,s.generation));if(s.diagnostics.size()>=8192){s.reset(3);throw std::runtime_error("physical diagnostic queue capacity exceeded");}s.diagnostics.push_back(event);}
void mag_controls_push_relative_mouse(double dx,double dy){auto& s=state();std::lock_guard lock(s.mutex);check(mag_input_mouse(s.handle,s.generation,dx,dy));if(s.capture.focused&&s.capture.supported_context&&s.capture.mouse_captured)s.mouse_motion_seen|=dx!=0||dy!=0;}
void mag_controls_push_mouse(int64_t dx,int64_t dy){mag_controls_push_relative_mouse(double(dx),double(dy));}
namespace {
ControllerAxes normalize_axes(ControllerAxes axes){
    for(double* value:{&axes.move_x,&axes.move_y,&axes.look_x,&axes.look_y,&axes.left_trigger,&axes.right_trigger})if(!std::isfinite(*value))throw std::invalid_argument("invalid controller axis");
    axes.move_x=std::clamp(axes.move_x,-1.0,1.0);axes.move_y=std::clamp(axes.move_y,-1.0,1.0);axes.look_x=std::clamp(axes.look_x,-1.0,1.0);axes.look_y=std::clamp(axes.look_y,-1.0,1.0);axes.left_trigger=std::clamp(axes.left_trigger,0.0,1.0);axes.right_trigger=std::clamp(axes.right_trigger,0.0,1.0);return axes;
}
const PhysicalInputSample& seal(State& s,InputFrameKey k){
    if(!k.process_session||!k.sample_sequence||k.player_slot)throw std::invalid_argument("invalid input identity");
    if(s.sample&&s.sample->key==k)return *s.sample;
    check(mag_input_sample(s.handle,key(k)));s.frame.reset();s.sample_time=now();
    PhysicalInputSample sample;sample.key=k;sample.capture_generation=s.generation;sample.capture=s.capture;sample.controller_axes=s.axes;sample.mouse_motion_seen=s.mouse_motion_seen;sample.events=std::move(s.diagnostics);s.diagnostics.clear();s.mouse_motion_seen=false;s.sample=std::move(sample);return *s.sample;
}
const HostInputFrame& attach(State& s,InputFrameKey k,const InputInterpretation& interpretation){
    if(s.frame&&s.frame->key==k){if(s.frame->context!=interpretation.context)throw std::invalid_argument("context already frozen");return *s.frame;}
    if(!s.sample||s.sample->key!=k)throw std::invalid_argument("physical sample not sealed");
    MagInputFrame raw{};check(mag_input_seal(s.handle,key(k),s.sample_time,k.process_session,uint32_t(interpretation.context),&raw));s.generation=raw.generation;
    const bool neutral=interpretation.controller_neutral.value_or(s.sample->capture.stick_neutral);
    const bool fresh=s.last_neutral&&!neutral;s.last_neutral=neutral;
    const bool gameplay=interpretation.context!=InputContext::Menu && interpretation.context!=InputContext::Settings && interpretation.context!=InputContext::Rebinding;
    MagInputPolicy resolved{};check(mag_input_policy(s.handle,uint32_t(s.profile.mode),uint32_t(gameplay&&s.capture.focused&&s.capture.supported_context)|(uint32_t(s.capture.controller_connected)<<1)|(uint32_t(neutral)<<2)|(uint32_t(fresh)<<3),&resolved));
    HostInputFrame result;result.key=k;result.context=interpretation.context;result.capture_generation=raw.generation;result.policy=policy(resolved);
    if(gameplay && result.policy.mode!=InputMode::Controller){result.mouse_dx=raw.dx;result.mouse_dy=raw.dy;if(raw.dx!=0||raw.dy!=0)result.policy.look_owner=LookOwner::Mouse;}
    if(gameplay && result.policy.mode!=InputMode::KBM && result.policy.stick_rearmed)result.controller_axes=s.sample->controller_axes;
    std::set<ActionId> actions;for(auto binding:s.profile.bindings)actions.insert(binding.action);
    for(auto action:actions){MagInputOffer value{};const int status=mag_input_offer(s.handle,key(k),action,&value);if(status!=3)check(status);result.values.push_back({action,status==0&&value.down?1.0:0.0});}
    s.frame=std::move(result);return *s.frame;
}
}
void mag_controls_clear_controller_sample(){auto& s=state();std::lock_guard lock(s.mutex);s.axes.reset();}
void mag_controls_capture_controller_axes(const ControllerAxes& axes){auto value=normalize_axes(axes);auto& s=state();std::lock_guard lock(s.mutex);if(s.capture.focused&&s.capture.supported_context&&s.capture.controller_connected)s.axes=value;}
void mag_controls_push_controller_axes(const ControllerAxes& axes,bool neutral){auto value=normalize_axes(axes);auto& s=state();std::lock_guard lock(s.mutex);s.capture.stick_neutral=neutral;if(s.capture.focused&&s.capture.supported_context&&s.capture.controller_connected)s.axes=value;}
const PhysicalInputSample& mag_controls_seal_sample(InputFrameKey k){auto& s=state();std::lock_guard lock(s.mutex);return seal(s,k);}
const HostInputFrame& mag_controls_attach_sample(InputFrameKey k,InputContext context){auto& s=state();std::lock_guard lock(s.mutex);return attach(s,k,{context,{}});}
const HostInputFrame& mag_controls_attach_sample(InputFrameKey k,const InputInterpretation& interpretation){auto& s=state();std::lock_guard lock(s.mutex);return attach(s,k,interpretation);}
const HostInputFrame& mag_controls_acquire_frame(InputFrameKey k,InputContext context){auto& s=state();std::lock_guard lock(s.mutex);seal(s,k);return attach(s,k,{context,{}});}
ActionFrameView mag_controls_actions(const HostInputFrame& frame,InputContext context,ConsumptionLedger& ledger){auto& s=state();std::lock_guard lock(s.mutex);(void)ledger;if(!s.frame||s.frame->key!=frame.key||frame.context!=context||frame.capture_generation!=s.generation)return {};return {frame.values,{}};}
void mag_controls_retire_frame(InputFrameKey k){auto& s=state();std::lock_guard lock(s.mutex);if(s.sample&&s.sample->key==k){check(mag_input_retire(s.handle,key(k)));s.frame.reset();s.sample.reset();s.sync_capture();}}
void mag_controls_release_input(InputResetReason reason){auto& s=state();std::lock_guard lock(s.mutex);s.reset(reason==InputResetReason::Device?2:3);}
void mag_controls_release_source(InputSource source){if(uint32_t(source)>2)throw std::invalid_argument("invalid source");auto& s=state();std::lock_guard lock(s.mutex);s.reset(uint32_t(source));}
bool mag_controls_frame_valid(InputFrameKey k,uint64_t generation){auto& s=state();std::lock_guard lock(s.mutex);if(!s.frame||s.frame->key!=k||s.generation!=generation||!s.capture.focused||!s.capture.supported_context)return false;MagInputOffer ignored{};return mag_input_offer(s.handle,key(k),0,&ignored)==3;}
uint64_t mag_controls_current_generation(){auto& s=state();std::lock_guard lock(s.mutex);return s.generation;}
InputPolicy mag_controls_current_policy(){auto& s=state();std::lock_guard lock(s.mutex);return s.read_policy();}
std::optional<ControlOffer> mag_controls_offer(InputFrameKey k,ActionId action){auto& s=state();std::lock_guard lock(s.mutex);if(!s.frame||s.frame->key!=k)return {};MagInputOffer value{};const int status=mag_input_offer(s.handle,key(k),action,&value);if(status==3)return {};check(status);return ControlOffer{value.pulse_id,value.duration_ns,bool(value.down),value.source_mask};}
ControlRecognition mag_controls_recognize(InputFrameKey k,uint64_t consumer,uint64_t control,ActionId action,double value,bool blocked,const ControlRecognitionConfig& config){auto& s=state();std::lock_guard lock(s.mutex);if(!s.frame||s.frame->key!=k)throw std::invalid_argument("recognition requires attached frame");MagInputRecognition result{};check(mag_input_recognize(s.handle,key(k),consumer,control,action,value,uint32_t(blocked),{config.press_delay_ns,config.double_tap_gap_ns,config.repeat_delay_ns,config.repeat_interval_ns,config.refocus_delay_ns,config.down_threshold,config.change_epsilon},&result));return {result.value,result.elapsed_ns,ControlPhase(result.phase),result.events};}
void mag_controls_observe(InputFrameKey k,ActionId action,const ControlOffer& value){auto& s=state();std::lock_guard lock(s.mutex);if(!s.frame||s.frame->key!=k)throw std::invalid_argument("observation requires attached frame");check(mag_input_observe(s.handle,key(k),action,offer(value)));}
void mag_controls_accept_action(InputFrameKey k,ActionId action,const ControlOffer& value){auto& s=state();std::lock_guard lock(s.mutex);if(!s.frame||s.frame->key!=k)throw std::invalid_argument("acceptance requires attached frame");check(mag_input_accept_action(s.handle,key(k),action,offer(value)));}
void mag_controls_accept_mouse(InputFrameKey k){auto& s=state();std::lock_guard lock(s.mutex);if(!s.frame||s.frame->key!=k)throw std::invalid_argument("acceptance requires attached frame");check(mag_input_accept_mouse(s.handle,key(k),uint32_t(s.last_neutral)));}
void mag_controls_forget_consumer(uint64_t consumer){auto& s=state();std::lock_guard lock(s.mutex);check(mag_input_forget_consumer(s.handle,consumer));}
} // namespace mag
