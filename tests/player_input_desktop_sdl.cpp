// SDL desktop capture backend through a real SDL event queue (SDL_VIDEODRIVER=dummy): synthetic
// events pushed with SDL_PushEvent pass the backend's event filter, a loop shaped like the SDK's
// window pump reads them, and the pump seam hands them to the capture core. The engine and the
// keyboard-ownership flag are fakes. Synthetic input only.
#include "player_controls.h"
#include "player_input_desktop.h"
#include "player_input_desktop_sdl.h"
#include <SDL2/SDL.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Capture { bool focused = true, mouse = false; };
Capture capture;
std::vector<mag::PhysicalInputEvent> events;
double motion_x = 0, motion_y = 0;
int owned = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}  // namespace

namespace mag {
void mag_controls_push_event(const PhysicalInputEvent& event) { events.push_back(event); }
void mag_controls_push_relative_mouse(double dx, double dy) { motion_x += dx, motion_y += dy; }
void mag_controls_set_window_capture(bool focused, bool mouse_captured) { capture = {focused, mouse_captured}; }
void mag_controls_release_source(InputSource) {}
}  // namespace mag
extern "C" int mag_input_capture_keyboard_owned(void) { return owned; }
extern "C" int mag_player_input_blocked(void) { return 0; }

namespace {
SDL_Window* window;
// The SDK's window pump: poll every event (Escape quits), then the per-pump keyboard sample.
bool pump(int dialog = 0) {
    bool quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) quit = true;
    mag_input_desktop_sdl_pump(window, dialog);
    return quit;
}
void key(SDL_Scancode code, bool down, bool repeat = false, Uint32 id = 0) {
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = id ? id : SDL_GetWindowID(window);
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = repeat;
    event.key.keysym.scancode = code;
    event.key.keysym.sym = SDL_GetKeyFromScancode(code);
    if (code == SDL_SCANCODE_ESCAPE) event.key.keysym.sym = SDLK_ESCAPE;
    SDL_PushEvent(&event);
}
void button(Uint8 index, bool down, Uint32 which = 0) {
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = SDL_GetWindowID(window);
    event.button.which = which;
    event.button.button = index;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&event);
}
void motion(int dx, int dy) {
    SDL_Event event{};
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = SDL_GetWindowID(window);
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    SDL_PushEvent(&event);
}
bool saw(mag::InputSource source, uint32_t code, bool down) {
    for (const auto& event : events)
        if (event.control.source == source && event.control.code == code && event.down == down) return true;
    return false;
}
bool filter_installed() {
    SDL_EventFilter filter = nullptr;
    void* data = nullptr;
    return SDL_GetEventFilter(&filter, &data) == SDL_TRUE && filter;
}

