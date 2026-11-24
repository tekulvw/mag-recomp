// Keyboard/mouse desktop capture for the SDL2 game window (Linux), on the portable capture core
// (player_desktop_capture_core.h). Observes nothing until the controls adapter enables capture;
// from the moment the game window attaches it only keeps Escape from quitting it.
//
// Seams. The SDK's window pump polls SDL events and then takes its keyboard-pad sample on the
// window thread; that sample is redirected (player_input_sdl_keyboard.c) to call
// mag_input_desktop_sdl_pump first, with the game window and whether a native dialog owns the
// keyboard. An SDL event filter sits in front of the SDK's pump for as long as the game window is
// attached. It passes every event on, except one: a press of Escape on the focused game window
// while no dialog owns the keyboard, because the pump would end the game on it (players press it to
// go back or pause; in a match the controls bind it to Start). Closing the window still quits.
// While capture is requested the filter also observes keyboard, mouse and window events as SDL
// queues them.
// The filter queues what it observed; the pump seam hands the queue to the core and refreshes it,
// so the core only ever runs on the window thread.
//
// Mapping. SDL scancodes 4..231 are USB HID keyboard usages. Mouse buttons: left 0, right 1,
// middle 2, X1 3, X2 4. Relative motion uses SDL relative mouse mode; dy is reported down-positive.
// Touch-synthesized mouse events are ignored. SDL presents one logical mouse (device 1 here).
#include "player_input_desktop.h"
#include "player_input_desktop_sdl.h"
#include "player_desktop_capture_core.h"
#include "player_input_capture.h"
#include "player_settings_runtime.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace mag {
// Exposed for the synthetic SDL fixture; the backend uses them the same way.
uint32_t sdl_desktop_mouse_index(uint8_t sdl_button);
}  // namespace mag

