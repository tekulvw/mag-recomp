// The settings window's process and its place in a launch (player_settings_session.h).
#include "player_settings_session.h"
#include "mag_extension.h"
#ifdef MAG_PLAYER_SETTINGS_CONTROLS
#include "player_controls_settings.h"
#endif
#include "player_settings_form.h"
#include "player_settings_text.h"
#include "player_settings_window.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <fcntl.h>
#include <deque>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifdef MAG_PLAYER_INPUT_HOLD
// The controls seams' input block (player_settings_runtime.h): controllers read neutral and the
// desktop capture releases what it held while the window is open.
extern "C" void mag_player_set_input_blocked(int blocked);
#endif

namespace fs = std::filesystem;

namespace {
std::atomic<bool> s_open{false};
#ifdef _WIN32
std::atomic<HWND> s_game_window{nullptr};
#endif

void say(const std::string& message) { std::fprintf(stderr, "[mag settings] %s\n", message.c_str()); }

fs::path path_variable(const char* name) {
#ifdef _WIN32
    std::wstring wide(name, name + std::strlen(name));
    const wchar_t* value = _wgetenv(wide.c_str());
    return value && *value ? fs::path(value) : fs::path();
#else
    const char* value = std::getenv(name);
    return value && *value ? fs::path(value) : fs::path();
#endif
}

fs::path own_binary() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (!n || n >= buffer.size()) return {};
    buffer.resize(n);
    return fs::path(buffer);
#else
    std::error_code code;
    auto self = fs::read_symlink("/proc/self/exe", code);
    return code ? fs::path() : self;
#endif
}

void hold_input(bool held) {
#ifdef MAG_PLAYER_INPUT_HOLD
    mag_player_set_input_blocked(held ? 1 : 0);
#else
    (void)held;
#endif
}

// Gameplay input is held while the settings window is open and the game window does not have the
// focus: what the player types or presses in the settings window (a binding capture among it) never
// reaches the game, and clicking back into the game plays as usual with the window still open.
// The window's open and close and the game window's pump (its focus, every frame) report here.
std::mutex s_hold_lock;
bool s_hold_open = false;     // the settings window is open
bool s_game_focused = false;  // the game window had the focus at its last pump
bool s_held = false;          // what the input block was last given
enum class Report { Open, Closed, Focused, Unfocused };
void update_hold(Report report) {
    std::lock_guard<std::mutex> lock(s_hold_lock);
    switch (report) {
    case Report::Open: s_hold_open = true; break;
    case Report::Closed: s_hold_open = false; break;
    case Report::Focused: s_game_focused = true; break;
    case Report::Unfocused: s_game_focused = false; break;
    }
    const bool held = s_hold_open && !s_game_focused;
    if (held == s_held) return;
    s_held = held;
    hold_input(held);
    // Opening and closing say so themselves; a change of focus with the window open is said here.
    if (s_hold_open && (report == Report::Focused || report == Report::Unfocused))
        say(held ? "gameplay input is held while the settings window is open and the game window does not have the focus"
                 : "the game window has the focus; gameplay input is live while the settings window stays open");
}
void game_focus(bool focused) { update_hold(focused ? Report::Focused : Report::Unfocused); }

#ifndef _WIN32
// Requests from the settings window for the game window (`display ...`), applied by the window's
// own pump (mag_player_settings_sdl_pump).
std::mutex s_display_lock;
std::deque<std::string> s_display_requests;
bool s_display_previewing = false;  // a preview not yet kept or restored (pump thread only)
void request_display(const std::string& line) {
    std::lock_guard<std::mutex> lock(s_display_lock);
    s_display_requests.push_back(line);
}
#endif

