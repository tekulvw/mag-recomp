// The settings window's process and its place in a launch (player_settings_session.h).
#include "player_settings_session.h"
#include "mag_extension.h"
#ifdef MAG_PLAYER_SETTINGS_CONTROLS
#include "player_controls_settings.h"
#endif
#include "player_settings_form.h"
#include "player_settings_window.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

// Starts `binary` with one switch and waits for it. -1 when it could not start.
int run_window_process(const fs::path& binary, const char* argument) {
#ifdef _WIN32
    std::wstring command = L"\"" + binary.wstring() + L"\" ";
    for (const char* c = argument; *c; ++c) command += wchar_t(*c);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    // The window process may take the foreground from this one.
    AllowSetForegroundWindow(ASFW_ANY);
    if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process))
        return -1;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return int(code);
#else
    const std::string path = binary.string();
    char* argv[] = {const_cast<char*>(path.c_str()), const_cast<char*>(argument), nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, path.c_str(), nullptr, nullptr, argv, environ) != 0) return -1;
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

// After a save: the running game takes the saved controls now (the rest waits for the next launch).
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
    const bool setup = !std::strcmp(argv[1], MAG_PLAYER_SETTINGS_SETUP_SWITCH);
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
    std::string notice;
    if (!mag::settings_form_load(settings, form, notice))
        notice = settings.string() + ": " + notice + " Saving replaces it.";
    switch (mag::settings_window_run(form, settings, setup, notice)) {
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
        const int code = run_window_process(self, MAG_PLAYER_SETTINGS_SWITCH);
        if (code < 0) say("the settings window could not start");
        else if (code == 0) apply_saved_controls();
        else say(code == 3 ? "settings window closed without saving" : "the settings window failed; see the log above");
        hold_input(false);
        s_open.store(false, std::memory_order_release);
    }).detach();
}

extern "C" int mag_player_settings_setup_wanted(int settings_exist) {
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
    if (code < 0) say("the settings window could not start; continuing with the saved settings");
    return code == 0;
}

#ifdef _WIN32
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