namespace {
using namespace mag;
constexpr uint64_t kMouseDevice = 1;
constexpr size_t kQueueLimit = 4096;

uint64_t steady_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

struct Observed {
    SDL_Event event;
    DesktopCaptureGate::Stamp stamp;
    uint64_t time_ns;
    bool warp = false;  // motion SDL reported for the backend's own centring warp
};

// Opt-in diagnostics (MAG_GAME_CONTROLS_TRACE=1, any value but empty or 0, as on Windows): each
// acquisition, release or failure of the mouse lock; after each acquisition the next motion events
// with their SDL deltas, whether they reached the controls, and the epochs; and about once a second
// what became of the motion SDL reported, with the same counts as the Win32 backend.
constexpr int kTraceMotions = 20;
bool trace_enabled() {
    const char* value = std::getenv("MAG_GAME_CONTROLS_TRACE");
    return value && *value && std::strcmp(value, "0");
}
struct MotionTrace {
    bool on = trace_enabled();
    std::chrono::steady_clock::time_point next{};
    unsigned reports{}, accepted{}, other_window{}, not_installed{}, not_ready{}, not_relative{}, stale{},
        lock_lost{}, mouse_requests_on{}, mouse_requests_off{};
};
// Window thread only.
MotionTrace& motion_trace() {
    static MotionTrace trace;
    return trace;
}
// Mouse-look requests come from the adapter's thread.
std::atomic<unsigned> g_mouse_requests_on{0}, g_mouse_requests_off{0};

class SdlPlatform final : public DesktopCapturePlatform {
public:
    SDL_Window* window = nullptr;
    bool dialog = false, relative_refused = false;
    bool window_attached() override { return window != nullptr; }
    bool window_focused() override { return window && SDL_GetKeyboardFocus() == window; }
    bool input_blocked() override { return dialog || mag_player_input_blocked(); }
    bool raw_mouse_available() override { return window && !relative_refused; }
    // Mouse look holds two things while captured: SDL relative mode (hidden cursor, relative
    // motion, pointer lock where the platform has one) and an explicit window grab that confines
    // the pointer to the game window, so a long or fast swipe cannot carry it onto another
    // window even where relative mode alone does not lock it.
    bool set_relative(bool on) override {
        if (!on) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
            if (window) SDL_SetWindowGrab(window, SDL_FALSE);
            if (motion_trace().on) std::fprintf(stderr, "[game-controls] trace lock released\n");
            return true;
        }
        // SDL ignores warps while relative mode is on, so (re)acquire in this order: leave
        // relative mode, confine, put the pointer in the middle of the window, then lock it.
        SDL_SetRelativeMouseMode(SDL_FALSE);
        if (window) {
            SDL_SetWindowGrab(window, SDL_TRUE);
            center_pointer();
        }
        if (SDL_SetRelativeMouseMode(SDL_TRUE) == 0) {
            if (motion_trace().on) {
                int x = 0, y = 0, w = 0, h = 0;
                if (window) SDL_GetWindowPosition(window, &x, &y), SDL_GetWindowSize(window, &w, &h);
                const char* driver = SDL_GetCurrentVideoDriver();
                std::fprintf(stderr, "[game-controls] trace lock acquired: window %dx%d at %d,%d (driver %s) grab=%d\n", w, h,
                             x, y, driver ? driver : "?", window && SDL_GetWindowGrab(window) ? 1 : 0);
            }
            tracing = trace_enabled();
            if (tracing) {
                trace_left = kTraceMotions;
                std::fprintf(stderr, "[game-controls] trace mouse lock acquired; tracing the next %d motions\n", kTraceMotions);
            }
            return true;
        }
        if (window) SDL_SetWindowGrab(window, SDL_FALSE);
        if (motion_trace().on) std::fprintf(stderr, "[game-controls] trace lock failed: %s\n", SDL_GetError());
        // Not available here (for example a compositor without relative pointer support):
        // report it and stop asking until the window changes.
        relative_refused = true;
        std::fprintf(stderr, "[game-controls] SDL relative mouse mode unavailable: %s\n", SDL_GetError());
        return false;
    }
    // Motion SDL reports for this warp is the pointer jumping to the centre, not the player's
    // hand: the event filter drops motion while this is set (SDL delivers it synchronously on this
    // thread). Later X motion for the warp arrives after relative mode is back on and SDL ignores
    // it there.
    std::atomic<bool> warping{false};
    std::atomic<bool> tracing{false};  // also read by the event filter
    int trace_left = 0;
    void center_pointer() {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        if (w <= 0 || h <= 0) return;
        warping.store(true, std::memory_order_release);
        SDL_WarpMouseInWindow(window, w / 2, h / 2);
        warping.store(false, std::memory_order_release);
    }
    // Whether the video driver reports the pointer and the window in screen coordinates; set when
    // the window attaches.
    bool global_pointer = false;
    // While captured: is the lock still in place, with the pointer inside the window?
    bool lock_held() {
        if (!window) return false;
        SdlLockProbe probe;
        probe.relative = SDL_GetRelativeMouseMode();
        probe.grabbed = SDL_GetWindowGrab(window);
        probe.global_pointer = global_pointer;
        SDL_GetWindowSize(window, &probe.width, &probe.height);
        if (global_pointer) {
            SDL_GetGlobalMouseState(&probe.pointer_x, &probe.pointer_y);
            SDL_GetWindowPosition(window, &probe.window_x, &probe.window_y);
        } else {
            SDL_GetMouseState(&probe.pointer_x, &probe.pointer_y);
            const SDL_Window* focus = SDL_GetMouseFocus();
            probe.other_window = focus && focus != window;
        }
        return sdl_desktop_lock_held(probe);
    }
    DesktopPressedState pressed() override {
        DesktopPressedState state;
        int count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&count);
        for (int code = 4; keys && code <= 231 && code < count; ++code)
            if (keys[code]) state.keys.push_back(uint32_t(code));
        const Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
        for (uint8_t button = 1; button <= 32; ++button)
            if (buttons & SDL_BUTTON(button)) state.buttons.emplace_back(sdl_desktop_mouse_index(button), kMouseDevice);
        return state;
    }
    uint64_t now_ns() override { return steady_ns(); }
};

struct Backend {
    SdlPlatform platform;
    DesktopCaptureEngineSink sink;
    DesktopCaptureCore core{platform, sink};
    std::atomic<SDL_Window*> window{nullptr};
    std::atomic<Uint32> window_id{0};
    std::atomic<bool> dialog{false};
    std::atomic<bool> observing{false};  // capture requested: the filter queues what it sees
    // Filter state: written on the window thread before SDL_SetEventFilter, which serializes it
    // with filter calls.
    bool filter_installed = false;
    SDL_EventFilter prior = nullptr;
    void* prior_data = nullptr;
    std::mutex queue_mutex;
    std::vector<Observed> queue;
    bool overflow = false;
    int reported = -1;
    bool lock_lost_reported = false;
};