// Starts `binary` with these switches and waits for it. -1 when it could not start. `signalled`
// runs (on this thread) each time the window saves controls: on Windows when `event` is set; on
// POSIX for each "controls" line on the window's event pipe (`events`, the read end, whose write
// end the child gets as fd 3).
int run_window_process(const fs::path& binary, const std::string& arguments, void* event = nullptr,
                       void (*signalled)() = nullptr, int events = -1, int events_write = -1) {
#ifdef _WIN32
    (void)events;
    (void)events_write;
    std::wstring command = L"\"" + binary.wstring() + L"\" ";
    for (char c : arguments) command += wchar_t(c);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    // The window process may take the foreground from this one.
    AllowSetForegroundWindow(ASFW_ANY);
    if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process))
        return -1;
    CloseHandle(process.hThread);
    HANDLE waits[2] = {process.hProcess, HANDLE(event)};
    while (event && WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
        if (signalled) signalled();
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return int(code);
#else
    (void)event;
    const std::string path = binary.string();
    std::vector<std::string> words;
    for (size_t at = 0; at < arguments.size();) {
        const size_t end = std::min(arguments.find(' ', at), arguments.size());
        if (end > at) words.push_back(arguments.substr(at, end - at));
        at = end + 1;
    }
    std::vector<char*> argv = {const_cast<char*>(path.c_str())};
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    pid_t child = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (events_write >= 0) posix_spawn_file_actions_adddup2(&actions, events_write, 3);  // dup2 clears close-on-exec
    const int spawned = posix_spawn(&child, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (events_write >= 0) close(events_write);
    if (spawned != 0) { if (events >= 0) close(events); return -1; }
    if (events >= 0) {
        // Until the window closes (end of file): saved controls now, display requests to the pump.
        std::string pending;
        char buffer[256];
        for (;;) {
            const ssize_t n = read(events, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            pending.append(buffer, size_t(n));
            for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                const std::string line = pending.substr(0, end);
                if (line == "controls") { if (signalled) signalled(); }
                else if (line.rfind("display ", 0) == 0) request_display(line);
            }
        }
        close(events);
        request_display("display end");  // a preview the window left behind returns
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

// After a save: the running game takes the saved controls now (the rest waits for the next launch).
void apply_saved_controls();
void controls_saved() { apply_saved_controls(); }
void apply_saved_controls() {
#ifndef MAG_PLAYER_SETTINGS_CONTROLS
    say("saved; server and display changes apply at the next launch");
#else
    if (!mag::mag_controls_settings_available()) return;
    try {
        const auto registration = mag::mag_controls_settings_registration();
        const auto profile = mag::mag_controls_settings_load();
        if (registration.apply) registration.apply(profile);
        say("saved controls applied to this game; server and display changes apply at the next launch");
    } catch (const std::exception& e) {
        say(std::string("saved controls were not applied: ") + e.what());
    }
#endif
}
}  // namespace

extern "C" int mag_player_settings_window_command(int argc, char** argv, int* code) {
    if (argc < 2 || (std::strcmp(argv[1], MAG_PLAYER_SETTINGS_SWITCH) && std::strcmp(argv[1], MAG_PLAYER_SETTINGS_SETUP_SWITCH)))
        return 0;
    mag::SettingsWindowOptions options;
    options.setup = !std::strcmp(argv[1], MAG_PLAYER_SETTINGS_SETUP_SWITCH);
    // Optional switches: the game window, the event that tells the game controls were saved, and
    // why setup opened again (the launcher needs a player name).
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        const auto value = [&](const char* name) { return option.rfind(name, 0) == 0 ? option.substr(std::strlen(name)) : std::string(); };
        if (!value("--game-window=").empty()) options.game_window = reinterpret_cast<void*>(std::strtoull(value("--game-window=").c_str(), nullptr, 16));
        if (!value("--controls-event=").empty()) options.controls_saved = reinterpret_cast<void*>(std::strtoull(value("--controls-event=").c_str(), nullptr, 16));
        if (option == "--notice=player-name") options.notice = MAG_SETTINGS_NAME_NEEDED;
#ifndef _WIN32
        if (!value("--events-fd=").empty()) {
            options.game_events = std::atoi(value("--events-fd=").c_str());
            // Not for the browser or anything else this window starts: the game waits for its end.
            if (options.game_events >= 0) fcntl(options.game_events, F_SETFD, FD_CLOEXEC);
        }
#endif
    }
    const fs::path settings = path_variable("MAG_PLAYER_SETTINGS_PATH");
    if (settings.empty()) {
        say("The settings window needs MAG_PLAYER_SETTINGS_PATH (start MAG from its own launcher).");
        *code = 1;
        return 1;
    }
    // The controls adapter registers its action catalog here, before any guest memory exists.
    try {
        if (auto prepare = mag_extension()->prepare) prepare("");
    } catch (const std::exception& e) {
        say(std::string("controls are unavailable in the settings window: ") + e.what());
    }
    mag::SettingsForm form;
    std::string problem;
    if (!mag::settings_form_load(settings, form, problem))
        options.notice = settings.string() + ": " + problem + " " MAG_SETTINGS_RECOVERY_PLAY;
    switch (mag::settings_window_run(form, settings, options)) {
    case mag::SettingsWindowResult::Saved: *code = 0; break;
    case mag::SettingsWindowResult::Cancelled: *code = 3; break;
    default: *code = 1; break;
    }
    return 1;
}

extern "C" int mag_player_settings_is_open(void) { return s_open.load(std::memory_order_acquire); }

extern "C" void mag_player_settings_open(void) {
    if (path_variable("MAG_PLAYER_SETTINGS_PATH").empty()) return;
    bool expected = false;
    if (!s_open.compare_exchange_strong(expected, true)) return;
    const fs::path self = own_binary();
    if (self.empty()) { s_open.store(false); say("cannot find this program to open the settings window"); return; }
    update_hold(Report::Open);
    say("settings window opened; gameplay input is held while it is open and the game window does not have the focus");
    std::thread([self] {
        std::string arguments = MAG_PLAYER_SETTINGS_SWITCH;
        void* event = nullptr;
#ifdef _WIN32
        // The window previews display changes on the game window, and says when it saved controls.
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        event = CreateEventW(&inherit, FALSE, FALSE, nullptr);
        char handles[96];
        std::snprintf(handles, sizeof handles, " --game-window=%llx --controls-event=%llx",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s_game_window.load())),
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(event)));
        arguments += handles;
#endif
        int events = -1, events_write = -1;
#ifndef _WIN32
        // The window's requests to this game: saved controls, and the display preview.
        int ends[2];
        if (pipe(ends) == 0) {
            fcntl(ends[0], F_SETFD, FD_CLOEXEC);
            fcntl(ends[1], F_SETFD, FD_CLOEXEC);
            events = ends[0];
            events_write = ends[1];
            arguments += " --events-fd=3";
        }
#endif
        const int code = run_window_process(self, arguments, event, controls_saved, events, events_write);
#ifdef _WIN32
        if (event) CloseHandle(event);
#endif
        if (code < 0) say("the settings window could not start");
        else if (code == 0) apply_saved_controls();  // also after General: harmless when unchanged
        else say(code == 3 ? "settings window closed without saving" : "the settings window failed; see the log above");
        update_hold(Report::Closed);
        s_open.store(false, std::memory_order_release);
    }).detach();
}

