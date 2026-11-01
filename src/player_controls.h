#pragma once
#include <cstdint>
#include <compare>
#include <optional>
#include <set>
#include <vector>
namespace mag {
enum class InputMode { Auto, Controller, KBM };
enum class InputSource { Keyboard, Mouse, Controller };
enum class LookOwner { Controller, Mouse };
enum class AimMode { Hip, ADS, Scoped };
enum class ControllerResponse { Native, Linear };
struct InputPolicy {
    InputMode mode=InputMode::Auto;
    LookOwner look_owner=LookOwner::Controller;
    bool kbm_used_this_process=false, stick_rearmed=true, controller_connected=false;
};
struct PolicyEvents {
    std::optional<InputMode> requested_mode;
    bool focused=true, gameplay=true, controller_connected=false;
    bool gameplay_kbm=false, mouse_motion=false, mouse_captured=false;
    bool stick_neutral=true, fresh_stick_deflection=false;
};
struct MouseLookSettings {
    double radians_per_count=.001, x_gain=1, y_gain=1;
    double hip_gain=1, ads_gain=1, scoped_gain=1;
    bool invert_y=false;
};
struct MouseLookDelta { double yaw=0, pitch=0; };
struct ControllerLookSettings {
    ControllerResponse response=ControllerResponse::Native;
    double inner_x=0, inner_y=0, outer_x=0, outer_y=0;
    double x_gain=1, y_gain=1, hip_gain=1, ads_gain=1, scoped_gain=1;
    bool invert_x=false, invert_y=false;
};
// Samples before cellPad quantization; sticks signed [-1,1], triggers [0,1].
struct ControllerAxes {
    double move_x=0, move_y=0, look_x=0, look_y=0, left_trigger=0, right_trigger=0;
};
struct ControllerLookRequest {
    double x=0, y=0, rate_scale_x=1, rate_scale_y=1;
    bool use_native_curve=true;
};
enum class InputContext : uint32_t { OnFoot=1, Vehicle=2, Turret=4, Tactical=8, Menu=16, Settings=32, Rebinding=64 };
using ActionId=uint32_t;
struct PhysicalControl {
    InputSource source=InputSource::Keyboard;
    uint32_t code=0;
    friend auto operator<=>(const PhysicalControl&, const PhysicalControl&)=default;
};
enum class BindingBehavior { Hold, Toggle };
struct ControlBinding {
    PhysicalControl control;
    ActionId action=0;
    uint32_t contexts=uint32_t(InputContext::OnFoot);
    BindingBehavior behavior=BindingBehavior::Hold;
};
struct ControlsProfile {
    InputMode mode=InputMode::Auto;
    MouseLookSettings mouse;
    ControllerLookSettings controller;
    std::vector<ControlBinding> bindings;
};
struct InputFrameKey {
    uint64_t process_session=0, simulation_sequence=0;
    uint32_t player_slot=0;
    friend auto operator<=>(const InputFrameKey&, const InputFrameKey&)=default;
};
enum class InputResetReason { Focus, Capture, Context, Mode, Device, Actor };
enum class ActionEventKind { Press, Release, Tap, Hold, Repeat, DoubleTap };
struct ActionEvent {
    ActionId action=0;
    ActionEventKind kind=ActionEventKind::Press;
    uint64_t token=0;
    InputSource source=InputSource::Keyboard;
    bool cleanup=false;
};
struct ActionValue { ActionId action=0; double value=0; };
struct InputCaptureState {
    bool focused=true, supported_context=true, mouse_captured=false, controller_connected=false;
    bool stick_neutral=true, fresh_stick_deflection=false;
    uint64_t time_ns=0;
};
struct PhysicalInputEvent { PhysicalControl control; bool down=false; uint64_t time_ns=0; };
struct HostInputFrame {
    InputFrameKey key;
    InputContext context=InputContext::OnFoot;
    uint64_t capture_generation=0;
    InputPolicy policy;
    int64_t mouse_dx=0, mouse_dy=0;
    std::optional<ControllerAxes> controller_axes;
    std::vector<ActionValue> values;
    std::vector<ActionEvent> events;
};
struct ConsumptionLedger {
    std::set<uint64_t> consumed;
    bool consume(uint64_t token) { return consumed.insert(token).second; }
};
struct ActionFrameView { std::vector<ActionValue> values; std::vector<ActionEvent> events; };
void mag_controls_validate_profile(const ControlsProfile& profile);
void mag_controls_configure(const ControlsProfile& profile);
void mag_controls_set_capture_state(const InputCaptureState& state);
void mag_controls_push_event(const PhysicalInputEvent& event);
void mag_controls_push_mouse(int64_t dx, int64_t dy);
// Backend reports neutral without changing the supplied sample. A reset requires
// one neutral sample before accepting a new deflection. No host curve is added.
void mag_controls_push_controller_axes(const ControllerAxes&, bool neutral);
// Only local player slot zero is supported. Acquisition/retirement and consumers
// share the simulation thread. Capture callbacks may run on other threads.
// Reference remains valid only until retirement or acquisition of a different key.
const HostInputFrame& mag_controls_acquire_frame(InputFrameKey key, InputContext classified_context);
ActionFrameView mag_controls_actions(const HostInputFrame&, InputContext, ConsumptionLedger&);
void mag_controls_retire_frame(InputFrameKey key);
void mag_controls_release_input(InputResetReason reason);
bool mag_controls_frame_valid(InputFrameKey key, uint64_t capture_generation);
InputPolicy mag_controls_current_policy();
InputPolicy mag_controls_update_policy(const InputPolicy&, const PolicyEvents&);
// Permission only. Native eligibility and completed cache cleanup are additional gates.
bool mag_controls_assist_allowed(const InputPolicy&);
bool mag_controls_gameplay_source_allowed(const InputPolicy&, InputSource);
// Canonical counts: right-positive X, down-positive Y. Angles: right/up-positive.
MouseLookDelta mag_controls_mouse_look(int64_t dx, int64_t dy, const MouseLookSettings&, AimMode);
ControllerLookRequest mag_controls_controller_look(double x, double y, const ControllerLookSettings&, AimMode);
} // namespace mag