// Never destroyed: SDL may call the filter until the process ends.
Backend& backend() {
    static Backend* state = new Backend;
    return *state;
}

bool observed_type(Uint32 type) {
    switch (type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION: case SDL_MOUSEWHEEL: case SDL_WINDOWEVENT:
        return true;
    default:
        return false;
    }
}

int SDLCALL filter(void* data, SDL_Event* event) {
    auto& b = *static_cast<Backend*>(data);
    bool keep = true;
    if (event && observed_type(event->type)) {
        if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE) {
            SDL_Window* game = b.window.load(std::memory_order_acquire);
            const Uint32 id = b.window_id.load(std::memory_order_acquire);
            DesktopQuitKeyContext context;
            context.quit_key_down = true;
            context.on_game_window = id != 0 && event->key.windowID == id;
            context.game_window_focused = game && SDL_GetKeyboardFocus() == game;
            context.dialog_active = b.dialog.load(std::memory_order_acquire);
            keep = !desktop_capture_withholds_quit_key(context);
        }
        const bool observing = b.observing.load(std::memory_order_acquire);
        const bool repeat = (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) && event->key.repeat;
        const bool warp = event->type == SDL_MOUSEMOTION && b.platform.warping.load(std::memory_order_acquire);
        // Warp motion is queued only to be traced; delivery never passes it to the controls.
        if (observing && !repeat && (!warp || b.platform.tracing)) {
            const auto stamp = b.core.stamp();
            const uint64_t time = steady_ns();
            std::lock_guard lock(b.queue_mutex);
            if (b.queue.size() < kQueueLimit) b.queue.push_back({*event, stamp, time, warp});
            else b.overflow = true;
        }
    }
    if (!keep) return 0;
    return b.prior ? b.prior(b.prior_data, event) : 1;
}

void set_filter(Backend& b, bool on) {
    if (on == b.filter_installed) return;
    if (on) {
        SDL_EventFilter prior = nullptr;
        void* prior_data = nullptr;
        if (!SDL_GetEventFilter(&prior, &prior_data)) prior = nullptr;
        b.prior = prior == filter ? nullptr : prior;
        b.prior_data = b.prior ? prior_data : nullptr;
        SDL_SetEventFilter(filter, &b);
    } else {
        SDL_EventFilter current = nullptr;
        void* current_data = nullptr;
        // Leave a filter someone installed after this one in place.
        if (SDL_GetEventFilter(&current, &current_data) && current == filter) SDL_SetEventFilter(b.prior, b.prior_data);
        std::lock_guard lock(b.queue_mutex);
        b.queue.clear();
        b.overflow = false;
    }
    b.filter_installed = on;
}

