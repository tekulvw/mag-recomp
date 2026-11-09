// Portable desktop capture core against a fake window system and a fake engine that applies the
// engine's activation rule (a press while unfocused, or held across focus loss, is blocked until
// released). Synthetic input only.
#include "player_desktop_capture_core.h"
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace mag;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct FakePlatform final : DesktopCapturePlatform {
    bool attached = true, focused = true, blocked = false, raw = true, relative_ok = true, relative = false;
    int acquisitions = 0, releases = 0;
    DesktopPressedState held;
    uint64_t time = 1;
    bool window_attached() override { return attached; }
    bool window_focused() override { return focused; }
    bool input_blocked() override { return blocked; }
    bool raw_mouse_available() override { return raw; }
    // Runs inside set_relative(true): a backend may observe input while it acquires (the
    // pointer warping to the centre).
    std::function<void()> during_acquire;
    bool set_relative(bool on) override {
        if (on && during_acquire) during_acquire();
        if (!on) {
            ++releases;
            relative = false;
            return true;
        }
        ++acquisitions;
        if (!relative_ok) return false;
        relative = true;
        return true;
    }
    DesktopPressedState pressed() override { return held; }
    uint64_t now_ns() override { return ++time; }
};

struct FakeEngine final : DesktopCaptureSink {
    bool focused = true, mouse = false;  // the engine's defaults
    int capture_writes = 0, pushes = 0;
    std::set<PhysicalControl> down, blocked;
    std::set<PhysicalControl> active;  // accepted presses not yet released
    double dx = 0, dy = 0;
    void push_event(const PhysicalInputEvent& event) override {
        ++pushes;
        const auto control = event.control;
        if (event.down) down.insert(control);
        else down.erase(control), active.erase(control);
        if (blocked.contains(control)) {
            if (!event.down) blocked.erase(control);
            return;
        }
        if (!focused) {
            if (event.down) blocked.insert(control);
            return;
        }
        if (event.down) active.insert(control);
    }
    void push_relative_mouse(double x, double y) override {
        if (focused && mouse) dx += x, dy += y;
    }
    void set_window_capture(bool f, bool m) override {
        ++capture_writes;
        if (focused && !f) {
            for (auto control : down) blocked.insert(control);
            active.clear();
        }
        focused = f;
        mouse = m;
    }
    void release_source(InputSource source) override {
        for (auto control : down)
            if (control.source == source) blocked.insert(control), active.erase(control);
    }
};

constexpr PhysicalControl W{InputSource::Keyboard, 26}, D{InputSource::Keyboard, 7};
constexpr PhysicalControl Left{InputSource::Mouse, 0}, Right{InputSource::Mouse, 1};

struct Fixture {
    FakePlatform platform;
    FakeEngine engine;
    DesktopCaptureCore core{platform, engine};
    Fixture() {
        core.request_enabled(true);
        core.refresh();
        require(core.installed() && core.ready() && core.relative(), "capture becomes ready and relative");
        require(engine.focused && engine.mouse && platform.relative, "engine and window see the capture");
    }
    void key(PhysicalControl control, bool down) { core.key(control.code, down, platform.now_ns(), core.stamp()); }
    void button(PhysicalControl control, bool down) {
        core.mouse_button(control.code, 7, down, platform.now_ns(), core.stamp());
    }
};

void focus_loss_releases() {
    Fixture f;
    f.key(W, true);
    f.button(Left, true);
    require(f.engine.active.contains(W) && f.engine.active.contains(Left), "presses while ready act");
    f.platform.focused = false;
    f.core.refresh();
    require(!f.core.ready() && !f.engine.focused && !f.engine.mouse, "focus loss reaches the engine");
    require(f.engine.down.empty() && f.engine.active.empty(), "focus loss releases held keys and buttons");
    require(!f.platform.relative, "focus loss gives the cursor back");
    const int pushes = f.engine.pushes;
    f.key(W, false);
    f.button(Left, false);
    f.key(D, true);
    require(f.engine.pushes == pushes, "host releases after focus loss and presses while unfocused are dropped");
    // Alt-Tab: Alt goes down on the game window, focus leaves, Alt comes up elsewhere.
    f.platform.focused = true;
    f.core.refresh();
    const PhysicalControl alt{InputSource::Keyboard, 226};
    f.key(alt, true);
    f.platform.focused = false;
    f.core.refresh();
    f.platform.focused = true;
    f.core.refresh();
    require(!f.engine.down.contains(alt) && f.engine.active.empty(), "no key stays stuck across Alt-Tab");
    // A dialog (on-screen keyboard) owning input counts as focus loss.
    f.key(W, true);
    f.platform.blocked = true;
    f.core.refresh();
    require(!f.core.ready() && f.engine.down.empty(), "a dialog takes the keyboard like focus loss");
}