extern "C" int mag_player_settings_setup_wanted(int settings_exist) {
    // The packaged supervisor opens setup itself (Windows), or the caller asked for none.
    if (const char* skip = std::getenv("MAG_SKIP_SETUP"); skip && *skip && std::strcmp(skip, "0")) return 0;
#ifdef _WIN32
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return 1;
#endif
    return !settings_exist;
}

extern "C" int mag_player_settings_setup(void) {
    // The launcher image has no controls catalog; the engine binary the supervisor verified does.
    const fs::path engine = path_variable("MAG_ENGINE_BINARY");
    if (engine.empty()) return 0;
    say("opening the settings window before the game starts");
    const int code = run_window_process(engine, MAG_PLAYER_SETTINGS_SETUP_SWITCH);
    if (code == 3) say("setup was cancelled; continuing with the saved settings");
    if (code < 0) say("the settings window could not start; continuing with the saved settings");
    return code == 0;
}

#ifdef _WIN32
// The game window as it opens: remembered for the display preview, and sized to the saved output
// resolution (physical pixels, centred on its monitor) when it is windowed. A size that does not
// fit the monitor's work area is left to the runtime.
extern "C" void mag_player_settings_win32_window(void* window) {
    HWND hwnd = static_cast<HWND>(window);
    const HWND previous = s_game_window.exchange(hwnd);
    if (!hwnd || hwnd == previous) return;
    const char* size = std::getenv("MAG_PLAYER_OUTPUT_SIZE");
    unsigned width = 0, height = 0;
    if (!size || std::sscanf(size, "%ux%u", &width, &height) != 2 || !width || !height) return;
    using SetThread = HANDLE(WINAPI*)(HANDLE);
    using Adjust = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    using GetDpi = UINT(WINAPI*)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto set_thread = reinterpret_cast<SetThread>(reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
    const auto adjust = reinterpret_cast<Adjust>(reinterpret_cast<void*>(GetProcAddress(user32, "AdjustWindowRectExForDpi")));
    const auto get_dpi = reinterpret_cast<GetDpi>(reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
    HANDLE previous_context = set_thread ? set_thread(HANDLE(LONG_PTR(-4))) : nullptr;  // physical pixels
    const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)), ex_style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!(style & WS_POPUP)) {
        RECT frame = {0, 0, LONG(width), LONG(height)};
        if (adjust && get_dpi) adjust(&frame, style, FALSE, ex_style, get_dpi(hwnd));
        else AdjustWindowRectEx(&frame, style, FALSE, ex_style);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof monitor;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& area = monitor.rcWork;
        const LONG w = frame.right - frame.left, h = frame.bottom - frame.top;
        if (w <= area.right - area.left && h <= area.bottom - area.top)
            SetWindowPos(hwnd, nullptr, area.left + (area.right - area.left - w) / 2, area.top + (area.bottom - area.top - h) / 2, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        else
            say(std::string("the output resolution ") + size + " does not fit this display; keeping the window's size");
    }
    if (set_thread && previous_context) set_thread(previous_context);
}

