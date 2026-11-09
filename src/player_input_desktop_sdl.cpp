// Keyboard/mouse desktop capture for the SDL2 game window (Linux), on the portable capture core
// (player_desktop_capture_core.h). Inert until the controls adapter enables capture.
//
// Seams. The SDK's window pump polls SDL events and then takes its keyboard-pad sample on the
// window thread; that sample is redirected (player_input_sdl_keyboard.c) to call
// mag_input_desktop_sdl_pump first, with the game window and whether a native dialog owns the
// keyboard. While capture is requested an SDL event filter observes keyboard, mouse and window
// events as SDL queues them, before the SDK's pump reads them. The filter passes every event on,
// except one: a press of Escape on the focused game window while the adapter owns the keyboard
// and captures it, because the pump would treat it as quit and the controls bind it to Start.
// The filter queues what it observed; the pump seam hands the queue to the core and refreshes it,
// so the core only ever runs on the window thread.
//
// Mapping. SDL scancodes 4..231 are USB HID keyboard usages. Mouse buttons: left 0, right 1,
// middle 2, X1 3, X2 4. Relative motion uses SDL relative mouse mode; dy is reported up-positive.
// Touch-synthesized mouse events are ignored. SDL presents one logical mouse (device 1 here).
#include "player_input_desktop.h"
#include "player_input_desktop_sdl.h"
#include "player_desktop_capture_core.h"
#include "player_input_capture.h"
#include "player_settings_runtime.h"
#include <SDL2/SDL.h>
#include <atomic>
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
};

class SdlPlatform final : public DesktopCapturePlatform {
public:
    SDL_Window* window = nullptr;
    bool dialog = false, relative_refused = false;
    bool window_attached() override { return window != nullptr; }
    bool window_focused() override { return window && SDL_GetKeyboardFocus() == window; }
    bool input_blocked() override { return dialog || mag_player_input_blocked(); }
    bool raw_mouse_available() override { return window && !relative_refused; }
    bool set_relative(bool on) override {
        if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0) return true;
        if (on) {
            // Not available here (for example a compositor without relative pointer support):
            // report it and stop asking until the window changes.
            relative_refused = true;
            std::fprintf(stderr, "[game-controls] SDL relative mouse mode unavailable: %s\n", SDL_GetError());
        }
        return false;
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
    // Filter state: written on the window thread before SDL_SetEventFilter, which serializes it
    // with filter calls.
    bool filter_installed = false;
    SDL_EventFilter prior = nullptr;
    void* prior_data = nullptr;
    std::mutex queue_mutex;
    std::vector<Observed> queue;
    bool overflow = false;
    int reported = -1;
};

// Never destroyed: SDL may call the filter until the process ends.
Backend& backend() {
    static Backend* state = new Backend;
    return *state;
}

bool observed_type(Uint32 type) {
    switch (type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION: case SDL_WINDOWEVENT:
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
            context.keyboard_owned = mag_input_capture_keyboard_owned() != 0;
            context.capture_requested = b.core.enabled_requested();
            context.on_game_window = id != 0 && event->key.windowID == id;
            context.game_window_focused = game && SDL_GetKeyboardFocus() == game;
            context.dialog_active = b.dialog.load(std::memory_order_acquire);
            keep = !desktop_capture_withholds_quit_key(context);
        }
        const bool repeat = (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) && event->key.repeat;
        if (!repeat) {
            const auto stamp = b.core.stamp();
            const uint64_t time = steady_ns();
            std::lock_guard lock(b.queue_mutex);
            if (b.queue.size() < kQueueLimit) b.queue.push_back({*event, stamp, time});
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
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.windowID == id) b.core.request_recapture();
        b.core.mouse_button(sdl_desktop_mouse_index(e.button.button), kMouseDevice, e.type == SDL_MOUSEBUTTONDOWN,
                            item.time_ns, item.stamp);
        break;
    case SDL_MOUSEMOTION:
        if (e.motion.which == SDL_TOUCH_MOUSEID || e.motion.windowID != id) break;
        b.core.mouse_motion(double(e.motion.xrel), -double(e.motion.yrel), item.stamp);
        break;
    case SDL_WINDOWEVENT:
        if (e.window.windowID == id && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) b.core.request_recapture();
        break;
    default:
        break;
    }
}

void pump(Backend& b, SDL_Window* window, bool dialog) {
    b.dialog.store(dialog, std::memory_order_release);
    b.platform.dialog = dialog;
    if (window != b.platform.window) {
        set_filter(b, false);
        b.core.window_changed();
        b.platform.window = window;
        b.platform.relative_refused = false;
        b.window.store(window, std::memory_order_release);
        b.window_id.store(window ? SDL_GetWindowID(window) : 0, std::memory_order_release);
    }
    set_filter(b, window && b.core.enabled_requested());
    std::vector<Observed> events;
    bool overflow = false;
    {
        std::lock_guard lock(b.queue_mutex);
        events.swap(b.queue);
        overflow = std::exchange(b.overflow, false);
    }
    if (overflow) b.core.interrupt();
    else for (const auto& item : events) deliver(b, item);
    b.core.refresh();
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
    default: return sdl_button ? uint32_t(sdl_button) - 1 : 0;
    }
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
extern "C" void mag_input_desktop_set_mouse_enabled(int enabled) { backend().core.request_mouse(enabled != 0); }
// Requests apply at the window's next pump, every frame.
extern "C" void mag_input_desktop_refresh(void) {}
extern "C" int mag_input_desktop_raw_mouse_available(void) { return backend().core.raw_mouse_available(); }