void fresh_activation() {
    Fixture f;
    const auto before = f.core.stamp();
    f.platform.focused = false;
    f.core.refresh();
    f.platform.held.keys = {26, 227};  // W and the left GUI key are still held
    f.platform.held.buttons = {{0, 7}};
    f.platform.focused = true;
    f.core.refresh();
    require(f.core.ready() && f.engine.focused, "focus returns");
    require(f.engine.down.contains(W) && f.engine.down.contains(Left), "held controls are reported on activation");
    require(f.engine.active.empty(), "controls held across focus changes need a fresh press");
    require(!f.engine.down.contains({InputSource::Keyboard, 227}), "system shortcut keys are never captured");
    const int pushes = f.engine.pushes;
    f.key(W, true);
    require(f.engine.pushes == pushes, "a repeated press of a held key is not a new press");
    f.core.key(D.code, true, f.platform.now_ns(), before);
    require(!f.engine.down.contains(D), "a press observed before activation is ignored");
    f.key(W, false);
    f.button(Left, false);
    f.key(W, true);
    f.button(Left, true);
    require(f.engine.active.contains(W) && f.engine.active.contains(Left), "a fresh press after release acts");
}

void relative_motion() {
    Fixture f;
    f.core.mouse_motion(3, 4, f.core.stamp());
    require(f.engine.dx == 3 && f.engine.dy == 4, "motion while ready and relative reaches the engine");
    const auto stale = f.core.stamp();
    f.platform.focused = false;
    f.core.refresh();
    f.core.mouse_motion(5, 5, f.core.stamp());
    f.platform.focused = true;
    f.core.refresh();
    f.core.mouse_motion(5, 5, stale);
    require(f.engine.dx == 3, "motion is ignored while unfocused or from before the readiness change");
    f.core.request_mouse(false);
    f.core.refresh();
    require(!f.core.relative() && !f.platform.relative && f.engine.focused && !f.engine.mouse, "menus release relative mode");
    f.core.mouse_motion(1, 1, f.core.stamp());
    require(f.engine.dx == 3, "no motion without relative mode");
    f.core.request_mouse(true);
    f.core.refresh();
    require(f.core.relative() && f.engine.mouse, "gameplay takes relative mode again");
    f.platform.raw = false;
    f.core.refresh();
    require(!f.core.relative() && !f.core.raw_mouse_available(), "no relative mode without raw motion");
}

void uninstall_releases() {
    Fixture f;
    f.key(W, true);
    f.button(Right, true);
    f.core.request_enabled(false);
    f.core.refresh();
    require(!f.core.installed() && !f.core.ready() && !f.platform.relative, "uninstall drops capture and the cursor");
    require(f.engine.down.empty() && f.engine.active.empty(), "uninstall releases everything held");
    require(f.engine.focused && !f.engine.mouse, "uninstall hands the engine's window fields back at their defaults");
    require(!f.core.raw_mouse_available(), "no raw motion is offered while uninstalled");
    const int writes = f.engine.capture_writes, pushes = f.engine.pushes, acquisitions = f.platform.acquisitions;
    f.key(W, true);
    f.core.mouse_motion(1, 1, f.core.stamp());
    f.platform.focused = false;
    f.core.refresh();
    f.core.window_changed();
    f.core.refresh();
    require(f.engine.capture_writes == writes && f.engine.pushes == pushes && f.platform.acquisitions == acquisitions,
            "an uninstalled capture touches neither the engine nor the window");
    f.platform.focused = true;
    f.platform.held.keys = {26};
    f.core.request_enabled(true);
    f.core.refresh();
    require(f.core.ready() && f.engine.down.contains(W) && !f.engine.active.contains(W),
            "reinstalling is a fresh activation");
}