void deliver(Backend& b, const Observed& item) {
    const SDL_Event& e = item.event;
    const Uint32 id = b.window_id.load(std::memory_order_relaxed);
    switch (e.type) {
    case SDL_KEYDOWN: case SDL_KEYUP:
        b.core.key(uint32_t(e.key.keysym.scancode), e.type == SDL_KEYDOWN, item.time_ns, item.stamp);
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        if (e.button.which == SDL_TOUCH_MOUSEID) break;
        // A click in the game window takes the cursor back (click-to-recapture).
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.windowID == id) b.core.recapture_after_click(b.platform.lock_held());
        b.core.mouse_button(sdl_desktop_mouse_index(e.button.button), kMouseDevice, e.type == SDL_MOUSEBUTTONDOWN,
                            item.time_ns, item.stamp);
        break;
    case SDL_MOUSEWHEEL: {
        // Whole steps, up positive; a "flipped" (natural scrolling) report is turned back to the
        // wheel's own direction so wheel up is the same physical control on every host.
        // Some SDL builds (sdl2-compat) report fractional scrolling in preciseY only: whole steps
        // are then taken from its running total.
        if (e.wheel.which == SDL_TOUCH_MOUSEID || e.wheel.windowID != id) break;
        int steps = e.wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        static float precise = 0;
        if (steps) precise = 0;
        else if (e.wheel.preciseY != 0) {
            precise += e.wheel.preciseY;
            steps = int(precise);
            precise -= float(steps);
        }
#endif
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) steps = -steps;
        b.core.mouse_wheel(steps, kMouseDevice, item.time_ns, item.stamp);
        break;
    }
    case SDL_MOUSEMOTION: {
        const bool foreign = e.motion.which == SDL_TOUCH_MOUSEID || e.motion.windowID != id;
        const bool accepted = !foreign && !item.warp &&
                              b.core.mouse_motion(double(e.motion.xrel), double(e.motion.yrel), item.stamp);
        // The centring warp is never look input; SDL's zero-length motion (leaving relative mode)
        // is no motion at all.
        if (!item.warp && (e.motion.xrel || e.motion.yrel)) {
            auto& trace = motion_trace();
            ++trace.reports;
            if (accepted) ++trace.accepted;
            else if (foreign) ++trace.other_window;
            else if (!b.core.installed()) ++trace.not_installed;
            else if (!b.core.ready()) ++trace.not_ready;
            else if (!b.core.relative()) ++trace.not_relative;
            else ++trace.stale;
        }
        if (b.platform.trace_left > 0) {
            --b.platform.trace_left;
            std::fprintf(stderr, "[game-controls] trace motion xrel=%d yrel=%d %s epoch=%u current=%u\n",
                         int(e.motion.xrel), int(e.motion.yrel),
                         accepted ? "accepted" : item.warp ? "dropped(warp)" : foreign ? "dropped(other window)" : "dropped(stale)",
                         DesktopCaptureCore::motion_epoch(item.stamp), b.core.motion_epoch());
        }
        break;
    }
    case SDL_WINDOWEVENT:
        if (e.window.windowID == id && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) b.core.request_recapture();
        break;
    default:
        break;
    }
}

// About once a second, with MAG_GAME_CONTROLS_TRACE: what became of the motion SDL reported since
// the last line (the counts of the Win32 backend, plus motion for another window), the lock losses
// found and the mouse-look requests the adapter made. Silent while nothing happened.
void trace_summary(Backend& b) {
    auto& trace = motion_trace();
    const auto now = std::chrono::steady_clock::now();
    if (now < trace.next) return;
    trace.mouse_requests_on = g_mouse_requests_on.exchange(0, std::memory_order_relaxed);
    trace.mouse_requests_off = g_mouse_requests_off.exchange(0, std::memory_order_relaxed);
    if (trace.on && (trace.reports || trace.lock_lost || trace.mouse_requests_on || trace.mouse_requests_off))
        std::fprintf(stderr,
                     "[game-controls] trace motion: reports=%u accepted=%u other_window=%u not_installed=%u "
                     "not_ready=%u not_relative=%u stale_epoch=%u lock_lost=%u mouse_requests on/off=%u/%u "
                     "(ready=%d relative=%d)\n",
                     trace.reports, trace.accepted, trace.other_window, trace.not_installed, trace.not_ready,
                     trace.not_relative, trace.stale, trace.lock_lost, trace.mouse_requests_on, trace.mouse_requests_off,
                     b.core.ready(), b.core.relative());
    trace = MotionTrace{};  // reads the variable again, so a running game can be traced from now on
    trace.next = now + std::chrono::seconds(1);
}

