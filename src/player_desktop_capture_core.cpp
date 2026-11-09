// Portable desktop capture state machine; the behaviour is described in player_desktop_capture_core.h.
#include "player_desktop_capture_core.h"
namespace mag {
namespace {
constexpr uint32_t kFirstKey = 4, kLastKey = 231;   // USB HID keyboard usages the catalog binds
constexpr uint32_t kLeftGui = 227, kRightGui = 231;  // system shortcut keys stay with the host
}  // namespace

DesktopCaptureCore::DesktopCaptureCore(DesktopCapturePlatform& platform, DesktopCaptureSink& sink)
    : platform_(platform), sink_(sink) {}

bool DesktopCaptureCore::captured_key(uint32_t usage) {
    return usage >= kFirstKey && usage <= kLastKey && usage != kLeftGui && usage != kRightGui;
}

bool DesktopCaptureCore::request_enabled(bool enabled) {
    return requested_.exchange(enabled, std::memory_order_acq_rel) != enabled;
}

bool DesktopCaptureCore::request_mouse(bool enabled) {
    return mouse_requested_.exchange(enabled, std::memory_order_acq_rel) != enabled;
}

void DesktopCaptureCore::publish_capture(bool focused, bool mouse_captured) {
    const std::pair<bool, bool> next{focused, mouse_captured};
    if (published_ == next) return;
    published_ = next;
    sink_.set_window_capture(focused, mouse_captured);
}

void DesktopCaptureCore::release_held() {
    const uint64_t time = platform_.now_ns();
    auto keys = std::move(keys_);
    auto buttons = std::move(buttons_);
    keys_.clear();
    buttons_.clear();
    for (uint32_t usage : keys) sink_.push_event({{InputSource::Keyboard, usage}, false, time, 0});
    for (auto [index, device] : buttons) sink_.push_event({{InputSource::Mouse, index}, false, time, device});
}

void DesktopCaptureCore::apply_relative(bool desired) {
    if (desired) {
        if (relative_ && !needs_recapture_) return;
        if (!platform_.set_relative(true)) {
            if (relative_) {
                platform_.set_relative(false);
                relative_ = false;
            }
            return;
        }
        relative_ = true;
        needs_recapture_ = false;
    } else if (relative_) {
        platform_.set_relative(false);
        relative_ = false;
    }
}

void DesktopCaptureCore::install() {
    // Nothing observed before installation counts; start from released engine sources.
    gate_.invalidate();
    gate_.publish(false, false);
    ready_ = false;
    publish_capture(false, false);
    sink_.release_source(InputSource::Keyboard);
    sink_.release_source(InputSource::Mouse);
    release_held();
    installed_ = true;
}

void DesktopCaptureCore::uninstall() {
    installed_ = false;
    ready_ = false;
    needs_recapture_ = false;
    gate_.invalidate();
    gate_.publish(false, false);
    apply_relative(false);
    publish_capture(false, false);
    release_held();
    raw_.store(false, std::memory_order_release);
    // Hand the engine's window fields back at their defaults: a capture that no longer runs must
    // not keep gating other sources (a controller) behind a focus it no longer observes.
    publish_capture(true, false);
    published_.reset();
}

void DesktopCaptureCore::activate() {
    // The engine still sees the window unfocused: whatever is held now is held from before the
    // activation and needs a release and a new press.
    publish_capture(false, false);
    const auto pressed = platform_.pressed();
    const uint64_t time = platform_.now_ns();
    for (uint32_t usage : pressed.keys)
        if (captured_key(usage) && keys_.insert(usage).second)
            sink_.push_event({{InputSource::Keyboard, usage}, true, time, 0});
    for (const auto& button : pressed.buttons)
        if (buttons_.insert(button).second)
            sink_.push_event({{InputSource::Mouse, button.first}, true, time, button.second});
}

void DesktopCaptureCore::refresh() {
    const bool want = requested_.load(std::memory_order_acquire);
    if (want && !installed_) install();
    else if (!want && installed_) uninstall();

    const bool ready = installed_ && platform_.window_attached() && platform_.window_focused() &&
                       !platform_.input_blocked();
    if (!ready && ready_) {
        // Presses observed from here on are stale; the engine releases on focus loss and this
        // capture forgets what it held, so later host releases are not needed.
        ready_ = false;
        gate_.publish(false, false);
        publish_capture(false, false);
        release_held();
    } else if (ready && !ready_) {
        activate();
    }
    ready_ = ready;

    const bool mouse = mouse_requested_.load(std::memory_order_acquire);
    const bool intentional = mouse != mouse_mode_;
    mouse_mode_ = mouse;
    const bool raw = installed_ && platform_.raw_mouse_available();
    raw_.store(raw, std::memory_order_release);
    apply_relative(ready && raw && mouse);
    gate_.publish(ready, relative_, intentional);
    if (installed_) publish_capture(ready, relative_);
}

void DesktopCaptureCore::key(uint32_t usage, bool down, uint64_t time_ns, DesktopCaptureGate::Stamp stamp) {
    if (!installed_ || !captured_key(usage)) return;
    if (down) {
        // A press counts only when it was observed while ready; a repeated press of a held key
        // is not a new press.
        if (!ready_ || !gate_.accepts(stamp) || !keys_.insert(usage).second) return;
    } else if (!keys_.erase(usage)) {
        return;  // Releases clear only keys this capture reported as held.
    }
    sink_.push_event({{InputSource::Keyboard, usage}, down, time_ns, 0});
}

void DesktopCaptureCore::mouse_button(uint32_t index, uint64_t device, bool down, uint64_t time_ns,
                                      DesktopCaptureGate::Stamp stamp) {
    if (!installed_) return;
    const Button button{index, device};
    if (down) {
        if (!ready_ || !gate_.accepts(stamp) || !buttons_.insert(button).second) return;
    } else if (!buttons_.erase(button)) {
        return;
    }
    sink_.push_event({{InputSource::Mouse, index}, down, time_ns, device});
}

void DesktopCaptureCore::mouse_motion(double dx, double dy, DesktopCaptureGate::Stamp stamp) {
    if (!installed_ || !ready_ || !relative_ || !gate_.accepts(stamp, true)) return;
    try {
        sink_.push_relative_mouse(dx, dy);
    } catch (...) {
        sink_.release_source(InputSource::Mouse);
    }
}

void DesktopCaptureCore::interrupt() {
    gate_.invalidate();
    if (ready_) {
        ready_ = false;
        gate_.publish(false, false);
    }
    if (installed_) publish_capture(false, false);
    release_held();
}

void DesktopCaptureCore::window_changed() {
    apply_relative(false);
    needs_recapture_ = false;
    interrupt();
}

}  // namespace mag