void run() {
    require(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video (dummy driver) initializes");
    window = SDL_CreateWindow("mag-desktop-capture", 0, 0, 64, 64, 0);
    require(window, "a window can be created");
    pump();
    if (SDL_GetKeyboardFocus() != window) throw std::runtime_error("skip: this SDL dummy driver gives no keyboard focus");

    // Not requested: no filter, nothing reaches the engine, Escape still quits.
    owned = 1;
    key(SDL_SCANCODE_W, true);
    key(SDL_SCANCODE_ESCAPE, true);
    require(pump(), "without capture Escape still quits");
    require(!filter_installed() && events.empty() && capture.focused && !capture.mouse, "an unrequested backend is inert");

    mag_input_desktop_set_enabled(1);
    pump();
    require(filter_installed(), "capture installs the event filter");
    require(capture.focused && capture.mouse && SDL_GetRelativeMouseMode(), "the focused window is captured in relative mode");
    require(mag_input_desktop_raw_mouse_available(), "raw motion is offered");

    key(SDL_SCANCODE_W, false);
    key(SDL_SCANCODE_W, true);
    key(SDL_SCANCODE_W, true, true);
    pump();
    require(saw(mag::InputSource::Keyboard, SDL_SCANCODE_W, true), "a key press reaches the engine as its HID usage");
    require(events.size() == 1, "a release that was never captured and key repeats are dropped");
    key(SDL_SCANCODE_W, false);
    pump();
    require(saw(mag::InputSource::Keyboard, SDL_SCANCODE_W, false), "the release follows");

    key(SDL_SCANCODE_ESCAPE, true);
    key(SDL_SCANCODE_ESCAPE, true, true);
    require(!pump(), "Escape does not quit while the adapter owns the focused game window");
    require(saw(mag::InputSource::Keyboard, SDL_SCANCODE_ESCAPE, true), "the withheld Escape still reaches the controls");
    key(SDL_SCANCODE_ESCAPE, false);
    pump();
    owned = 0;
    key(SDL_SCANCODE_ESCAPE, true);
    require(pump(), "Escape quits when the adapter does not own the keyboard");
    owned = 1;
    key(SDL_SCANCODE_ESCAPE, true, false, SDL_GetWindowID(window) + 100);
    require(pump(), "Escape on another window is passed on");
    key(SDL_SCANCODE_ESCAPE, false);
    pump(1);
    key(SDL_SCANCODE_ESCAPE, true);
    require(pump(1), "a dialog that owns the keyboard receives Escape");
    require(!capture.focused, "a dialog takes the keyboard from the controls");
    pump();
    require(capture.focused, "closing the dialog gives it back");

    events.clear();
    button(SDL_BUTTON_RIGHT, true);
    button(SDL_BUTTON_X1, true);
    button(SDL_BUTTON_LEFT, true, SDL_TOUCH_MOUSEID);
    motion(5, 3);
    pump();
    require(saw(mag::InputSource::Mouse, 1, true) && saw(mag::InputSource::Mouse, 3, true), "SDL buttons map to right 1 and X1 3");
    require(!saw(mag::InputSource::Mouse, 0, true), "touch-synthesized clicks are ignored");
    require(events.back().device == 1, "SDL's logical mouse is one device");
    require(motion_x == 5 && motion_y == -3, "relative motion is reported up-positive");

    mag_input_desktop_set_mouse_enabled(0);
    pump();
    require(!SDL_GetRelativeMouseMode() && capture.focused && !capture.mouse, "menus give the cursor back");
    require(!saw(mag::InputSource::Mouse, 1, false), "a mouse-mode change keeps held buttons");
    motion(4, 4);
    pump();
    require(motion_x == 5, "no motion outside relative mode");
    mag_input_desktop_set_mouse_enabled(1);
    pump();
    require(SDL_GetRelativeMouseMode() && capture.mouse, "gameplay takes relative mode again");

    // Something takes the lock (here: relative mode turned off behind the backend's back). The
    // next pump re-applies it, centring the pointer; that warp is not look input.
    const double before_x = motion_x, before_y = motion_y;
    setenv("MAG_GAME_CONTROLS_TRACE", "1", 1);  // the re-lock below arms the motion trace
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_WarpMouseInWindow(window, 1, 1);  // the pointer has drifted to a corner
    pump();
    pump();
    require(SDL_GetRelativeMouseMode() && SDL_GetWindowGrab(window), "a lost lock is re-applied with the grab");
    require(motion_x == before_x && motion_y == before_y, "the re-lock's centring warp became look input");
    motion(2, -1);
    pump();
    require(motion_x == before_x + 2 && motion_y == before_y + 1, "real motion still reaches the controls after a re-lock");
    unsetenv("MAG_GAME_CONTROLS_TRACE");

    mag_input_desktop_set_enabled(0);
    pump();
    require(!filter_installed() && !SDL_GetRelativeMouseMode(), "uninstall removes the filter and relative mode");
    require(saw(mag::InputSource::Mouse, 1, false) && saw(mag::InputSource::Mouse, 3, false), "uninstall releases held buttons");
    require(capture.focused && !capture.mouse, "the engine's window fields are handed back");
    key(SDL_SCANCODE_ESCAPE, true);
    require(pump(), "Escape quits again once capture is off");

    mag_input_desktop_sdl_pump(nullptr, 1);  // the window goes away
    SDL_DestroyWindow(window);
    SDL_Quit();
}
}  // namespace

int main() {
    try {
        run();
        std::cout << "sdl desktop capture: ok\n";
        return 0;
    } catch (const std::exception& error) {
        const std::string message = error.what();
        std::cerr << message << '\n';
        return message.rfind("skip:", 0) == 0 ? 77 : 1;
    }
}
