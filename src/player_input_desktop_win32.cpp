// Keyboard/mouse desktop capture for the Win32 game window (Windows), on the portable capture core
// (player_desktop_capture_core.h). Inert until the controls adapter enables capture.
//
// Seams. The SDK's Vulkan and D3D12 backends create the game window and pump its messages on the
// window's own thread; they share that pump with the native on-screen keyboard through three
// calls, which player_input_win32_window.c takes over (attach/detach, once per pump, once per
// message). This backend therefore runs only on the window thread: the per-pump call applies
// requests and readiness, and the per-message call observes the messages the pump reads. It never
// consumes a message the SDK or the on-screen keyboard needs; the one exception is a press of
// Escape on the focused game window while the adapter owns the keyboard and requests capture and
// no dialog owns the keyboard: the window would treat it as quit, and the controls bind it to
// Start.
//
// Input. While capture is requested the window registers for Raw Input from keyboards and mice
// (no other flags: legacy key and mouse messages still flow, and input arrives only while this
// application is in the foreground). Keys are translated from scan codes, with their E0 prefix, to
// USB HID usages (player_input_win32_map.c); auto-repeat is filtered by tracking each key's
// physical state. Mouse buttons map left 0, right 1, middle 2, X1 3, X2 4 on one logical mouse
// (physical buttons; the system's swap setting does not apply). Look motion is the raw relative
// delta, reported up-positive; absolute-position devices do not drive look. Relative mode hides
// the cursor and confines it to the client area; it is re-applied after a click in the window,
// when focus returns and when the confinement was lost (the window moved or something else
// changed it), and released when focus goes.
//
// Controllers stay with cellPad's XInput backend, whose readings also reach the capture layer
// (player_input_win32_pad.c). Keyboard and mouse capture never depends on whether an XInput device
// (real or virtual) is present.
#ifndef _WIN32
#error "the Win32 desktop capture backend builds only for Windows"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "player_desktop_capture_core.h"
#include "player_input_capture.h"
#include "player_input_desktop.h"
#include "player_input_desktop_win32.h"
#include "player_input_win32_map.h"
#include "player_settings_runtime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>

static_assert(RI_KEY_BREAK == MAG_WIN32_KEY_BREAK && RI_KEY_E0 == MAG_WIN32_KEY_E0 && RI_KEY_E1 == MAG_WIN32_KEY_E1);
static_assert(MOUSE_MOVE_ABSOLUTE == MAG_WIN32_MOUSE_MOVE_ABSOLUTE);
static_assert(RI_MOUSE_LEFT_BUTTON_DOWN == MAG_WIN32_MOUSE_LEFT_DOWN && RI_MOUSE_LEFT_BUTTON_UP == MAG_WIN32_MOUSE_LEFT_UP &&
              RI_MOUSE_RIGHT_BUTTON_DOWN == MAG_WIN32_MOUSE_RIGHT_DOWN && RI_MOUSE_RIGHT_BUTTON_UP == MAG_WIN32_MOUSE_RIGHT_UP &&
              RI_MOUSE_MIDDLE_BUTTON_DOWN == MAG_WIN32_MOUSE_MIDDLE_DOWN &&
              RI_MOUSE_MIDDLE_BUTTON_UP == MAG_WIN32_MOUSE_MIDDLE_UP && RI_MOUSE_BUTTON_4_DOWN == MAG_WIN32_MOUSE_X1_DOWN &&
              RI_MOUSE_BUTTON_4_UP == MAG_WIN32_MOUSE_X1_UP && RI_MOUSE_BUTTON_5_DOWN == MAG_WIN32_MOUSE_X2_DOWN &&
              RI_MOUSE_BUTTON_5_UP == MAG_WIN32_MOUSE_X2_UP);

namespace {
using namespace mag;
constexpr uint64_t kMouseDevice = 1;
constexpr USHORT kGenericDesktop = 0x01, kMouseUsage = 0x02, kKeyboardUsage = 0x06;

uint64_t steady_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

bool same_rect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// With Num Lock on, a keypad digit is reported to the system under its own virtual key rather
// than the navigation key its scan code names.
UINT keypad_virtual_key(uint32_t usage) {
    if (usage >= 89 && usage <= 97) return VK_NUMPAD1 + (usage - 89);
    if (usage == 98) return VK_NUMPAD0;
    if (usage == 99) return VK_DECIMAL;
    return 0;
}

bool virtual_key_down(UINT key) { return key && (GetAsyncKeyState(int(key)) & 0x8000) != 0; }

class Win32Platform final : public DesktopCapturePlatform {
public:
    HWND window = nullptr;
    bool dialog = false, registered = false;
    MagWin32KeyState keys{};  // physical key state, to tell presses from auto-repeat