void never_installed_is_inert() {
    FakePlatform platform;
    FakeEngine engine;
    DesktopCaptureCore core{platform, engine};
    for (int i = 0; i < 3; ++i) {
        core.refresh();
        core.key(W.code, true, 1, core.stamp());
        core.mouse_motion(1, 1, core.stamp());
        core.request_recapture();
        core.window_changed();
        core.interrupt();
    }
    require(engine.capture_writes == 0 && engine.pushes == 0 && platform.acquisitions == 0 && platform.releases == 0,
            "without a request the capture does nothing");
}

void mouse_mode_keeps_buttons() {
    Fixture f;
    f.key(W, true);
    f.button(Left, true);
    const auto receipt = f.core.stamp();
    f.core.request_mouse(false);
    f.core.refresh();
    require(f.engine.active.contains(W) && f.engine.active.contains(Left), "a mouse-mode change keeps held keys and buttons");
    f.core.key(D.code, true, f.platform.now_ns(), receipt);
    require(f.engine.active.contains(D), "button receipts survive an intentional mouse-mode change");
    f.core.request_mouse(true);
    f.core.refresh();
    f.core.mouse_motion(2, 2, receipt);
    require(f.engine.dx == 0, "motion receipts do not survive a mouse-mode change");
    require(f.engine.active.contains(Left), "and toggling back keeps them too");
}

void recapture() {
    Fixture f;
    f.core.refresh();
    require(f.platform.acquisitions == 1, "relative mode is acquired once");
    f.core.request_recapture();
    f.core.refresh();
    require(f.platform.acquisitions == 2 && f.platform.relative, "a click or focus return re-applies relative mode");
    f.platform.relative_ok = false;
    f.core.request_recapture();
    f.core.refresh();
    require(!f.core.relative() && !f.platform.relative && !f.engine.mouse && f.engine.focused,
            "a refused acquisition leaves the cursor free and keeps keys");
    f.platform.relative_ok = true;
    f.core.window_changed();
    require(!f.core.ready() && !f.engine.focused, "a window change requires a fresh activation");
    f.core.refresh();
    require(f.core.ready() && f.core.relative(), "the new window is captured");
}

void quit_key_rule() {
    DesktopQuitKeyContext held{true, true, true, true, true, false};
    require(desktop_capture_withholds_quit_key(held), "Escape is withheld while the adapter owns the focused game window");
    auto check = [&](bool DesktopQuitKeyContext::*field, bool value, const char* message) {
        auto context = held;
        context.*field = value;
        require(!desktop_capture_withholds_quit_key(context), message);
    };
    check(&DesktopQuitKeyContext::quit_key_down, false, "only the quit key press is withheld");
    check(&DesktopQuitKeyContext::keyboard_owned, false, "the SDK keeps Escape while it owns the keyboard");
    check(&DesktopQuitKeyContext::capture_requested, false, "the SDK keeps Escape without desktop capture");
    check(&DesktopQuitKeyContext::on_game_window, false, "Escape on another window is not withheld");
    check(&DesktopQuitKeyContext::game_window_focused, false, "Escape is not withheld without game focus");
    check(&DesktopQuitKeyContext::dialog_active, true, "a dialog always receives Escape");
}

