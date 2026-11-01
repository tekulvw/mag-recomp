#include "player_controls.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
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
    std::set<PhysicalControl> physical_down, accepted_down, blocked;
    std::vector<QueuedInput> queue;
    std::map<ActionId,ActiveAction> actions;
    std::vector<ActionEvent> releases;
    std::optional<InputContext> context;
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
    } else {
        for (auto& [id,action]:state.actions) if(action.down) {
            state.releases.push_back({id,ActionEventKind::Release,state.next_token++,action.source,true});
            action.down=false; action.sources=0;
        }
        state.accepted_down.clear(); state.queue.clear();
    }
    if(!source || *source==InputSource::Mouse) { state.mouse_dx=0; state.mouse_dy=0; state.mouse_motion_seen=false; }
    if(!source || *source==InputSource::Controller) { state.controller_axes.reset(); state.controller_axes_rearmed=false; }
    ++state.generation;
}
void retire_locked(ControlsState& state) {
    if(!state.frame) return;
    const auto key=state.frame->key;
    auto& high=state.retired[{key.process_session,key.player_slot}];
    high=std::max(high,key.simulation_sequence);
    state.frame.reset();
}
unsigned bound_sources(const ControlsState& state, ActionId id, InputContext context, const InputPolicy& policy, bool gameplay) {
    unsigned sources=0;
    for(const auto& binding:state.profile.bindings)
        if(binding.action==id && (binding.contexts & uint32_t(context)) && state.accepted_down.contains(binding.control) &&
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
void mag_controls_configure(const ControlsProfile& profile) {
    (void)mag_controls_mouse_look(0,0,profile.mouse,AimMode::Hip);
    (void)mag_controls_controller_look(0,0,profile.controller,AimMode::Hip);
    InputPolicy mode; PolicyEvents event; event.requested_mode=profile.mode;
    (void)mag_controls_update_policy(mode,event);
    if(profile.bindings.size()>1024) throw std::invalid_argument("too many bindings");
    for(size_t i=0;i<profile.bindings.size();++i) {
        const auto& b=profile.bindings[i];
        if(!b.action || !b.contexts || (b.contexts & ~uint32_t(31)) ||
           !mag_controls_gameplay_source_allowed(InputPolicy{},b.control.source)) throw std::invalid_argument("invalid binding");
        for(size_t j=0;j<i;++j) {
            const auto& a=profile.bindings[j];
            if(a.control==b.control && (a.contexts & b.contexts)) throw std::invalid_argument("overlapping physical bindings");
        }
    }
    auto& state=controls(); std::lock_guard lock(state.mutex);
    release_locked(state);
    state.profile=profile;
    for(const auto& b:profile.bindings) state.actions.try_emplace(b.action);
    if(profile.mode==InputMode::KBM) {
        PolicyEvents deny; deny.requested_mode=InputMode::KBM;
        state.policy=mag_controls_update_policy(state.policy,deny);
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
const HostInputFrame& mag_controls_acquire_frame(InputFrameKey key,InputContext context) {
    if(key.player_slot!=0) throw std::invalid_argument("only local player slot zero is supported");
    const bool gameplay=gameplay_context(context);
    auto& state=controls(); std::lock_guard lock(state.mutex);
    if(!key.process_session || !key.simulation_sequence) throw std::invalid_argument("input key identity required");
    if(state.frame && state.frame->key==key) {
        if(state.frame->context!=context) throw std::invalid_argument("input context already frozen");
        return *state.frame;
    }
    const auto owner=std::make_pair(key.process_session,key.player_slot);
    if(key.simulation_sequence<=state.retired[owner] ||
       (state.frame && state.frame->key.process_session==key.process_session && state.frame->key.player_slot==key.player_slot && key.simulation_sequence<state.frame->key.simulation_sequence))
        throw std::invalid_argument("retired input frame key");
    retire_locked(state);
    if(state.context && *state.context!=context) release_locked(state);
    state.context=context;
    HostInputFrame frame; frame.key=key; frame.context=context; frame.capture_generation=state.generation;
    frame.policy=state.policy; frame.policy.mode=state.profile.mode;
    frame.events=std::move(state.releases); state.releases.clear();
    bool kbm=false;
    const bool accepting=enabled(state) && context!=InputContext::Settings && context!=InputContext::Rebinding;
    if(accepting) for(const auto& queued:state.queue) {
        if(queued.revoke_source) {
            std::erase_if(state.accepted_down,[&](auto c){return c.source==*queued.revoke_source;});
            update_actions(state,frame,*queued.revoke_source,gameplay,true);
            continue;
        }
        const auto& event=queued.event;
        if(gameplay && !mag_controls_gameplay_source_allowed(frame.policy,event.control.source)) continue;
        if(event.down) state.accepted_down.insert(event.control); else state.accepted_down.erase(event.control);
        bool matching=false;
        for(const auto& b:state.profile.bindings) if(b.control==event.control && (b.contexts & uint32_t(context))) matching=true;
        if(gameplay && matching && event.down && event.control.source!=InputSource::Controller) kbm=true;
        update_actions(state,frame,event.control.source,gameplay);
    }
    state.queue.clear();
    if(accepting && gameplay && mag_controls_gameplay_source_allowed(frame.policy,InputSource::Mouse)) {
        frame.mouse_dx=state.mouse_dx; frame.mouse_dy=state.mouse_dy;
    }
    state.mouse_dx=0; state.mouse_dy=0;
    PolicyEvents events; events.requested_mode=state.profile.mode; events.focused=accepting; events.gameplay=gameplay;
    events.controller_connected=state.capture.controller_connected; events.gameplay_kbm=kbm;
    events.mouse_motion=accepting && gameplay && state.mouse_motion_seen; state.mouse_motion_seen=false; events.mouse_captured=state.capture.mouse_captured;
    events.stick_neutral=state.capture.stick_neutral; events.fresh_stick_deflection=state.capture.fresh_stick_deflection;
    state.capture.fresh_stick_deflection=false;
    state.policy=mag_controls_update_policy(state.policy,events); frame.policy=state.policy;
    if(accepting && gameplay && mag_controls_gameplay_source_allowed(frame.policy,InputSource::Controller)) frame.controller_axes=state.controller_axes;
    for(const auto& [id,action]:state.actions) frame.values.push_back({id,action.down ? 1.0 : 0.0});
    state.frame=std::move(frame);
    return *state.frame;
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
    if(state.frame && state.frame->key==key) retire_locked(state);
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