    bool window_attached() override { return window && IsWindow(window); }
    bool window_focused() override {
        return window_attached() && GetForegroundWindow() == window && GetActiveWindow() == window &&
               !IsIconic(window);
    }
    bool input_blocked() override { return dialog || mag_player_input_blocked(); }
    bool raw_mouse_available() override { return registered && window_attached(); }
    bool set_relative(bool on) override {
        if (!on) {
            release_cursor();
            return true;
        }
        RECT rect;
        if (!client_rect(rect) || !ClipCursor(&rect)) {
            release_cursor();
            return false;
        }
        clipped_ = true;
        requested_clip_ = rect;
        if (!GetClipCursor(&applied_clip_)) applied_clip_ = rect;
        if (!hidden_) {
            // The cursor shows while the thread's display count is not negative.
            while (hides_ < 16) {
                ++hides_;
                if (ShowCursor(FALSE) < 0) break;
            }
            hidden_ = true;
        }
        return true;
    }
    // Activation snapshot. Also restarts the auto-repeat tracking from what is held now: releases
    // that happened while the application was in the background were never reported.
    DesktopPressedState pressed() override {
        DesktopPressedState state;
        mag_win32_key_reset(&keys);
        for (int e0 = 0; e0 < 2; ++e0) {
            for (UINT code = 1; code < 0x80; ++code) {
                const uint32_t usage = mag_win32_raw_key_usage(uint16_t(code), e0 ? MAG_WIN32_KEY_E0 : 0, 0);
                if (!usage) continue;
                const UINT key = MapVirtualKeyW(e0 ? (0xE000u | code) : code, MAPVK_VSC_TO_VK_EX);
                if (!virtual_key_down(key) && !virtual_key_down(keypad_virtual_key(usage))) continue;
                if (std::find(state.keys.begin(), state.keys.end(), usage) == state.keys.end())
                    state.keys.push_back(usage);
                mag_win32_key_set(&keys, usage);
            }
        }
        // GetAsyncKeyState names logical buttons; capture reports physical ones.
        const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        const UINT buttons[5] = {UINT(swapped ? VK_RBUTTON : VK_LBUTTON), UINT(swapped ? VK_LBUTTON : VK_RBUTTON),
                                 VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        for (uint32_t index = 0; index < 5; ++index)
            if (virtual_key_down(buttons[index])) state.buttons.emplace_back(index, kMouseDevice);
        return state;
    }
    uint64_t now_ns() override { return steady_ns(); }

    // Relative mode holds a confinement that no longer matches the window or was replaced.
    bool clip_lost() {
        if (!clipped_) return false;
        RECT want, now;
        if (!client_rect(want) || !GetClipCursor(&now)) return true;
        return !same_rect(want, requested_clip_) || !same_rect(now, applied_clip_);
    }
    void release_cursor() {
        if (clipped_) ClipCursor(nullptr);
        clipped_ = false;
        for (; hides_ > 0; --hides_) ShowCursor(TRUE);
        hidden_ = false;
    }

private:
    bool client_rect(RECT& rect) {
        if (!window_attached() || IsIconic(window) || !GetClientRect(window, &rect)) return false;
        POINT corners[2] = {{rect.left, rect.top}, {rect.right, rect.bottom}};
        if (!ClientToScreen(window, &corners[0]) || !ClientToScreen(window, &corners[1])) return false;
        rect = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
        return rect.right > rect.left && rect.bottom > rect.top;
    }
    bool clipped_ = false, hidden_ = false;
    int hides_ = 0;
    RECT requested_clip_{}, applied_clip_{};
};

struct Backend {
    Win32Platform platform;
    DesktopCaptureEngineSink sink;
    DesktopCaptureCore core{platform, sink};
    std::atomic<HWND> requested{nullptr};  // what the SDK attached; applied on the window thread
    std::atomic<DWORD> window_thread{0};   // the thread that owns the attached window
    // Window thread only.
    bool focused = false, registration_failed = false;
    int reported = -1;
    bool lock_lost_reported = false;
};

// Never destroyed: the SDK may pump until the process ends.
Backend& backend() {
    static Backend* state = new Backend;
    return *state;
}

void set_raw_input(Backend& b, bool on) {
    if (on == b.platform.registered) return;
    if (on) {
        if (b.registration_failed) return;
        RAWINPUTDEVICE devices[2] = {{kGenericDesktop, kKeyboardUsage, 0, b.platform.window},
                                     {kGenericDesktop, kMouseUsage, 0, b.platform.window}};
        if (!RegisterRawInputDevices(devices, 2, sizeof devices[0])) {
            // Not retried until the window changes; capture stays installed but receives nothing.
            b.registration_failed = true;
            std::fprintf(stderr, "[game-controls] Raw Input registration failed (error %lu): no keyboard/mouse capture\n",
                         GetLastError());
            return;
        }
        mag_win32_key_reset(&b.platform.keys);
        b.platform.registered = true;
    } else {
        RAWINPUTDEVICE devices[2] = {{kGenericDesktop, kKeyboardUsage, RIDEV_REMOVE, nullptr},
                                     {kGenericDesktop, kMouseUsage, RIDEV_REMOVE, nullptr}};
        RegisterRawInputDevices(devices, 2, sizeof devices[0]);
        b.platform.registered = false;
    }
}

// Window thread: the attached window changed or went away.
void apply_window(Backend& b, HWND window) {
    if (window == b.platform.window) return;
    set_raw_input(b, false);
    b.core.window_changed();
    b.platform.release_cursor();
    b.platform.window = window;
    b.window_thread.store(window ? GetWindowThreadProcessId(window, nullptr) : 0, std::memory_order_release);
    mag_win32_key_reset(&b.platform.keys);
    b.focused = false;
    b.registration_failed = false;
}

void raw_input(Backend& b, HRAWINPUT handle) {
    RAWINPUT raw;
    UINT size = sizeof raw;
    if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    const auto stamp = b.core.stamp();
    const uint64_t time = steady_ns();
    // Checked per report: focus can leave between two pumps, before the core has refreshed.
    const int focused = b.platform.window_focused() ? 1 : 0;
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& key = raw.data.keyboard;
        const uint32_t usage = mag_win32_raw_key_usage(key.MakeCode, key.Flags, key.VKey);
        const bool down = (key.Flags & RI_KEY_BREAK) == 0;
        // The key state still tracks every edge, so auto-repeat stays recognised after refocus.
        if (usage && mag_win32_key_edge(&b.platform.keys, usage, down) && mag_win32_raw_input_admitted(focused, down))
            b.core.key(usage, down, time, stamp);
    } else if (raw.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& mouse = raw.data.mouse;
        MagWin32MouseEdge edges[10];
        const unsigned count = mag_win32_raw_mouse_edges(mouse.usButtonFlags, edges);
        for (unsigned i = 0; i < count; ++i)
            if (mag_win32_raw_input_admitted(focused, edges[i].down))
                b.core.mouse_button(edges[i].index, kMouseDevice, edges[i].down != 0, time, stamp);
        double dx = 0, dy = 0;
        if (mag_win32_raw_mouse_motion(mouse.usFlags, mouse.lLastX, mouse.lLastY, &dx, &dy) &&
            mag_win32_raw_input_admitted(focused, 1))
            b.core.mouse_motion(dx, dy, stamp);
    }
}

void pump(Backend& b, bool dialog) {
    HWND window = b.requested.load(std::memory_order_acquire);
    if (window && !IsWindow(window)) window = nullptr;
    if (window != b.platform.window) {
        const DWORD owner = window ? GetWindowThreadProcessId(window, nullptr)
                                   : b.window_thread.load(std::memory_order_acquire);
        if (!owner || owner == GetCurrentThreadId()) apply_window(b, window);
    }
    // Only the attached window's own pump drives capture.
    const DWORD owner = b.window_thread.load(std::memory_order_acquire);
    if (owner && owner != GetCurrentThreadId()) return;
    b.platform.dialog = dialog;
    set_raw_input(b, b.platform.window && b.core.enabled_requested());
    const bool focused = b.platform.window_focused();
    if (focused && !b.focused) b.core.request_recapture();
    b.focused = focused;
    // Something replaced or dropped the confinement (another window, a resize): re-apply it at
    // this refresh, logged once per loss.
    if (b.core.relative() && b.platform.clip_lost()) {
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
    // Focus loss releases the confinement and shows the cursor (set_relative(false)); one line
    // per change makes that visible.
    if (const char* change = desktop_capture_transition(was_ready, was_relative, b.core.ready(), b.core.relative()))
        std::fprintf(stderr, "[game-controls] %s\n", change);
    const int on = b.core.installed() ? 1 : 0;
    if (on != b.reported) {
        if (b.reported >= 0 || on)
            std::fprintf(stderr, "[game-controls] desktop capture %s (Win32 window)\n", on ? "on" : "off");
        b.reported = on;
    }
}

int message(Backend& b, const MSG* msg, bool dialog) {
    // The pump reads only its own thread's messages, so one for the attached window is on its thread.
    if (!msg || !b.platform.window || msg->hwnd != b.platform.window) return 0;
    switch (msg->message) {
    case WM_INPUT:
        if (b.platform.registered && GET_RAWINPUT_CODE_WPARAM(msg->wParam) == RIM_INPUT)
            raw_input(b, reinterpret_cast<HRAWINPUT>(msg->lParam));
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        // A click in the game window takes the cursor back (click-to-recapture) when it is free.
        b.core.recapture_after_click(!b.platform.clip_lost());
        return 0;
    case WM_KEYDOWN: {
        if (msg->wParam != VK_ESCAPE) return 0;
        DesktopQuitKeyContext context;
        context.quit_key_down = true;
        context.keyboard_owned = mag_input_capture_keyboard_owned() != 0;
        context.capture_requested = b.core.enabled_requested();
        context.on_game_window = true;
        context.game_window_focused = b.platform.window_focused();
        context.dialog_active = dialog;
        return desktop_capture_withholds_quit_key(context) ? 1 : 0;
    }
    default:
        return 0;
    }
}

template <class F> auto guarded(F&& body, decltype(body()) failed) -> decltype(body()) {
    auto& b = backend();
    try {
        return body();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[game-controls] desktop capture error: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[game-controls] desktop capture error\n");
    }
    try {
        b.core.interrupt();
    } catch (...) {
    }
    return failed;
}
}  // namespace

extern "C" void mag_input_desktop_win32_window(void* hwnd) {
    guarded([&] {
        auto& b = backend();
        const HWND window = static_cast<HWND>(hwnd);
        b.requested.store(window, std::memory_order_release);
        // The SDK attaches and detaches on the window's own thread: apply at once there, so the
        // cursor is released before the window is destroyed. Elsewhere the next pump applies it.
        const DWORD owner = window ? GetWindowThreadProcessId(window, nullptr)
                                   : b.window_thread.load(std::memory_order_acquire);
        if (owner && owner == GetCurrentThreadId()) apply_window(b, window);
        return 0;
    }, 0);
}

extern "C" void mag_input_desktop_win32_pump(int dialog_active) {
    guarded([&] {
        pump(backend(), dialog_active != 0);
        return 0;
    }, 0);
}

extern "C" int mag_input_desktop_win32_message(void* msg, int dialog_active) {
    return guarded([&] { return message(backend(), static_cast<const MSG*>(msg), dialog_active != 0); }, 0);
}

// The window pump seams supply the game window; an explicit attachment is not needed here.
extern "C" void mag_input_desktop_attach_window(void*) {}
extern "C" void mag_input_desktop_set_enabled(int enabled) {
    auto& b = backend();
    // Without a game window (a headless run) nothing pumps; say so once instead of waiting silently.
    static std::atomic<bool> waiting_reported{false};
    if (b.core.request_enabled(enabled != 0) && enabled && !b.requested.load(std::memory_order_acquire) &&
        !waiting_reported.exchange(true))
        std::fprintf(stderr, "[game-controls] desktop capture requested; waiting for the Win32 game window\n");
}
extern "C" void mag_input_desktop_set_mouse_enabled(int enabled) { backend().core.request_mouse(enabled != 0); }
// Requests apply at the window's next pump, every frame.
extern "C" void mag_input_desktop_refresh(void) {}
extern "C" int mag_input_desktop_raw_mouse_available(void) { return backend().core.raw_mouse_available(); }