// The mouse lock is engaged while ready, released on focus loss and in menus, engaged again on
// return, and re-applied when the backend reports it lost. Each state change has one log line.
void mouse_lock() {
    Fixture f;  // engaged: ready and relative
    require(f.platform.acquisitions == 1 && f.platform.relative, "capture engages the lock once");
    auto step = [&](auto change) {
        const bool ready = f.core.ready(), relative = f.core.relative();
        change();
        f.core.refresh();
        return desktop_capture_transition(ready, relative, f.core.ready(), f.core.relative());
    };
    const char* line = step([&] { f.platform.focused = false; });
    require(!f.platform.relative && f.platform.releases == 1, "focus loss releases the lock");
    require(line && std::string(line).find("capture released") == 0, "focus loss is logged");
    line = step([&] { f.platform.focused = true; });
    require(f.platform.relative && f.platform.acquisitions == 2, "focus return engages the lock again");
    require(line && std::string(line) == "capture engaged; mouse locked to the game window", "re-engagement is logged");
    line = step([&] { f.core.request_mouse(false); });
    require(f.core.ready() && !f.platform.relative && f.platform.releases == 2, "a menu releases only the lock");
    require(line && std::string(line).find("mouse released") == 0, "the menu release is logged");
    line = step([&] { f.core.request_mouse(true); });
    require(f.platform.relative && f.platform.acquisitions == 3, "leaving the menu locks the mouse again");
    require(line && std::string(line) == "mouse locked to the game window", "the menu exit is logged");
    // The backend found the lock gone (the pointer left the window): it asks for a recapture.
    f.platform.relative = false;
    line = step([&] { f.core.request_recapture(); });
    require(f.platform.relative && f.platform.acquisitions == 4 && f.core.relative(), "a lost lock is re-applied");
    require(!line, "re-applying an already engaged lock adds no state-change line");
    line = step([] {});
    require(!line && f.platform.acquisitions == 4, "a held lock is not re-applied every refresh");
}

// Re-applying a lost lock warps the pointer; motion observed before or during the re-acquisition
// is discarded, motion after it counts, and held buttons survive.
void reacquire_discards_warp_motion() {
    Fixture f;
    f.button(Left, true);
    DesktopCaptureGate::Stamp stale = f.core.stamp(), during{};
    f.platform.during_acquire = [&] { during = f.core.stamp(); };
    f.core.request_recapture();
    f.core.refresh();
    require(f.platform.acquisitions == 2 && f.core.relative(), "the lock is re-applied");
    f.core.mouse_motion(300, -40, during);
    f.core.mouse_motion(7, 7, stale);
    require(f.engine.dx == 0 && f.engine.dy == 0, "motion from the re-lock's warp became look input");
    f.core.mouse_motion(2, 1, f.core.stamp());
    require(f.engine.dx == 2 && f.engine.dy == 1, "motion after the re-lock reaches the engine");
    require(f.engine.active.contains(Left), "a held button survives the re-lock");
    f.core.mouse_button(Left.code, 7, false, f.platform.now_ns(), stale);
    require(!f.engine.active.contains(Left), "its release still ends it");
}

// Clicks while the lock is held (firing, aiming) do not re-acquire it, so look motion around a
// shot is kept; a click while the cursor is free, or after the lock was lost, takes it back.
void click_recapture() {
    Fixture f;
    require(f.platform.acquisitions == 1, "capture engages the lock once");
    for (int shot = 0; shot < 5; ++shot) {
        const auto before = f.core.stamp();
        f.core.recapture_after_click(true);
        f.core.refresh();
        f.core.mouse_motion(3, 1, before);
    }
    require(f.platform.acquisitions == 1, "a click re-acquired a held lock");
    require(f.engine.dx == 15 && f.engine.dy == 5, "look motion around clicks was discarded");
    f.core.recapture_after_click(false);  // the backend found the lock gone
    f.core.refresh();
    require(f.platform.acquisitions == 2, "a click after the lock was lost did not take it back");
    f.core.request_mouse(false);  // a menu: the cursor is free
    f.core.refresh();
    f.core.request_mouse(true);
    f.platform.relative_ok = false;
    f.core.refresh();
    require(!f.core.relative(), "the lock was refused");
    f.platform.relative_ok = true;
    f.core.recapture_after_click(true);
    f.core.refresh();
    require(f.core.relative(), "a click while the cursor is free takes it back");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string which = argv[1];
        if (which == "focus") focus_loss_releases();
        else if (which == "fresh") fresh_activation();
        else if (which == "motion") relative_motion();
        else if (which == "uninstall") uninstall_releases();
        else if (which == "inert") never_installed_is_inert();
        else if (which == "mouse-mode") mouse_mode_keeps_buttons();
        else if (which == "recapture") recapture();
        else if (which == "quit-key") quit_key_rule();
        else if (which == "mouse-lock") mouse_lock();
        else if (which == "reacquire") reacquire_discards_warp_motion();
        else if (which == "click") click_recapture();
        else return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
