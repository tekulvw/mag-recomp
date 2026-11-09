// The Windows SDK seams of the desktop capture backend (src/player_input_win32_pad.c and
// src/player_input_win32_window.c), built against synthetic stand-ins for the system and SDK
// headers (written by tests/test_player_input_desktop_win32.py) with recording fakes. Synthetic
// input only.
#include <windows.h>
#include <xinput.h>
#include "libs/misc/osk_native.h"
#include "player_input_capture.h"
#include "player_input_desktop_win32.h"
#include "player_settings_runtime.h"
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
DWORD WINAPI mag_player_xinput_get_state(DWORD port, XINPUT_STATE* state);
SHORT WINAPI mag_player_get_async_key_state(int key);
void mag_player_win32_set_window(void* window);
void mag_player_win32_pump(void);
int mag_player_win32_message(void* message);
}

namespace {
std::string calls;
XINPUT_STATE pads[4];
bool connected[4];
int owned = 0, async_reads = 0, dialog = 0, withhold = 0, osk_handles = 0;
void* controller_of_port[4];

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}  // namespace

extern "C" {
// The system.
DWORD WINAPI XInputGetState(DWORD port, XINPUT_STATE* state) {
    if (port > 3 || !connected[port]) return ERROR_DEVICE_NOT_CONNECTED;
    *state = pads[port];
    return ERROR_SUCCESS;
}
SHORT WINAPI GetAsyncKeyState(int key) {
    ++async_reads;
    return key == 'E' ? SHORT(0x8000) : 0;
}
// The capture layer.
int mag_input_capture_keyboard_owned(void) { return owned; }
void mag_input_capture_backend_begin(void) { calls += "begin;"; }
void mag_input_capture_controller_open(void* controller, unsigned port) {
    controller_of_port[port] = controller;
    calls += "open" + std::to_string(port) + ";";
}
void mag_input_capture_controller_attached(void* controller, int attached) {
    for (unsigned port = 0; port < 4; ++port)
        if (controller_of_port[port] == controller) calls += "attached" + std::to_string(port) + "=" + std::to_string(attached) + ";";
}
void mag_input_capture_controller_axis(void* controller, unsigned axis, int32_t raw) {
    if (controller == controller_of_port[0]) calls += "axis" + std::to_string(axis) + "=" + std::to_string(raw) + ";";
}
void mag_input_capture_controller_button(void* controller, unsigned button, int down) {
    if (controller == controller_of_port[0] && down) calls += "button" + std::to_string(button) + ";";
}
// The on-screen keyboard and the desktop capture backend.
void osk_native_win32_set_window(void* window) { calls += std::string("osk-window(") + (window ? "w" : "0") + ");"; }
void osk_native_win32_pump(void) {
    calls += "osk-pump;";
    dialog = 1;  // the pump may open the dialog; the backend must see that
}
int osk_native_win32_handle_message(void*) {
    calls += "osk-message;";
    return osk_handles;
}
int osk_native_win32_input_active(void) { return dialog; }
void mag_input_desktop_win32_window(void* window) { calls += std::string("capture-window(") + (window ? "w" : "0") + ");"; }
void mag_input_desktop_win32_pump(int dialog_active) { calls += "capture-pump(" + std::to_string(dialog_active) + ");"; }
int mag_input_desktop_win32_message(void*, int dialog_active) {
    calls += "capture-message(" + std::to_string(dialog_active) + ");";
    return withhold;
}
}

