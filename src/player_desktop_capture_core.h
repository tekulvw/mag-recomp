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
//
// Threads: the request_* functions, stamp(), enabled_requested() and raw_mouse_available() may be
// called from any thread. Everything else runs on the one thread that owns the backend's window.
#include "player_controls.h"
#include "player_desktop_capture.h"
#include <atomic>
#include <cstdint>
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
};

// Where accepted input goes. Production uses DesktopCaptureEngineSink.
class DesktopCaptureSink {
public:
    virtual ~DesktopCaptureSink() = default;
    virtual void push_event(const PhysicalInputEvent& event) = 0;
    // dx right-positive, dy up-positive.
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
    // Relative motion: dx right-positive, dy up-positive.
    void mouse_motion(double dx, double dy, DesktopCaptureGate::Stamp stamp);
    // Re-apply relative mode at the next refresh (a click in the window, focus returning).
    void request_recapture() { needs_recapture_ = true; }
    // The attached window changed or went away: drop relative mode, release everything and
    // require a fresh activation.
    void window_changed();
    // Observed input was lost (for example an overflowing event queue): release everything and
    // require a fresh activation.
    void interrupt();

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
};

// Whether the backend withholds the host's quit key (Escape) from the SDK's window pump. The
// controls bind Escape to Start, so while the adapter owns the keyboard and captures the focused
// game window Escape must not end the game. A dialog that owns the keyboard (the on-screen
// keyboard) always receives it.
struct DesktopQuitKeyContext {
    bool quit_key_down = false;        // the event is a press (or repeat) of the quit key
    bool keyboard_owned = false;       // the adapter owns keyboard interpretation
    bool capture_requested = false;    // the adapter requests desktop capture
    bool on_game_window = false;       // the event targets the game window
    bool game_window_focused = false;  // the game window has keyboard focus
    bool dialog_active = false;        // a native dialog owns the keyboard
};
constexpr bool desktop_capture_withholds_quit_key(const DesktopQuitKeyContext& c) {
    return c.quit_key_down && c.keyboard_owned && c.capture_requested && c.on_game_window &&
           c.game_window_focused && !c.dialog_active;
}

}  // namespace mag