extern "C" void mag_player_settings_win32_pump(void) {
    const HWND game = s_game_window.load();
    game_focus(game && GetForegroundWindow() == game && !IsIconic(game));
}

extern "C" int mag_player_settings_win32_message(const void* message) {
    const MSG* msg = static_cast<const MSG*>(message);
    if (!msg || msg->wParam != VK_F10) return 0;
    switch (msg->message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!(msg->lParam & (1L << 30))) mag_player_settings_open();  // not an auto-repeat
        return 1;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return 1;  // its default handling would enter the window's menu mode
    default:
        return 0;
    }
}
#else
namespace {
std::atomic<bool> s_requested{false};
std::atomic<bool> s_focused{false};  // the watched game window has the keyboard focus
SDL_Window* s_watched = nullptr;
int watch(void* userdata, SDL_Event* event) {
    auto* window = static_cast<SDL_Window*>(userdata);
    if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_F10 && !event->key.repeat &&
        window && event->key.windowID == SDL_GetWindowID(window))
        s_requested.store(true, std::memory_order_release);
    if (event->type == SDL_WINDOWEVENT && window && event->window.windowID == SDL_GetWindowID(window)) {
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) s_focused.store(true, std::memory_order_release);
        else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) s_focused.store(false, std::memory_order_release);
    }
    return 1;
}
// The window's client size in physical pixels (`width` x `height`), centred: SDL sizes windows in
// points, which a scaled desktop maps to more pixels.
void set_output_size(SDL_Window* window, int width, int height) {
    int points_w = 0, points_h = 0, pixels_w = 0, pixels_h = 0;
    SDL_GetWindowSize(window, &points_w, &points_h);
    SDL_Vulkan_GetDrawableSize(window, &pixels_w, &pixels_h);
    const double scale = points_w > 0 && pixels_w > 0 ? double(pixels_w) / points_w : 1.0;
    SDL_SetWindowSize(window, int(width / scale + .5), int(height / scale + .5));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}
struct DisplayState { Uint32 fullscreen = 0; int w = 0, h = 0, x = 0, y = 0; } s_before_preview;
void apply_display(SDL_Window* window, const std::string& line) {
    if (line == "display keep") { s_display_previewing = false; return; }
    if (line == "display restore" || line == "display end") {
        if (!s_display_previewing) return;
        s_display_previewing = false;
        SDL_SetWindowFullscreen(window, s_before_preview.fullscreen);
        if (!s_before_preview.fullscreen) {
            SDL_SetWindowSize(window, s_before_preview.w, s_before_preview.h);
            SDL_SetWindowPosition(window, s_before_preview.x, s_before_preview.y);
        }
        return;
    }
    if (!s_display_previewing) {
        s_display_previewing = true;
        s_before_preview.fullscreen = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_GetWindowSize(window, &s_before_preview.w, &s_before_preview.h);
        SDL_GetWindowPosition(window, &s_before_preview.x, &s_before_preview.y);
    }
    int width = 0, height = 0;
    if (line == "display borderless") SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    else if (std::sscanf(line.c_str(), "display windowed %d %d", &width, &height) == 2 && width > 0 && height > 0) {
        SDL_SetWindowFullscreen(window, 0);
        set_output_size(window, width, height);
    }
}
}  // namespace
extern "C" void mag_player_settings_sdl_pump(void* sdl_window) {
    auto* window = static_cast<SDL_Window*>(sdl_window);
    if (window != s_watched) {
        if (s_watched) SDL_DelEventWatch(watch, s_watched);
        s_watched = window;
        s_display_previewing = false;
        s_focused.store(window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS), std::memory_order_release);
        if (window) {
            SDL_AddEventWatch(watch, window);
            // The saved output resolution (MAG_PLAYER_OUTPUT_SIZE) when windowed and it fits the screen.
            unsigned width = 0, height = 0;
            SDL_Rect usable{0, 0, 0, 0};
            if (const char* size = std::getenv("MAG_PLAYER_OUTPUT_SIZE");
                size && std::sscanf(size, "%ux%u", &width, &height) == 2 && width && height &&
                !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) &&
                SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &usable) == 0 &&
                int(width) <= usable.w && int(height) <= usable.h)
                set_output_size(window, int(width), int(height));
        }
    }
    std::deque<std::string> requests;
    {
        std::lock_guard<std::mutex> lock(s_display_lock);
        requests.swap(s_display_requests);
    }
    for (const auto& line : requests)
        if (window) apply_display(window, line);
    game_focus(window && s_focused.load(std::memory_order_acquire));
    if (s_requested.exchange(false, std::memory_order_acq_rel)) mag_player_settings_open();
}
#endif