namespace {
void xinput() {
    // No controller anywhere: every port reports disconnected, port 0 opens a refresh.
    XINPUT_STATE state{};
    for (DWORD port = 0; port < 4; ++port) require(mag_player_xinput_get_state(port, &state) == ERROR_DEVICE_NOT_CONNECTED, "absent");
    require(calls == "begin;open0;attached0=0;open1;attached1=0;open2;attached2=0;open3;attached3=0;",
            "port 0 begins the refresh; each port opens once and reports its connection");
    calls.clear();

    connected[0] = true;
    pads[0].dwPacketNumber = 7;
    pads[0].Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_RIGHT;
    pads[0].Gamepad.sThumbLX = -100;
    pads[0].Gamepad.sThumbLY = 32767;
    pads[0].Gamepad.sThumbRX = 200;
    pads[0].Gamepad.sThumbRY = -300;
    pads[0].Gamepad.bLeftTrigger = 255;
    pads[0].Gamepad.bRightTrigger = 0;
    require(mag_player_xinput_get_state(0, &state) == ERROR_SUCCESS, "connected");
    require(calls == "begin;attached0=1;axis0=-100;axis1=-32767;axis2=200;axis3=300;axis4=32767;axis5=0;"
                     "button0;button6;button14;",
            "a reading reaches the capture layer in SDL numbering, without reopening the port");
    require(state.dwPacketNumber == 7 && state.Gamepad.wButtons == pads[0].Gamepad.wButtons &&
                state.Gamepad.sThumbLY == 32767 && state.Gamepad.bLeftTrigger == 255,
            "cellPad reads the controller unchanged");
    calls.clear();
    require(mag_player_xinput_get_state(1, &state) == ERROR_DEVICE_NOT_CONNECTED && calls == "attached1=0;",
            "other ports do not begin a refresh");

    mag_player_set_input_blocked(1);
    require(mag_player_input_blocked() == 1, "block recorded");
    calls.clear();
    require(mag_player_xinput_get_state(0, &state) == ERROR_SUCCESS, "still connected while blocked");
    require(calls.find("button0;") != std::string::npos, "the capture layer still sees the raw reading");
    require(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbLY == 0 && state.Gamepad.bLeftTrigger == 0 &&
                state.dwPacketNumber == 7,
            "a block masks what cellPad sees");
    mag_player_set_input_blocked(0);
}

void keyboard() {
    require(mag_player_get_async_key_state('E') == SHORT(0x8000) && async_reads == 1, "unowned: the system's answer");
    owned = 1;
    require(mag_player_get_async_key_state('E') == 0 && async_reads == 1, "owned: nothing held, the system is not asked");
    // Ownership alone decides, whatever XInput reports: a (virtual) controller on port 0 does not
    // turn the suppression off, and no controller does not turn it on.
    connected[0] = true;
    XINPUT_STATE state{};
    mag_player_xinput_get_state(0, &state);
    require(mag_player_get_async_key_state('E') == 0, "owned with a controller");
    owned = 0;
    require(mag_player_get_async_key_state('E') == SHORT(0x8000), "released ownership restores the fallback");
}

void window() {
    int handle = 0;
    mag_player_win32_set_window(&handle);
    mag_player_win32_set_window(nullptr);
    require(calls == "capture-window(w);osk-window(w);capture-window(0);osk-window(0);",
            "the backend hears of the window first, so it lets the cursor go before the window goes");
    calls.clear();
    mag_player_win32_pump();
    require(calls == "osk-pump;capture-pump(1);", "the keyboard's pump runs first; the backend gets its dialog flag");
    calls.clear();
    int msg = 0;
    osk_handles = 1;
    require(mag_player_win32_message(&msg) == 1 && calls == "capture-message(1);osk-message;",
            "the backend observes every message before the dialog");
    calls.clear();
    osk_handles = 0;
    require(mag_player_win32_message(&msg) == 0 && calls == "capture-message(1);osk-message;",
            "an observed message goes on to the window");
    calls.clear();
    withhold = 1;
    require(mag_player_win32_message(&msg) == 1 && calls == "capture-message(1);",
            "a withheld quit key reaches neither the dialog nor the window");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string name = argv[1];
    try {
        if (name == "xinput") xinput();
        else if (name == "keyboard") keyboard();
        else if (name == "window") window();
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << name << ": " << error.what() << "\ncalls: " << calls << '\n';
        return 1;
    }
    return 0;
}
