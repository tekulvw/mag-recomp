#pragma once
// Portable keyboard/mouse desktop capture state machine (docs/native-controls.md, section 8.1).
//
// A host backend observes its window system's keyboard, mouse-button, relative-motion and focus
// events and forwards them here. This core decides what reaches the input engine:
//
// - Capture exists only while the adapter requests it ("installed"). Installing or removing it
//   releases everything this capture pushed. The engine's window fields (focused, mouse captured)
//   are written only while installed; removal hands them back at their defaults (focused, not
//   captured) so a capture that no longer runs does not gate a controller.
// - Input counts only while the capture is "ready": installed, a game window is attached and has
//   keyboard focus, and nothing else (a dialog or settings panel) owns input. Losing readiness
//   tells the engine the window lost focus and releases every control this capture held.
// - Regaining readiness is a fresh activation: controls still physically held are reported as
//   held while the engine still sees the window unfocused, so each needs a release and a new press
//   before it acts. Presses observed before readiness was published are ignored the same way.
// - Relative mouse mode is requested while ready, available and the adapter wants mouse look. A
//   recapture request (a click in the window, focus returning) re-applies it. Motion counts only
//   when it was observed while ready and relative under the current mode.
// - An intentional mouse-mode change (menu versus gameplay) changes relative motion only; held
//   keys and buttons stay held.
// - System shortcut keys (left and right GUI) are never captured.
// - A release the window system never delivers (focus juggling, a modal loop that read the
//   message, a lost raw-input report) must not keep a control held. While ready, the core asks the
//   platform every ReconcileIntervalNs whether each held key and button is still physically down;
//   one reported up on two checks in a row is released as if its release had arrived. Readings
//   count only once the platform has confirmed some held control as down, so a host that cannot
//   read physical state (or reads nothing without a permission) never releases anything.
//
// Threads: the request_* functions, stamp(), enabled_requested() and raw_mouse_available() may be
// called from any thread. Everything else runs on the one thread that owns the backend's window.
#include "player_controls.h"
#include "player_desktop_capture.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <utility>
#include <vector>
namespace mag {

struct DesktopPressedState {
    std::vector<uint32_t> keys;                          // USB HID keyboard usages
    std::vector<std::pair<uint32_t, uint64_t>> buttons;  // mouse button index, device identity
};

// What the core needs from the host window system.
class DesktopCapturePlatform {
public:
    virtual ~DesktopCapturePlatform() = default;
    virtual bool window_attached() = 0;
    // The game window has keyboard focus inside an active application.
    virtual bool window_focused() = 0;
    // Something other than the game (a text dialog, a settings panel) owns input.
    virtual bool input_blocked() = 0;
    // Relative motion can be acquired for the attached window.
    virtual bool raw_mouse_available() = 0;
    // Acquire (hide and confine the cursor, deliver relative motion) or release relative mode.
    // Returns whether acquisition succeeded; release is not expected to fail.
    virtual bool set_relative(bool on) = 0;
    // Physical keys and mouse buttons held right now.
    virtual DesktopPressedState pressed() = 0;
    virtual uint64_t now_ns() = 0;
    // Whether one key (a USB HID usage) or mouse button is physically down now; nullopt when the
    // platform cannot tell. The defaults cannot tell, which turns reconciliation off.
    virtual std::optional<bool> key_down(uint32_t) { return std::nullopt; }
    virtual std::optional<bool> button_down(uint32_t, uint64_t) { return std::nullopt; }
    // The core released a key whose release never arrived: forget any per-key state (auto-repeat
    // tracking) so the next press is a press.
    virtual void key_forgotten(uint32_t) {}
};

// Where accepted input goes. Production uses DesktopCaptureEngineSink.
class DesktopCaptureSink {
public:
    virtual ~DesktopCaptureSink() = default;
    virtual void push_event(const PhysicalInputEvent& event) = 0;
    // dx right-positive, dy down-positive (screen directions; mag_controls_mouse_look turns them into angles).
    virtual void push_relative_mouse(double dx, double dy) = 0;
    virtual void set_window_capture(bool focused, bool mouse_captured) = 0;
    virtual void release_source(InputSource source) = 0;
};

class DesktopCaptureEngineSink final : public DesktopCaptureSink {
public:
    void push_event(const PhysicalInputEvent& event) override { mag_controls_push_event(event); }
    void push_relative_mouse(double dx, double dy) override { mag_controls_push_relative_mouse(dx, dy); }
    void set_window_capture(bool focused, bool mouse_captured) override {
        mag_controls_set_window_capture(focused, mouse_captured);
    }
    void release_source(InputSource source) override { mag_controls_release_source(source); }
};

class DesktopCaptureCore {
public:
    DesktopCaptureCore(DesktopCapturePlatform& platform, DesktopCaptureSink& sink);

    // Any thread. Each returns whether the request changed.
    bool request_enabled(bool enabled);
    bool request_mouse(bool enabled);
    bool enabled_requested() const { return requested_.load(std::memory_order_acquire); }
    bool raw_mouse_available() const { return raw_.load(std::memory_order_acquire); }
    // Take when the backend observes an event, before it is handed to this thread.
    DesktopCaptureGate::Stamp stamp() const { return gate_.stamp(); }

