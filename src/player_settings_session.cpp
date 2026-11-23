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

// Starts `binary` with these switches and waits for it. -1 when it could not start. On Windows,
// `signalled` runs (on this thread) each time `event` is set while the window is open.
int run_window_process(const fs::path& binary, const std::string& arguments, void* event = nullptr,
                       void (*signalled)() = nullptr) {
#ifdef _WIN32
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
    (void)signalled;
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
    if (posix_spawn(&child, path.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
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
    hold_input(true);
    say("settings window opened; gameplay input is held until it closes");
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
        const int code = run_window_process(self, arguments, event, controls_saved);
#ifdef _WIN32
        if (event) CloseHandle(event);
#endif
        if (code < 0) say("the settings window could not start");
        else if (code == 0) apply_saved_controls();  // also after General: harmless when unchanged
        else say(code == 3 ? "settings window closed without saving" : "the settings window failed; see the log above");
        hold_input(false);
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
SDL_Window* s_watched = nullptr;
int watch(void* userdata, SDL_Event* event) {
    auto* window = static_cast<SDL_Window*>(userdata);
    if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_F10 && !event->key.repeat &&
        window && event->key.windowID == SDL_GetWindowID(window))
        s_requested.store(true, std::memory_order_release);
    return 1;
}
}  // namespace
extern "C" void mag_player_settings_sdl_pump(void* sdl_window) {
    auto* window = static_cast<SDL_Window*>(sdl_window);
    if (window != s_watched) {
        if (s_watched) SDL_DelEventWatch(watch, s_watched);
        s_watched = window;
        if (window) SDL_AddEventWatch(watch, window);
    }
    if (s_requested.exchange(false, std::memory_order_acq_rel)) mag_player_settings_open();
}
#endif
