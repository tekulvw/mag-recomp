// The Linux settings window's place in a running game (player_settings_session.cpp, SDL half), with
// the dummy SDL video driver and synthetic events only.
//
// The game side runs here: a stand-in game window, F10 pushed into SDL's queue, and the window's
// pump seam (mag_player_settings_sdl_pump). F10 starts this same binary as the settings window's
// process (the session finds it through /proc/self/exe), whose real command line entry point
// (mag_player_settings_window_command) runs with a stand-in for the window itself: it sends the
// game a display preview and saved controls over the events pipe, stepping when the game side
// writes a "go" file, then closes without keeping the preview. The game side checks the output
// size at attach, F10, the input hold, the preview applied by the pump and its restore.
#include "player_settings_session.h"
#include "player_settings_window.h"
#include "mag_extension.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// Stand-ins for what the session links against.
namespace {
std::atomic<int> g_blocked{0};
MagExtensionCallbacks g_extension{};
fs::path folder() { return fs::path(std::getenv("MAG_PLAYER_SETTINGS_PATH")).parent_path(); }
bool wait_for(const fs::path& file) {
    for (int i = 0; i < 1000; ++i) {
        if (fs::exists(file)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
void send(int fd, const char* line) {
    if (write(fd, line, std::strlen(line)) != ssize_t(std::strlen(line))) std::_Exit(41);
}
}  // namespace
extern "C" void mag_player_set_input_blocked(int blocked) { g_blocked.store(blocked); }
extern "C" const MagExtensionCallbacks* mag_extension(void) { return &g_extension; }
namespace mag {
bool settings_form_load(const fs::path&, SettingsForm&, std::string&) { return true; }
// The settings window's process: what the SDL panel sends the game while the player previews a
// display change and applies controls.
SettingsWindowResult settings_window_run(SettingsForm&, const fs::path& settings, const SettingsWindowOptions& options) {
    if (options.setup || options.game_events < 0) return SettingsWindowResult::Failed;
    if (settings.filename() != "settings.json") return SettingsWindowResult::Failed;
    send(options.game_events, "display windowed 320 240\n");
    send(options.game_events, "controls\n");
    if (!wait_for(folder() / "go-borderless")) return SettingsWindowResult::Failed;
    send(options.game_events, "display borderless\n");
    if (!wait_for(folder() / "go-close")) return SettingsWindowResult::Failed;
    return SettingsWindowResult::Cancelled;  // closed with the preview neither kept nor reverted
}
}  // namespace mag

namespace {
SDL_Window* window;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void f10(bool repeat = false, Uint32 id = 0) {
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.windowID = id ? id : SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.repeat = repeat;
    event.key.keysym.scancode = SDL_SCANCODE_F10;
    event.key.keysym.sym = SDLK_F10;
    SDL_PushEvent(&event);
}
// The SDK's window pump: poll every event, then the per-pump seam.
void pump() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
    mag_player_settings_sdl_pump(window);
}
bool size_is(int w, int h) {
    int now_w = 0, now_h = 0;
    SDL_GetWindowSize(window, &now_w, &now_h);
    return now_w == w && now_h == h;
}
template <class F> bool pump_until(F done) {
    for (int i = 0; i < 1000; ++i) {
        pump();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
void touch(const fs::path& file) { std::fclose(std::fopen(file.c_str(), "w")); }

void run() {
    require(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video (dummy driver) initializes");
    window = SDL_CreateWindow("mag-settings-session", 0, 0, 64, 64, 0);
    require(window, "a window can be created");

    // Attaching: the saved output resolution sizes the window; nothing opens without F10.
    pump();
    require(size_is(200, 100), "the window takes MAG_PLAYER_OUTPUT_SIZE when it attaches");
    require(!mag_player_settings_is_open() && g_blocked == 0, "nothing opens by itself");

    // Only a first press of F10 in the game window opens the settings window.
    f10(true);
    f10(false, SDL_GetWindowID(window) + 100);
    pump();
    require(!mag_player_settings_is_open(), "an auto-repeat or another window's F10 opens nothing");
    f10();
    pump();
    require(mag_player_settings_is_open() && g_blocked == 1, "F10 opens the window and holds gameplay input");
    f10();
    pump();  // a second F10 while it is open starts no second window (it would fail: no go files)

    // The window previews a resolution: the game's own pump applies it to the game window.
    require(pump_until([] { return size_is(320, 240); }), "the preview's windowed size reaches the game window");
    touch(folder() / "go-borderless");
    require(pump_until([] { return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; }),
            "the preview's borderless mode reaches the game window");

    // Closed without keeping it: the game window returns to how it was, and input is given back.
    touch(folder() / "go-close");
    require(pump_until([] { return !mag_player_settings_is_open(); }), "the window closes");
    require(g_blocked == 0, "closing the window releases gameplay input");
    pump();
    require(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) && size_is(200, 100),
            "a preview neither kept nor reverted is undone when the window closes");

    mag_player_settings_sdl_pump(nullptr);  // the window goes away
    SDL_DestroyWindow(window);
    SDL_Quit();
}
}  // namespace

int main(int argc, char** argv) {
    int code = 0;
    if (mag_player_settings_window_command(argc, argv, &code)) return code;  // the settings window's process
    try {
        run();
        std::cout << "settings session checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