    // Capture thread. Applies requests, readiness, fresh activation and relative mode.
    void refresh();
    // A keyboard edge; `usage` is a USB HID usage. Repeats must not be reported.
    void key(uint32_t usage, bool down, uint64_t time_ns, DesktopCaptureGate::Stamp stamp);
    // A mouse-button edge: index 0 left, 1 right, 2 middle, 3 and 4 the side buttons.
    void mouse_button(uint32_t index, uint64_t device, bool down, uint64_t time_ns,
                      DesktopCaptureGate::Stamp stamp);
    // Relative motion: dx right-positive, dy down-positive. Returns whether it reached the engine.
    bool mouse_motion(double dx, double dy, DesktopCaptureGate::Stamp stamp);
    // Whole wheel steps, positive away from the user (up). Each step is a press and release of
    // MouseWheelUp or MouseWheelDown; at most MaxWheelSteps per call. Counts only while ready.
    static constexpr int MaxWheelSteps = 8;
    void mouse_wheel(int steps, uint64_t device, uint64_t time_ns, DesktopCaptureGate::Stamp stamp);
    // The motion epoch a stamp carries, and the current one, for diagnostics.
    static uint32_t motion_epoch(DesktopCaptureGate::Stamp stamp) { return uint32_t(stamp.state >> 33); }
    uint32_t motion_epoch() const { return motion_epoch(gate_.stamp()); }
    // Re-apply relative mode at the next refresh (a click in the window, focus returning).
    void request_recapture() { needs_recapture_ = true; }
    // A click in the game window: take the cursor back only when the lock is not already held
    // (relative mode off, or the backend found the lock gone). Re-acquiring a held lock on every
    // click would move the pointer and discard look motion around each shot.
    void recapture_after_click(bool lock_held) {
        if (!relative_ || !lock_held) needs_recapture_ = true;
    }
    // The attached window changed or went away: drop relative mode, release everything and
    // require a fresh activation.
    void window_changed();
    // Observed input was lost (for example an overflowing event queue): release everything and
    // require a fresh activation.
    void interrupt();

    static constexpr uint64_t ReconcileIntervalNs = 250'000'000;
    // Controls released because the platform found them up although no release arrived.
    uint64_t lost_releases() const { return lost_releases_; }
    // The most recent of them, for a log line.
    std::optional<PhysicalControl> last_lost_release() const { return last_lost_; }

    bool installed() const { return installed_; }
    bool ready() const { return ready_; }
    bool relative() const { return relative_; }
    static bool captured_key(uint32_t usage);

private:
    using Button = std::pair<uint32_t, uint64_t>;
    void install();
    void uninstall();
    void activate();
    void release_held();
    void apply_relative(bool desired);
    void reconcile();
    void publish_capture(bool focused, bool mouse_captured);

    DesktopCapturePlatform& platform_;
    DesktopCaptureSink& sink_;
    std::atomic<bool> requested_{false}, mouse_requested_{true}, raw_{false};
    DesktopCaptureGate gate_;
    bool installed_ = false, ready_ = false, relative_ = false, needs_recapture_ = false;
    bool mouse_mode_ = true;
    std::optional<std::pair<bool, bool>> published_;
    std::set<uint32_t> keys_;
    std::set<Button> buttons_;
    // Reconciliation: held controls found up at the last check, whether the platform's readings
    // are trusted yet, when the next check is due and what it released.
    std::set<uint32_t> keys_up_;
    std::set<Button> buttons_up_;
    bool physical_trusted_ = false;
    uint64_t next_reconcile_ = 0, lost_releases_ = 0;
    std::optional<PhysicalControl> last_lost_;
};

// Whether the backend withholds the host's quit key (Escape) from the SDK's window pump. The SDK's
// window ends the game on an Escape press, with no question asked. Players press Escape to go back
// or pause, in the front end and lobby as well as in a match (where the controls bind it to Start),
// so it never ends the game on the focused game window: closing the window (its close button,
// Alt+F4) still does. A dialog that owns the keyboard (the on-screen keyboard) always receives it.
struct DesktopQuitKeyContext {
    bool quit_key_down = false;        // the event is a press (or repeat) of the quit key
    bool on_game_window = false;       // the event targets the game window
    bool game_window_focused = false;  // the game window has keyboard focus
    bool dialog_active = false;        // a native dialog owns the keyboard
};
constexpr bool desktop_capture_withholds_quit_key(const DesktopQuitKeyContext& c) {
    return c.quit_key_down && c.on_game_window && c.game_window_focused && !c.dialog_active;
}

// One log line for a change of capture state, or nullptr when nothing a player would notice
// changed. `ready` is keyboard and button capture of the focused game window; `relative` is the
// mouse locked to it for look.
constexpr const char* desktop_capture_transition(bool was_ready, bool was_relative, bool ready, bool relative) {
    if (!was_ready && ready) return relative ? "capture engaged; mouse locked to the game window"
                                             : "capture engaged; mouse free";
    if (was_ready && !ready) return "capture released (focus lost, window gone or input blocked); mouse free";
    if (ready && !was_relative && relative) return "mouse locked to the game window";
    if (ready && was_relative && !relative) return "mouse released (menu, or the lock could not be applied)";
    return nullptr;
}

// The backend's log line for a release the core reconciled (one per release; `reported` is the
// count already logged).
inline void desktop_capture_report_lost_releases(const DesktopCaptureCore& core, uint64_t& reported) {
    if (core.lost_releases() == reported) return;
    reported = core.lost_releases();
    if (const auto lost = core.last_lost_release())
        std::fprintf(stderr, "[game-controls] released %s %u: up on the device but its release was never reported\n",
                     lost->source == InputSource::Keyboard ? "key" : "mouse button", lost->code);
}

}  // namespace mag
