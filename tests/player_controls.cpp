#include "player_controls.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace mag;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static bool close(double a, double b) { return std::abs(a-b)<1e-9; }
static void policy_tests() {
    InputPolicy p;
    PolicyEvents e; e.controller_connected=true;
    p=mag_controls_update_policy(p,e);
    check(mag_controls_assist_allowed(p), "initial Auto controller permission");
    e.gameplay_kbm=true;
    p=mag_controls_update_policy(p,e);
    check(p.kbm_used_this_process && !mag_controls_assist_allowed(p), "keyboard with connected controller disables assist");
    check(p.look_owner==LookOwner::Controller, "keyboard action must not seize look");
    e.gameplay_kbm=false; e.mouse_motion=true; e.mouse_captured=true; e.stick_neutral=false;
    p=mag_controls_update_policy(p,e);
    check(p.look_owner==LookOwner::Mouse && !p.stick_rearmed, "mouse owns look while stick held");
    e.mouse_motion=false; e.fresh_stick_deflection=true;
    p=mag_controls_update_policy(p,e);
    check(p.look_owner==LookOwner::Mouse, "held stick cannot reclaim look");
    e.fresh_stick_deflection=false; e.stick_neutral=true;
    p=mag_controls_update_policy(p,e);
    e.stick_neutral=false; e.fresh_stick_deflection=true;
    p=mag_controls_update_policy(p,e);
    check(p.look_owner==LookOwner::Controller && !mag_controls_assist_allowed(p), "fresh stick can reclaim look without restoring assist");
    e.mouse_motion=true;
    p=mag_controls_update_policy(p,e);
    check(p.look_owner==LookOwner::Mouse, "same-frame mouse wins tie");
    e={}; e.controller_connected=true; e.gameplay_kbm=true; e.requested_mode=InputMode::Controller;
    p=mag_controls_update_policy(p,e);
    check(p.kbm_used_this_process && mag_controls_assist_allowed(p), "Controller exception retains provenance");
    check(!mag_controls_gameplay_source_allowed(p,InputSource::Keyboard), "Controller rejects gameplay keyboard");
    check(!mag_controls_gameplay_source_allowed(p,InputSource::Mouse), "Controller rejects gameplay mouse");
    check(mag_controls_gameplay_source_allowed(p,InputSource::Controller), "Controller accepts controller");
    e.gameplay_kbm=false; e.requested_mode=InputMode::Auto;
    p=mag_controls_update_policy(p,e);
    check(!mag_controls_assist_allowed(p), "Auto remembers prior KBM");
    e.requested_mode.reset(); e.focused=false;
    p=mag_controls_update_policy(p,e);
    check(p.kbm_used_this_process, "focus lifecycle never erases provenance");
    InputPolicy fresh;
    e={}; e.gameplay=false; e.gameplay_kbm=true; e.mouse_motion=true;
    fresh=mag_controls_update_policy(fresh,e);
    check(!fresh.kbm_used_this_process, "menu typing/motion is not gameplay KBM");
    e={}; e.mouse_motion=true; e.mouse_captured=false;
    fresh=mag_controls_update_policy(fresh,e);
    check(!fresh.kbm_used_this_process, "uncaptured motion is not gameplay KBM");
    e={}; e.requested_mode=InputMode::KBM;
    fresh=mag_controls_update_policy(fresh,e);
    check(fresh.kbm_used_this_process && !mag_controls_assist_allowed(fresh), "KBM selection disables before motion");
    check(!mag_controls_gameplay_source_allowed(fresh,InputSource::Controller), "KBM rejects gameplay controller");
}
static void mouse_tests() {
    MouseLookSettings s; s.radians_per_count=.001; s.x_gain=2; s.y_gain=.5;
    s.ads_gain=.4; s.scoped_gain=.2;
    auto d=mag_controls_mouse_look(125,-80,s,AimMode::Hip);
    check(close(d.yaw,.25) && close(d.pitch,.04), "right/down counts become right/up angular displacement");
    d=mag_controls_mouse_look(125,-80,s,AimMode::ADS);
    check(close(d.yaw,.1) && close(d.pitch,.016), "ADS gain applied once");
    s.invert_y=true;
    d=mag_controls_mouse_look(125,-80,s,AimMode::Scoped);
    check(close(d.yaw,.05) && close(d.pitch,-.008), "scoped overrides ADS; invert Y");
    s.radians_per_count=std::numeric_limits<double>::quiet_NaN();
    bool rejected=false; try { (void)mag_controls_mouse_look(1,1,s,AimMode::Hip); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"invalid sensitivity rejected");
}
static void controller_tests() {
    ControllerLookSettings s; s.response=ControllerResponse::Linear;
    s.inner_x=.2; s.outer_x=.1; s.inner_y=.1; s.outer_y=.2;
    s.x_gain=2; s.y_gain=.5; s.ads_gain=.4; s.scoped_gain=.2;
    auto d=mag_controls_controller_look(.55,-.45,s,AimMode::Hip);
    check(close(d.x,.5) && close(d.y,-.5), "linear axial mapping once");
    check(close(d.rate_scale_x,2) && close(d.rate_scale_y,.5), "rate gains separate from normalized axes");
    d=mag_controls_controller_look(.2,-.9,s,AimMode::Scoped);
    check(close(d.x,0) && close(d.y,-1), "deadzone boundary and outer saturation");
    check(close(d.rate_scale_x,.4) && close(d.rate_scale_y,.1), "scoped gain applied once");
    s.response=ControllerResponse::Native;
    d=mag_controls_controller_look(.55,-.45,s,AimMode::Hip);
    check(close(d.x,.55) && close(d.y,-.45) && d.use_native_curve, "native does not stack host deadzone");
    s.response=ControllerResponse::Linear; s.inner_x=.6; s.outer_x=.5;
    bool rejected=false; try { (void)mag_controls_controller_look(0,0,s,AimMode::Hip); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"overlapping deadzones rejected");
}
static ControlsProfile bindings_profile() {
    ControlsProfile p;
    p.bindings={{{InputSource::Keyboard,26},1,1},{{InputSource::Keyboard,82},1,1},
                {{InputSource::Mouse,0},2,1},{{InputSource::Controller,0},3,1}};
    return p;
}
static double action_value(const HostInputFrame& frame, ActionId action) {
    for (const auto& v:frame.values) if(v.action==action) return v.value;
    return 0;
}
static unsigned event_count(const HostInputFrame& frame, ActionId action, ActionEventKind kind) {
    unsigned count=0; for(const auto& e:frame.events) if(e.action==action && e.kind==kind) ++count; return count;
}
static void frame_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true; capture.controller_connected=true;
    mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Keyboard,26},true,1});
    mag_controls_push_mouse(125,-80);
    InputFrameKey key{10,1,0};
    bool bad_slot=false;
    try { (void)mag_controls_acquire_frame({10,1,1},InputContext::OnFoot); } catch(const std::invalid_argument&) { bad_slot=true; }
    check(bad_slot,"unsupported player slot rejected before draining shared input");
    const auto& first=mag_controls_acquire_frame(key,InputContext::OnFoot);
    check(action_value(first,1)==1 && event_count(first,1,ActionEventKind::Press)==1,"current action available at first frame acquisition");
    check(first.mouse_dx==125 && first.mouse_dy==-80,"current captured motion latched");
    check(!mag_controls_assist_allowed(first.policy),"first KBM action shares disable policy");
    const auto& again=mag_controls_acquire_frame(key,InputContext::OnFoot);
    check(&first==&again && again.mouse_dx==125,"same key reuses immutable frame");
    ConsumptionLedger ledger;
    auto view=mag_controls_actions(first,InputContext::OnFoot,ledger);
    check(view.events.size()==1 && ledger.consume(view.events[0].token),"native consumer can consume event token");
    check(mag_controls_actions(first,InputContext::OnFoot,ledger).events.empty(),"consumed event not redelivered");
    mag_controls_retire_frame(key);
    mag_controls_push_mouse(5,6);
    bool rejected=false;
    try { (void)mag_controls_acquire_frame(key,InputContext::OnFoot); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"retired key cannot drain fresh motion");
    const auto& next=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(next.mouse_dx==5 && next.mouse_dy==6 && event_count(next,1,ActionEventKind::Press)==0,"new key gets only new motion and no repeated press");
}
static void binding_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.controller_connected=true; mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Keyboard,26},true,1});
    (void)mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    mag_controls_push_event({{InputSource::Keyboard,82},true,2});
    mag_controls_push_event({{InputSource::Keyboard,26},false,3});
    const auto& held=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(action_value(held,1)==1 && event_count(held,1,ActionEventKind::Release)==0,"secondary keeps action held");
    mag_controls_push_event({{InputSource::Keyboard,82},false,4});
    const auto& released=mag_controls_acquire_frame({10,3,0},InputContext::OnFoot);
    check(action_value(released,1)==0 && event_count(released,1,ActionEventKind::Release)==1,"last binding release releases action once");
    auto p=bindings_profile(); p.mode=InputMode::Controller; mag_controls_configure(p);
    mag_controls_push_event({{InputSource::Keyboard,26},true,5});
    mag_controls_push_event({{InputSource::Controller,0},true,6});
    const auto& controller=mag_controls_acquire_frame({10,4,0},InputContext::OnFoot);
    check(action_value(controller,1)==0 && action_value(controller,3)==1,"exclusive Controller source filter");
}
static void device_tests() {
    ControlsProfile profile; profile.bindings={{{InputSource::Controller,0},1,1},{{InputSource::Keyboard,26},1,1}};
    mag_controls_configure(profile);
    InputCaptureState capture; capture.controller_connected=true; mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Controller,0},true,1});
    (void)mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    mag_controls_push_event({{InputSource::Keyboard,26},true,2});
    (void)mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    capture.controller_connected=false; mag_controls_set_capture_state(capture);
    const auto& keyboard=mag_controls_acquire_frame({10,3,0},InputContext::OnFoot);
    check(action_value(keyboard,1)==1 && event_count(keyboard,1,ActionEventKind::Release)==0,"disconnect preserves another held binding");
    mag_controls_push_event({{InputSource::Keyboard,26},false,3});
    (void)mag_controls_acquire_frame({10,4,0},InputContext::OnFoot);
    mag_controls_push_event({{InputSource::Controller,0},true,4});
    const auto& late=mag_controls_acquire_frame({10,5,0},InputContext::OnFoot);
    check(action_value(late,1)==0,"late disconnected callback cannot reactivate action");
    mag_controls_push_event({{InputSource::Controller,0},false,5});
    capture.controller_connected=true; mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Controller,0},true,6});
    const auto& reconnected=mag_controls_acquire_frame({10,6,0},InputContext::OnFoot);
    check(action_value(reconnected,1)==1,"release while disconnected permits fresh press after reconnect");
}
static void device_order_tests() {
    ControlsProfile profile; profile.bindings={{{InputSource::Controller,0},1,1},{{InputSource::Keyboard,26},1,1}};
    mag_controls_configure(profile);
    InputCaptureState capture; capture.controller_connected=true; mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Controller,0},true,1});
    (void)mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    mag_controls_push_event({{InputSource::Keyboard,26},true,2});
    mag_controls_push_event({{InputSource::Keyboard,26},false,3});
    capture.controller_connected=false; mag_controls_set_capture_state(capture);
    const auto& frame=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(action_value(frame,1)==0 && event_count(frame,1,ActionEventKind::Press)==0 &&
          event_count(frame,1,ActionEventKind::Release)==1,"disconnect preserves queued cross-source event order");
}
static void motion_detection_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true; capture.controller_connected=true;
    mag_controls_set_capture_state(capture);
    mag_controls_push_mouse(12,5); mag_controls_push_mouse(-12,-5);
    const auto& frame=mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    check(frame.mouse_dx==0 && frame.mouse_dy==0,"opposite mouse counts cancel displacement");
    check(frame.policy.kbm_used_this_process && !mag_controls_assist_allowed(frame.policy),"captured gameplay motion counts as KBM even if net displacement cancels");
}
static void axes_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.controller_connected=true;
    mag_controls_set_capture_state(capture);
    ControllerAxes axes; axes.look_x=.55; axes.look_y=-.25; axes.left_trigger=.6;
    mag_controls_push_controller_axes(axes,false);
    check(!mag_controls_acquire_frame({10,1,0},InputContext::OnFoot).controller_axes,"held axes require neutral after configuration");
    mag_controls_push_controller_axes({},true);
    mag_controls_push_controller_axes(axes,false);
    const auto first=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(first.controller_axes && close(first.controller_axes->look_x,.55) && close(first.controller_axes->left_trigger,.6),"pre-quantization signed axes latched");
    axes.look_x=.7; mag_controls_push_controller_axes(axes,false);
    check(close(mag_controls_acquire_frame({10,2,0},InputContext::OnFoot).controller_axes->look_x,.55),"same key retains immutable axis sample");
    check(close(mag_controls_acquire_frame({10,3,0},InputContext::OnFoot).controller_axes->look_x,.7),"later key gets fresh sample");
    check(close(mag_controls_acquire_frame({10,4,0},InputContext::OnFoot).controller_axes->look_x,.7),"sustained deflection persists across simulation updates");
    capture.focused=false; mag_controls_set_capture_state(capture);
    mag_controls_push_controller_axes(axes,false);
    capture.focused=true; mag_controls_set_capture_state(capture);
    mag_controls_push_controller_axes(axes,false);
    check(!mag_controls_acquire_frame({10,5,0},InputContext::OnFoot).controller_axes,"focus reset requires neutral before axes return");
    mag_controls_push_controller_axes({},true); mag_controls_push_controller_axes(axes,false);
    check(mag_controls_acquire_frame({10,6,0},InputContext::OnFoot).controller_axes.has_value(),"fresh post-neutral axes accepted");
    capture.controller_connected=false; mag_controls_set_capture_state(capture);
    mag_controls_push_controller_axes(axes,false);
    check(!mag_controls_acquire_frame({10,7,0},InputContext::OnFoot).controller_axes,"late disconnected axes rejected");
    axes.look_x=std::numeric_limits<double>::quiet_NaN();
    bool rejected=false; try { mag_controls_push_controller_axes(axes,false); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"nonfinite axis rejected before state mutation");
}
static void toggle_tests() {
    ControlsProfile p;
    ControlBinding keyboard{{InputSource::Keyboard,26},1,1}; keyboard.behavior=BindingBehavior::Toggle;
    p.bindings={keyboard,{{InputSource::Controller,0},1,1}};
    mag_controls_configure(p);
    InputCaptureState capture; capture.controller_connected=true; mag_controls_set_capture_state(capture);
    mag_controls_push_event({keyboard.control,true,1}); mag_controls_push_event({keyboard.control,true,2});
    mag_controls_push_event({keyboard.control,false,3});
    const auto& first=mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    check(action_value(first,1)==1 && event_count(first,1,ActionEventKind::Press)==1 && event_count(first,1,ActionEventKind::Release)==0,"toggle survives release and ignores duplicate down callback");
    mag_controls_push_event({{InputSource::Controller,0},true,4});
    mag_controls_push_event({keyboard.control,true,5}); mag_controls_push_event({keyboard.control,false,6});
    const auto& mixed=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(action_value(mixed,1)==1 && mixed.events.empty(),"controller hold keeps action down while keyboard toggles off");
    mag_controls_push_event({{InputSource::Controller,0},false,7});
    const auto& last=mag_controls_acquire_frame({10,3,0},InputContext::OnFoot);
    check(action_value(last,1)==0 && event_count(last,1,ActionEventKind::Release)==1,"last active source releases once");
    mag_controls_push_event({keyboard.control,true,8}); mag_controls_push_event({keyboard.control,false,9});
    (void)mag_controls_acquire_frame({10,4,0},InputContext::OnFoot);
    capture.controller_connected=false; mag_controls_set_capture_state(capture);
    check(action_value(mag_controls_acquire_frame({10,5,0},InputContext::OnFoot),1)==1,"controller disconnect preserves keyboard toggle latch");
    capture.focused=false; mag_controls_set_capture_state(capture);
    const auto& off=mag_controls_acquire_frame({10,6,0},InputContext::OnFoot);
    ConsumptionLedger ledger;
    check(action_value(off,1)==0 && mag_controls_actions(off,InputContext::OnFoot,ledger).events.size()==1,"focus loss releases toggle latch through consumer");
    capture.focused=true; mag_controls_set_capture_state(capture);
    check(action_value(mag_controls_acquire_frame({10,7,0},InputContext::OnFoot),1)==0,"toggle never resumes after lifecycle reset");
}
static void profile_validation_tests() {
    auto p=bindings_profile(); mag_controls_configure(p);
    mag_controls_push_event({{InputSource::Keyboard,26},true,1});
    (void)mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    p.bindings.push_back({{InputSource::Keyboard,27},1,1});
    bool rejected=false; try { mag_controls_configure(p); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"a third binding in the same input profile/context is rejected");
    check(action_value(mag_controls_acquire_frame({10,2,0},InputContext::OnFoot),1)==1,"invalid profile cannot mutate active inputs");
    p.bindings.back().contexts=uint32_t(InputContext::Vehicle);
    mag_controls_configure(p);
    p.bindings.push_back({{InputSource::Controller,1},1,1});
    mag_controls_configure(p);
}
static void focus_tests() {
    mag_controls_configure(bindings_profile());
    mag_controls_push_event({{InputSource::Mouse,0},true,1});
    InputFrameKey key{10,1,0};
    const auto& fire=mag_controls_acquire_frame(key,InputContext::OnFoot);
    const auto generation=fire.capture_generation;
    check(action_value(fire,2)==1,"held gameplay action before focus loss");
    InputCaptureState capture; capture.focused=false;
    mag_controls_set_capture_state(capture);
    check(!mag_controls_frame_valid(key,generation),"focus loss revokes old frame immediately");
    const auto& off=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(action_value(off,2)==0 && event_count(off,2,ActionEventKind::Release)==1,"focus loss releases held action");
    ConsumptionLedger cleanup;
    const auto view=mag_controls_actions(off,InputContext::OnFoot,cleanup);
    check(view.values.empty() && view.events.size()==1 && view.events[0].cleanup && view.events[0].kind==ActionEventKind::Release,"unfocused consumer receives cleanup release without gameplay values");
    cleanup.consume(view.events[0].token);
    check(mag_controls_actions(off,InputContext::OnFoot,cleanup).events.empty(),"cleanup token consumed once while unfocused");
    capture.focused=true; mag_controls_set_capture_state(capture);
    const auto& resumed=mag_controls_acquire_frame({10,3,0},InputContext::OnFoot);
    check(action_value(resumed,2)==0,"held button does not reactivate after focus restore");
    mag_controls_push_event({{InputSource::Mouse,0},false,2});
    mag_controls_push_event({{InputSource::Mouse,0},true,3});
    const auto& fresh=mag_controls_acquire_frame({10,4,0},InputContext::OnFoot);
    check(action_value(fresh,2)==1 && fresh.policy.kbm_used_this_process,"fresh activation restores action without clearing provenance");
}
static void sample_phase_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true; capture.controller_connected=true;
    mag_controls_set_capture_state(capture);
    ControllerAxes axes; axes.look_x=.25;
    mag_controls_push_controller_axes(axes,true);
    mag_controls_push_event({{InputSource::Keyboard,26},true,1});
    mag_controls_push_mouse(7,-2);
    const InputFrameKey first{10,1,0};
    const auto& sample=mag_controls_seal_sample(first);
    check(sample.mouse_dx==7 && sample.mouse_dy==-2 && sample.events.size()==1,"seal freezes raw physical events/counts");
    check(sample.controller_axes && sample.controller_axes->look_x==.25,"seal includes this poll's raw axes");
    check(!mag_controls_current_policy().kbm_used_this_process,"uninterpreted physical input is not accepted gameplay");
    mag_controls_push_event({{InputSource::Keyboard,26},false,2});
    mag_controls_push_mouse(99,3); axes.look_x=.75; mag_controls_push_controller_axes(axes,false);
    check(&mag_controls_seal_sample(first)==&sample && sample.mouse_dx==7 && sample.events.size()==1,"same sample key cannot drain later input");
    const auto& frame=mag_controls_attach_sample(first,InputContext::OnFoot);
    check(frame.mouse_dx==7 && frame.mouse_dy==-2 && frame.controller_axes->look_x==.25,"semantic attachment uses sealed data");
    check(action_value(frame,1)==1 && event_count(frame,1,ActionEventKind::Press)==1,"late key release cannot alter earlier gameplay view");
    check(frame.policy.kbm_used_this_process && !mag_controls_assist_allowed(frame.policy),"accepted gameplay revokes assist at attachment");
    check(&mag_controls_attach_sample(first,InputContext::OnFoot)==&frame,"repeated attachment preserves immutable view");
    const auto& next=mag_controls_seal_sample({10,2,0});
    check(next.mouse_dx==99 && next.events.size()==1 && next.controller_axes->look_x==.75,"late events/axes wait for next actual poll");
    const auto& released=mag_controls_attach_sample(next.key,InputContext::OnFoot);
    check(action_value(released,1)==0 && event_count(released,1,ActionEventKind::Release)==1,"next poll delivers the release once");
}
static void sample_lifecycle_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true;
    mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Mouse,0},true,1});
    check(action_value(mag_controls_acquire_frame({10,1,0},InputContext::OnFoot),2)==1,"fire held before skipped interpretation");
    mag_controls_push_event({{InputSource::Mouse,0},false,2});
    (void)mag_controls_seal_sample({10,2,0});
    mag_controls_retire_frame({10,2,0});
    const auto& cancelled=mag_controls_acquire_frame({10,3,0},InputContext::OnFoot);
    check(action_value(cancelled,2)==0 && event_count(cancelled,2,ActionEventKind::Release)==1 && cancelled.events[0].cleanup,"discarded interpretation cannot leave a missed release stuck");
    mag_controls_push_mouse(55,0);
    bool rejected=false;
    try { (void)mag_controls_seal_sample({10,2,0}); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"retired raw sample cannot be acquired again");
    mag_controls_retire_frame({10,2,0});
    const auto& fresh=mag_controls_seal_sample({10,4,0});
    check(fresh.mouse_dx==55,"late retire/stale acquire cannot drain a later poll's mouse counts");
    capture.focused=false; mag_controls_set_capture_state(capture);
    rejected=false;
    try { (void)mag_controls_attach_sample(fresh.key,InputContext::OnFoot); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"focus loss between poll and interpretation rejects the sealed sample");
    mag_controls_retire_frame({10,4,0});
    capture.focused=true; mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Keyboard,26},true,3});
    const auto& next=mag_controls_acquire_frame({10,5,0},InputContext::OnFoot);
    check(next.mouse_dx==0 && action_value(next,1)==1,"fresh input resumes without replaying cancelled mouse motion");
}
static void sample_context_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true;
    mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Keyboard,26},true,1}); mag_controls_push_mouse(9,0);
    bool rejected=false;
    try { (void)mag_controls_seal_sample({10,1,1}); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"unsupported raw sample slot is rejected before draining input");
    const auto& sample=mag_controls_seal_sample({10,1,0});
    check(sample.mouse_dx==9 && sample.events.size()==1,"valid raw sample retains input after rejected slot");
    const auto& menu=mag_controls_attach_sample(sample.key,InputContext::Menu);
    check(menu.mouse_dx==0 && !menu.policy.kbm_used_this_process,"menu interpretation does not accept gameplay motion or provenance");
    rejected=false;
    try { (void)mag_controls_attach_sample(sample.key,InputContext::OnFoot); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"same physical press cannot be reinterpreted in a new context");
    const auto& gameplay=mag_controls_acquire_frame({10,2,0},InputContext::OnFoot);
    check(action_value(gameplay,1)==0 && !gameplay.policy.kbm_used_this_process,"menu-held key cannot activate on context transition");
    mag_controls_push_event({{InputSource::Keyboard,26},false,2});
    mag_controls_push_event({{InputSource::Keyboard,26},true,3});
    check(action_value(mag_controls_acquire_frame({10,3,0},InputContext::OnFoot),1)==1,"fresh key activation works after the new context is established");
}
static void sample_revocation_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true; capture.controller_connected=true;
    mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Keyboard,26},true,1});
    (void)mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    mag_controls_push_event({{InputSource::Controller,0},true,2});
    mag_controls_push_event({{InputSource::Mouse,0},true,3}); mag_controls_push_mouse(8,0);
    (void)mag_controls_seal_sample({10,2,0});
    capture.controller_connected=false; mag_controls_set_capture_state(capture);
    const auto& frame=mag_controls_attach_sample({10,2,0},InputContext::OnFoot);
    check(action_value(frame,1)==1 && action_value(frame,2)==1 && frame.mouse_dx==8,"controller revocation between phases preserves keyboard hold and sealed mouse input");
    check(action_value(frame,3)==0 && !frame.controller_axes && !frame.policy.controller_connected,"revoked controller cannot contribute from the sealed sample");
    check(event_count(frame,1,ActionEventKind::Release)==0 && event_count(frame,2,ActionEventKind::Press)==1,"source revocation cannot create unrelated release/press pairs");
}
static void sample_mouse_revocation_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture; capture.mouse_captured=true; capture.controller_connected=true;
    capture.stick_neutral=false; capture.fresh_stick_deflection=true;
    mag_controls_set_capture_state(capture);
    mag_controls_push_event({{InputSource::Mouse,0},true,1}); mag_controls_push_mouse(8,0);
    (void)mag_controls_seal_sample({10,1,0});
    capture.mouse_captured=false; mag_controls_set_capture_state(capture);
    const auto& frame=mag_controls_attach_sample({10,1,0},InputContext::OnFoot);
    check(frame.mouse_dx==0 && action_value(frame,2)==0,"capture loss revokes sealed mouse contributions");
    check(!frame.policy.kbm_used_this_process && frame.policy.look_owner==LookOwner::Controller,"revoked mouse cannot latch gameplay provenance or seize look");
}
static void configure_policy_tests() {
    auto profile=bindings_profile(); profile.mode=InputMode::KBM;
    mag_controls_configure(profile);
    check(!mag_controls_assist_allowed(mag_controls_current_policy()),"KBM denies assist immediately");
    profile.mode=InputMode::Controller; mag_controls_configure(profile);
    check(!mag_controls_assist_allowed(mag_controls_current_policy()),"configuration cannot grant Controller permission before a valid boundary");
    (void)mag_controls_acquire_frame({10,1,0},InputContext::OnFoot);
    check(mag_controls_assist_allowed(mag_controls_current_policy()),"explicit Controller permission is available at a valid boundary");
    profile.mode=InputMode::Auto; mag_controls_configure(profile);
    check(!mag_controls_assist_allowed(mag_controls_current_policy()),"Auto immediately revokes Controller exception with retained KBM history");
    (void)mag_controls_seal_sample({10,2,0});
    InputCaptureState capture; capture.focused=false; mag_controls_set_capture_state(capture);
    bool rejected=false;
    try { (void)mag_controls_attach_sample({10,2,0},InputContext::OnFoot); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected && !mag_controls_assist_allowed(mag_controls_current_policy()),"rejected sample cannot preserve the old Controller exception");
}
static void native_neutral_tests() {
    mag_controls_configure(bindings_profile());
    InputCaptureState capture;capture.controller_connected=true;capture.mouse_captured=true;
    // Legacy/backend metadata deliberately disagrees with the actual native
    // interpretation; it must not override that later observation.
    capture.stick_neutral=true;capture.fresh_stick_deflection=true;mag_controls_set_capture_state(capture);
    ControllerAxes axes;axes.look_x=.7;mag_controls_capture_controller_axes(axes);mag_controls_push_mouse(1,0);
    const auto& raw=mag_controls_seal_sample({10,1,0});
    check(raw.controller_axes && raw.controller_axes->look_x==.7,"raw capture retains un-rearmed physical axes for later native interpretation");
    const auto& held=mag_controls_attach_sample(raw.key,InputInterpretation{InputContext::OnFoot,false});
    check(!held.controller_axes && held.policy.look_owner==LookOwner::Mouse,"held stick without native neutral cannot rearm or reclaim look");
    check(!mag_controls_attach_sample(raw.key,InputInterpretation{InputContext::OnFoot,true}).controller_axes,"repeated callback cannot reinterpret the frozen neutral observation");
    axes.look_x=.001;mag_controls_capture_controller_axes(axes);
    (void)mag_controls_seal_sample({10,2,0});
    const auto& neutral=mag_controls_attach_sample({10,2,0},InputInterpretation{InputContext::OnFoot,true});
    check(neutral.controller_axes && neutral.controller_axes->look_x==.001 && neutral.policy.look_owner==LookOwner::Mouse,"native neutral rearms without adding a host deadzone or seizing look");
    axes.look_x=.4;mag_controls_capture_controller_axes(axes);
    (void)mag_controls_seal_sample({10,3,0});
    const auto& fresh=mag_controls_attach_sample({10,3,0},InputInterpretation{InputContext::OnFoot,false});
    check(fresh.controller_axes && fresh.policy.look_owner==LookOwner::Controller && !mag_controls_assist_allowed(fresh.policy),"fresh native deflection reclaims look without restoring assist");
}
int main(int argc, char** argv) {
    try {
        if (argc!=2) return 2;
        std::string name=argv[1];
        if(name=="policy") policy_tests(); else if(name=="mouse") mouse_tests(); else if(name=="controller") controller_tests();
        else if(name=="frames") frame_tests(); else if(name=="bindings") binding_tests(); else if(name=="focus") focus_tests();
        else if(name=="validation") profile_validation_tests(); else if(name=="toggle") toggle_tests(); else if(name=="motion") motion_detection_tests(); else if(name=="axes") axes_tests();
        else if(name=="device") device_tests(); else if(name=="device-order") device_order_tests(); else if(name=="sample-phases") sample_phase_tests();
        else if(name=="sample-lifecycle") sample_lifecycle_tests(); else if(name=="sample-contexts") sample_context_tests(); else if(name=="sample-revocation") sample_revocation_tests();
        else if(name=="sample-mouse-revocation") sample_mouse_revocation_tests(); else if(name=="configure-policy") configure_policy_tests(); else if(name=="native-neutral") native_neutral_tests(); else return 2;
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
