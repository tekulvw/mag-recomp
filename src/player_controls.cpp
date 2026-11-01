#include "player_controls.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
namespace mag {
namespace {
struct ActiveAction { bool down=false; InputSource source=InputSource::Keyboard; unsigned sources=0; };
struct QueuedInput { PhysicalInputEvent event; std::optional<InputSource> revoke_source; };
struct ControlsState {
    std::mutex mutex;
    ControlsProfile profile;
    InputCaptureState capture;
    InputPolicy policy;
    uint64_t generation=1, next_token=1;
    int64_t mouse_dx=0, mouse_dy=0;
    bool mouse_motion_seen=false, controller_axes_rearmed=false;
    std::optional<ControllerAxes> controller_axes;
    std::set<PhysicalControl> physical_down, accepted_down, toggled, blocked;
    std::vector<QueuedInput> queue;
    std::map<ActionId,ActiveAction> actions;
    std::vector<ActionEvent> releases;
    std::optional<InputContext> context;
    std::optional<PhysicalInputSample> sample;
    std::vector<QueuedInput> sampled_queue;
    bool sample_invalidated=false;
    unsigned sample_revoked_sources=0;
    std::optional<HostInputFrame> frame;
    std::map<std::pair<uint64_t,uint32_t>,uint64_t> retired;
};
ControlsState& controls() { static ControlsState state; return state; }
bool gameplay_context(InputContext context) {
    switch(context) {
    case InputContext::OnFoot: case InputContext::Vehicle: case InputContext::Turret: case InputContext::Tactical: return true;
    case InputContext::Menu: case InputContext::Settings: case InputContext::Rebinding: return false;
    }
    throw std::invalid_argument("unknown input context");
}
bool enabled(const ControlsState& state) { return state.capture.focused && state.capture.supported_context; }
void release_locked(ControlsState& state, std::optional<InputSource> source={}) {
    for(auto control:state.physical_down) if(!source || control.source==*source) state.blocked.insert(control);
    if(source) {
        // Keep the revocation after already queued events from other sources.
        // Applying it early could manufacture release/press edges for an action
        // that those sources kept continuously held.
        std::erase_if(state.queue,[&](const auto& e){return !e.revoke_source && e.event.control.source==*source;});
        if(state.queue.size()>=8192) { release_locked(state); return; }
        state.queue.push_back({{},source});
        if(state.sample && !state.frame && !state.sample_invalidated) {
            // The sealed raw sample stays immutable. Its semantic projection
            // loses only the revoked source, after other sealed input events.
            if(state.sampled_queue.size()>=8192) { release_locked(state); return; }
            std::erase_if(state.sampled_queue,[&](const auto& e){return !e.revoke_source && e.event.control.source==*source;});
            state.sampled_queue.push_back({{},source});
            state.sample_revoked_sources|=1u<<unsigned(*source);
        }
    } else {
        state.sample_invalidated=true;
        state.sampled_queue.clear();
        for (auto& [id,action]:state.actions) if(action.down) {
            state.releases.push_back({id,ActionEventKind::Release,state.next_token++,action.source,true});
            action.down=false; action.sources=0;
        }
        state.accepted_down.clear(); state.toggled.clear(); state.queue.clear();
    }
    if(!source || *source==InputSource::Mouse) { state.mouse_dx=0; state.mouse_dy=0; state.mouse_motion_seen=false; }
    if(!source || *source==InputSource::Controller) { state.controller_axes.reset(); state.controller_axes_rearmed=false; }
    ++state.generation;
}
void retire_locked(ControlsState& state) {
    if(!state.sample) return;
    const auto key=state.sample->key;
    // No semantic attachment means this poll was rejected/unsupported. Cancel
    // old held actions and require fresh activation rather than replaying any
    // of its physical edges or preserving a missed release as a stuck action.
    if(!state.frame) release_locked(state);
    auto& high=state.retired[{key.process_session,key.player_slot}];
    high=std::max(high,key.sample_sequence);
    state.frame.reset(); state.sample.reset(); state.sampled_queue.clear();
    state.sample_invalidated=false; state.sample_revoked_sources=0;
}
unsigned bound_sources(const ControlsState& state, ActionId id, InputContext context, const InputPolicy& policy, bool gameplay) {
    unsigned sources=0;
    for(const auto& binding:state.profile.bindings)
        if(binding.action==id && (binding.contexts & uint32_t(context)) &&
           (binding.behavior==BindingBehavior::Toggle ? state.toggled.contains(binding.control) : state.accepted_down.contains(binding.control)) &&
           (!gameplay || mag_controls_gameplay_source_allowed(policy,binding.control.source))) sources|=1u<<unsigned(binding.control.source);
    return sources;
}
void update_actions(ControlsState& state, HostInputFrame& frame, InputSource source, bool gameplay, bool cleanup=false) {
    for(auto& [id,action]:state.actions) {
        const unsigned sources=bound_sources(state,id,frame.context,frame.policy,gameplay);
        const bool down=sources!=0;
        if(down!=action.down) {
            if(cleanup && down) continue; // Revocation can never grant an action.
            frame.events.push_back({id,down ? ActionEventKind::Press : ActionEventKind::Release,state.next_token++,source,cleanup});
            action.down=down; action.source=source;
        }
        action.sources=sources;
    }
}
int64_t add_counts(int64_t a, int64_t b) {
    if((b>0 && a>std::numeric_limits<int64_t>::max()-b) || (b<0 && a<std::numeric_limits<int64_t>::min()-b))
        throw std::invalid_argument("mouse count overflow");
    return a+b;
}
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
InputPolicy mag_controls_update_policy(const InputPolicy& previous, const PolicyEvents& events) {
    InputPolicy next=previous;
    if (events.requested_mode) next.mode=*events.requested_mode;
    next.controller_connected=events.controller_connected;
    const bool active=events.focused && events.gameplay;
    const bool mouse=active && events.mouse_captured && events.mouse_motion;
    switch(next.mode) {
    case InputMode::KBM:
        next.kbm_used_this_process=true;
        next.look_owner=LookOwner::Mouse;
        next.stick_rearmed=events.stick_neutral;
        break;
    case InputMode::Controller:
        next.look_owner=LookOwner::Controller;
        next.stick_rearmed=events.stick_neutral;
        break;
    case InputMode::Auto:
        if (active && (events.gameplay_kbm || mouse)) next.kbm_used_this_process=true;
        if (events.stick_neutral) next.stick_rearmed=true;
        if (mouse) {
            next.look_owner=LookOwner::Mouse;
            next.stick_rearmed=events.stick_neutral;
        } else if (active && events.controller_connected && events.fresh_stick_deflection && next.stick_rearmed) {
            next.look_owner=LookOwner::Controller;
            next.stick_rearmed=false;
        } else if (!events.controller_connected) {
            next.look_owner=LookOwner::Mouse;
        }
        break;
    default: throw std::invalid_argument("unknown input mode");
    }
    return next;
}
bool mag_controls_assist_allowed(const InputPolicy& policy) {
    return policy.mode==InputMode::Controller ||
        (policy.mode==InputMode::Auto && !policy.kbm_used_this_process);
}
bool mag_controls_gameplay_source_allowed(const InputPolicy& policy, InputSource source) {
    if (source!=InputSource::Keyboard && source!=InputSource::Mouse && source!=InputSource::Controller) return false;
    switch(policy.mode) {
    case InputMode::Auto: return true;
    case InputMode::Controller: return source==InputSource::Controller;
    case InputMode::KBM: return source!=InputSource::Controller;
    }
    return false;
}
MouseLookDelta mag_controls_mouse_look(int64_t dx, int64_t dy, const MouseLookSettings& settings, AimMode mode) {
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
void mag_controls_configure(const ControlsProfile& profile) {
    mag_controls_validate_profile(profile);
    auto& state=controls(); std::lock_guard lock(state.mutex);
    release_locked(state);
    state.profile=profile;
    for(const auto& b:profile.bindings) state.actions.try_emplace(b.action);
    if(profile.mode!=InputMode::Controller) {
        // Revocation is immediate even when the next sample cannot attach.
        // Granting the explicit Controller exception still waits for attachment.
        PolicyEvents event; event.requested_mode=profile.mode;
        event.focused=false; event.gameplay=false;
        event.controller_connected=state.capture.controller_connected;
        event.stick_neutral=state.capture.stick_neutral;
        state.policy=mag_controls_update_policy(state.policy,event);
    }
}
void mag_controls_set_capture_state(const InputCaptureState& capture) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(capture.time_ns<state.capture.time_ns) throw std::invalid_argument("input time moved backwards");
    if((state.capture.focused && !capture.focused) || (state.capture.supported_context && !capture.supported_context)) release_locked(state);
    else {
        if(state.capture.mouse_captured && !capture.mouse_captured) release_locked(state,InputSource::Mouse);
        if(state.capture.controller_connected && !capture.controller_connected) release_locked(state,InputSource::Controller);
    }
    state.capture=capture;
}
void mag_controls_push_event(const PhysicalInputEvent& event) {
    if(!mag_controls_gameplay_source_allowed(InputPolicy{},event.control.source)) throw std::invalid_argument("invalid physical source");
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(event.down) state.physical_down.insert(event.control); else state.physical_down.erase(event.control);
    if(state.blocked.contains(event.control)) {
        if(!event.down) state.blocked.erase(event.control);
        return;
    }
    if(event.control.source==InputSource::Controller && !state.capture.controller_connected) {
        if(event.down) state.blocked.insert(event.control);
        return;
    }
    if(!enabled(state)) { if(event.down) state.blocked.insert(event.control); return; }
    if(state.queue.size()>=8192) { release_locked(state); return; }
    state.queue.push_back({event,{}});
}
void mag_controls_push_mouse(int64_t dx,int64_t dy) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(!enabled(state) || !state.capture.mouse_captured) return;
    state.mouse_motion_seen |= dx!=0 || dy!=0;
    try { state.mouse_dx=add_counts(state.mouse_dx,dx); state.mouse_dy=add_counts(state.mouse_dy,dy); }
    catch(...) { release_locked(state); throw; }
}
void mag_controls_push_controller_axes(const ControllerAxes& input, bool neutral) {
    ControllerAxes axes=input;
    for(double* value:{&axes.move_x,&axes.move_y,&axes.look_x,&axes.look_y,&axes.left_trigger,&axes.right_trigger})
        if(!std::isfinite(*value)) throw std::invalid_argument("invalid controller axis");
    axes.move_x=std::clamp(axes.move_x,-1.0,1.0); axes.move_y=std::clamp(axes.move_y,-1.0,1.0);
    axes.look_x=std::clamp(axes.look_x,-1.0,1.0); axes.look_y=std::clamp(axes.look_y,-1.0,1.0);
    axes.left_trigger=std::clamp(axes.left_trigger,0.0,1.0); axes.right_trigger=std::clamp(axes.right_trigger,0.0,1.0);
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(!enabled(state) || !state.capture.controller_connected) return;
    if(neutral) state.controller_axes_rearmed=true;
    if(state.controller_axes_rearmed) state.controller_axes=axes;
}
namespace {
const PhysicalInputSample& seal_locked(ControlsState& state,InputFrameKey key) {
    if(key.player_slot!=0) throw std::invalid_argument("only local player slot zero is supported");
    if(!key.process_session || !key.sample_sequence) throw std::invalid_argument("input key identity required");
    if(state.sample && state.sample->key==key) return *state.sample;
    const auto owner=std::make_pair(key.process_session,key.player_slot);
    if(key.sample_sequence<=state.retired[owner] ||
       (state.sample && state.sample->key.process_session==key.process_session &&
        state.sample->key.player_slot==key.player_slot && key.sample_sequence<state.sample->key.sample_sequence))
        throw std::invalid_argument("retired input sample key");
    retire_locked(state);
    PhysicalInputSample sample; sample.key=key; sample.capture_generation=state.generation;
    sample.capture=state.capture;
    sample.mouse_dx=state.mouse_dx; sample.mouse_dy=state.mouse_dy;
    sample.mouse_motion_seen=state.mouse_motion_seen; sample.controller_axes=state.controller_axes;
    state.mouse_dx=0; state.mouse_dy=0; state.mouse_motion_seen=false;
    state.capture.fresh_stick_deflection=false;
    state.sampled_queue=std::move(state.queue); state.queue.clear();
    state.sample_invalidated=false; state.sample_revoked_sources=0;
    for(const auto& queued:state.sampled_queue) if(!queued.revoke_source) sample.events.push_back(queued.event);
    state.sample=std::move(sample);
    return *state.sample;
}
const HostInputFrame& attach_locked(ControlsState& state,InputFrameKey key,InputContext context) {
    const bool gameplay=gameplay_context(context);
    if(state.frame && state.frame->key==key) {
        if(state.frame->context!=context) throw std::invalid_argument("input context already frozen");
        return *state.frame;
    }
    if(!state.sample || state.sample->key!=key) throw std::invalid_argument("input sample must be sealed before attachment");
    const auto& sample=*state.sample;
    if(state.sample_invalidated) throw std::invalid_argument("input sample invalidated before attachment");
    const bool mouse_revoked=state.sample_revoked_sources & (1u<<unsigned(InputSource::Mouse));
    const bool controller_revoked=state.sample_revoked_sources & (1u<<unsigned(InputSource::Controller));
    const bool context_changed=state.context && *state.context!=context;
    if(context_changed) { release_locked(state); state.sampled_queue.clear(); }
    state.context=context;
    HostInputFrame frame; frame.key=key; frame.context=context; frame.capture_generation=state.generation;
    frame.policy=state.policy; frame.policy.mode=state.profile.mode;
    frame.events=std::move(state.releases); state.releases.clear();
    bool kbm=false;
    const bool accepting=enabled(state) && sample.capture.focused && sample.capture.supported_context &&
        context!=InputContext::Settings && context!=InputContext::Rebinding;
    if(accepting) for(const auto& queued:state.sampled_queue) {
        if(queued.revoke_source) {
            std::erase_if(state.accepted_down,[&](auto c){return c.source==*queued.revoke_source;});
            std::erase_if(state.toggled,[&](auto c){return c.source==*queued.revoke_source;});
            update_actions(state,frame,*queued.revoke_source,gameplay,true);
            continue;
        }
        const auto& event=queued.event;
        if(gameplay && !mag_controls_gameplay_source_allowed(frame.policy,event.control.source)) continue;
        const bool fresh=event.down && !state.accepted_down.contains(event.control);
        if(event.down) state.accepted_down.insert(event.control); else state.accepted_down.erase(event.control);
        bool matching=false;
        for(const auto& b:state.profile.bindings) if(b.control==event.control && (b.contexts & uint32_t(context))) {
            matching=true;
            if(fresh && b.behavior==BindingBehavior::Toggle) {
                if(!state.toggled.erase(event.control)) state.toggled.insert(event.control);
            }
        }
        if(gameplay && matching && event.down && event.control.source!=InputSource::Controller) kbm=true;
        update_actions(state,frame,event.control.source,gameplay);
    }
    state.sampled_queue.clear();
    if(accepting && gameplay && !context_changed && !mouse_revoked && mag_controls_gameplay_source_allowed(frame.policy,InputSource::Mouse)) {
        frame.mouse_dx=sample.mouse_dx; frame.mouse_dy=sample.mouse_dy;
    }
    PolicyEvents events; events.requested_mode=state.profile.mode; events.focused=accepting; events.gameplay=gameplay;
    events.controller_connected=sample.capture.controller_connected && !controller_revoked; events.gameplay_kbm=kbm;
    events.mouse_motion=accepting && gameplay && !context_changed && !mouse_revoked && sample.mouse_motion_seen;
    events.mouse_captured=sample.capture.mouse_captured;
    events.stick_neutral=controller_revoked || sample.capture.stick_neutral;
    events.fresh_stick_deflection=!controller_revoked && sample.capture.fresh_stick_deflection;
    state.policy=mag_controls_update_policy(state.policy,events); frame.policy=state.policy;
    if(accepting && gameplay && !context_changed && !controller_revoked && mag_controls_gameplay_source_allowed(frame.policy,InputSource::Controller)) frame.controller_axes=sample.controller_axes;
    for(const auto& [id,action]:state.actions) frame.values.push_back({id,action.down ? 1.0 : 0.0});
    state.frame=std::move(frame);
    return *state.frame;
}
}
const PhysicalInputSample& mag_controls_seal_sample(InputFrameKey key) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    return seal_locked(state,key);
}
const HostInputFrame& mag_controls_attach_sample(InputFrameKey key,InputContext context) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    return attach_locked(state,key,context);
}
const HostInputFrame& mag_controls_acquire_frame(InputFrameKey key,InputContext context) {
    (void)gameplay_context(context);
    auto& state=controls(); std::lock_guard lock(state.mutex);
    (void)seal_locked(state,key);
    return attach_locked(state,key,context);
}
ActionFrameView mag_controls_actions(const HostInputFrame& frame,InputContext context,ConsumptionLedger& ledger) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(frame.context!=context || !state.frame || state.frame->key!=frame.key || state.generation!=frame.capture_generation) return {};
    const bool accepting=enabled(state) && context!=InputContext::Settings && context!=InputContext::Rebinding;
    ActionFrameView view;
    if(accepting) view.values=frame.values;
    for(const auto& event:frame.events)
        if((accepting || event.cleanup) && !ledger.consumed.contains(event.token)) view.events.push_back(event);
    return view;
}
void mag_controls_retire_frame(InputFrameKey key) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(state.sample && state.sample->key==key) retire_locked(state);
}
void mag_controls_release_input(InputResetReason reason) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(reason==InputResetReason::Device) release_locked(state,InputSource::Controller); else release_locked(state);
}
bool mag_controls_frame_valid(InputFrameKey key,uint64_t generation) {
    auto& state=controls(); std::lock_guard lock(state.mutex);
    return state.frame && state.frame->key==key && state.generation==generation && enabled(state);
}
InputPolicy mag_controls_current_policy() {
    auto& state=controls(); std::lock_guard lock(state.mutex); return state.policy;
}
} // namespace mag