void pump(Backend& b, SDL_Window* window, bool dialog) {
    b.dialog.store(dialog, std::memory_order_release);
    b.platform.dialog = dialog;
    if (window != b.platform.window) {
        set_filter(b, false);
        b.core.window_changed();
        b.platform.window = window;
        b.platform.relative_refused = false;
        const char* driver = window ? SDL_GetCurrentVideoDriver() : nullptr;
        b.platform.global_pointer = driver && !std::strcmp(driver, "x11");
        b.window.store(window, std::memory_order_release);
        b.window_id.store(window ? SDL_GetWindowID(window) : 0, std::memory_order_release);
    }
    // The filter stays for as long as the window does (Escape); it observes only while capture is
    // requested, and what it queued before capture stopped is dropped.
    set_filter(b, window != nullptr);
    const bool observing = window && b.core.enabled_requested();
    if (b.observing.exchange(observing, std::memory_order_acq_rel) && !observing) {
        std::lock_guard lock(b.queue_mutex);
        b.queue.clear();
        b.overflow = false;
    }
    std::vector<Observed> events;
    bool overflow = false;
    {
        std::lock_guard lock(b.queue_mutex);
        events.swap(b.queue);
        overflow = std::exchange(b.overflow, false);
    }
    if (overflow) b.core.interrupt();
    else for (const auto& item : events) deliver(b, item);
    // Something else (the window manager, another grab) took the lock or the pointer left the
    // window: put the pointer back and re-apply the lock at this refresh. Logged once per loss.
    if (b.core.relative() && !b.platform.lock_held()) {
        ++motion_trace().lock_lost;
        if (!b.lock_lost_reported) {
            std::fprintf(stderr, "[game-controls] mouse lock lost; re-applying\n");
            b.lock_lost_reported = true;
        }
        b.core.request_recapture();
    } else if (b.core.relative()) {
        b.lock_lost_reported = false;
    }
    const bool was_ready = b.core.ready(), was_relative = b.core.relative();
    b.core.refresh();
    if (const char* change = desktop_capture_transition(was_ready, was_relative, b.core.ready(), b.core.relative()))
        std::fprintf(stderr, "[game-controls] %s\n", change);
    trace_summary(b);
    const int on = b.core.installed() ? 1 : 0;
    if (on != b.reported) {
        if (b.reported >= 0 || on)
            std::fprintf(stderr, "[game-controls] desktop capture %s (SDL window)\n", on ? "on" : "off");
        b.reported = on;
    }
}
}  // namespace

namespace mag {
uint32_t sdl_desktop_mouse_index(uint8_t sdl_button) {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: {
        // Further buttons follow, skipping the wheel's codes.
        const uint32_t code = sdl_button ? uint32_t(sdl_button) - 1 : 0;
        return code >= MouseWheelUp ? code + 2 : code;
    }
    }
}

bool sdl_desktop_lock_held(const SdlLockProbe& p) {
    if (!p.relative || !p.grabbed) return false;
    if (p.width <= 0 || p.height <= 0) return true;
    if (p.global_pointer)
        return p.pointer_x >= p.window_x && p.pointer_y >= p.window_y && p.pointer_x < p.window_x + p.width &&
               p.pointer_y < p.window_y + p.height;
    return !p.other_window && p.pointer_x >= 0 && p.pointer_y >= 0 && p.pointer_x < p.width && p.pointer_y < p.height;
}
}  // namespace mag

extern "C" void mag_input_desktop_sdl_pump(void* sdl_window, int dialog_active) {
    auto& b = backend();
    try {
        pump(b, static_cast<SDL_Window*>(sdl_window), dialog_active != 0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[game-controls] desktop capture error: %s\n", error.what());
        try { b.core.interrupt(); } catch (...) {}
    } catch (...) {
        std::fprintf(stderr, "[game-controls] desktop capture error\n");
        try { b.core.interrupt(); } catch (...) {}
    }
}

// The SDL pump seam supplies the game window; an explicit attachment is not needed here.
extern "C" void mag_input_desktop_attach_window(void*) {}
extern "C" void mag_input_desktop_set_enabled(int enabled) {
    auto& b = backend();
    // Without an SDL game window (a headless run) nothing pumps; say so once instead of waiting silently.
    static std::atomic<bool> waiting_reported{false};
    if (b.core.request_enabled(enabled != 0) && enabled && !b.window.load(std::memory_order_acquire) &&
        !waiting_reported.exchange(true))
        std::fprintf(stderr, "[game-controls] desktop capture requested; waiting for the SDL game window\n");
}
extern "C" void mag_input_desktop_set_mouse_enabled(int enabled) {
    if (backend().core.request_mouse(enabled != 0))
        (enabled ? g_mouse_requests_on : g_mouse_requests_off).fetch_add(1, std::memory_order_relaxed);
}
// Requests apply at the window's next pump, every frame.
extern "C" void mag_input_desktop_refresh(void) {}
extern "C" int mag_input_desktop_raw_mouse_available(void) { return backend().core.raw_mouse_available(); }
