// Portable translations of the Windows desktop capture backend (src/player_input_win32_map.c):
// Raw Input key and mouse reports, XInput readings and the keyboard fallback. Synthetic input only.
#include "player_input_win32_map.h"
#include "player_input_capture.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

uint32_t key(uint16_t code, uint16_t flags = 0, uint16_t vkey = 0) { return mag_win32_raw_key_usage(code, flags, vkey); }

void keys() {
    // Letters, digits and the common controls.
    require(key(0x11) == 26 && key(0x1F) == 22 && key(0x1E) == 4 && key(0x20) == 7, "W S A D");
    require(key(0x12) == 8 && key(0x13) == 21 && key(0x2E) == 6 && key(0x10) == 20, "E R C Q");
    require(key(0x02) == 30 && key(0x0A) == 38 && key(0x0B) == 39, "digits 1, 9 and 0");
    require(key(0x01) == 41 && key(0x0F) == 43 && key(0x39) == 44 && key(0x1C) == 40, "Escape Tab Space Return");
    require(key(0x3B) == 58 && key(0x44) == 67 && key(0x57) == 68 && key(0x58) == 69, "F1 F10 F11 F12");
    // Modifiers, left and right.
    require(key(0x2A) == 225 && key(0x36) == 229, "left and right Shift");
    require(key(0x1D) == 224 && key(0x1D, MAG_WIN32_KEY_E0) == 228, "left and right Control (E0)");
    require(key(0x38) == 226 && key(0x38, MAG_WIN32_KEY_E0) == 230, "left and right Alt (E0)");
    // The arrows and navigation keys carry E0; without it the same codes are the keypad.
    require(key(0x48, MAG_WIN32_KEY_E0) == 82 && key(0x50, MAG_WIN32_KEY_E0) == 81, "Up Down");
    require(key(0x4B, MAG_WIN32_KEY_E0) == 80 && key(0x4D, MAG_WIN32_KEY_E0) == 79, "Left Right");
    require(key(0x47, MAG_WIN32_KEY_E0) == 74 && key(0x4F, MAG_WIN32_KEY_E0) == 77, "Home End");
    require(key(0x52, MAG_WIN32_KEY_E0) == 73 && key(0x53, MAG_WIN32_KEY_E0) == 76, "Insert Delete");
    require(key(0x49, MAG_WIN32_KEY_E0) == 75 && key(0x51, MAG_WIN32_KEY_E0) == 78, "Page Up Page Down");
    require(key(0x48) == 96 && key(0x50) == 90 && key(0x4B) == 92 && key(0x4D) == 94, "keypad 8 2 4 6");
    require(key(0x52) == 98 && key(0x53) == 99 && key(0x4C) == 93, "keypad 0 . 5");
    require(key(0x37) == 85 && key(0x4A) == 86 && key(0x4E) == 87, "keypad * - +");
    require(key(0x35, MAG_WIN32_KEY_E0) == 84 && key(0x1C, MAG_WIN32_KEY_E0) == 88, "keypad / and Enter (E0)");
    require(key(0x35) == 56 && key(0x45) == 83, "/ and Num Lock without E0");
    // Keys left with the system or not keys at all.
    require(key(0x5B, MAG_WIN32_KEY_E0) == 0 && key(0x5C, MAG_WIN32_KEY_E0) == 0, "left and right GUI are not captured");
    require(key(0x1D, MAG_WIN32_KEY_E1) == 0, "Pause (E1 1D) is ignored");
    require(key(0x45, 0, MAG_WIN32_VKEY_NONE) == 0, "the 45 that follows E1 1D is not Num Lock");
    require(key(0x2A, MAG_WIN32_KEY_E0) == 0 && key(0x36, MAG_WIN32_KEY_E0) == 0, "fake shifts are ignored");
    require(key(0x46, MAG_WIN32_KEY_E0) == 0, "Break is not captured");
    require(key(0xFF) == 0 && key(0x80) == 0 && key(0) == 0, "overrun and out-of-table codes");
    // Releases translate the same; the break flag does not change the usage.
    require(key(0x11, MAG_WIN32_KEY_BREAK) == 26 && key(0x48, MAG_WIN32_KEY_E0 | MAG_WIN32_KEY_BREAK) == 82,
            "release reports keep the usage");
    // Everything the table names is a capturable usage.
    for (uint16_t flags : {uint16_t(0), uint16_t(MAG_WIN32_KEY_E0)})
        for (uint16_t code = 0; code < 0x80; ++code) {
            const uint32_t usage = key(code, flags);
            require(usage == 0 || (usage >= 4 && usage <= 231 && usage != 227 && usage != 231), "usages in 4..231, no GUI");
        }
}

void repeats() {
    MagWin32KeyState state{};
    require(mag_win32_key_edge(&state, 26, 1) == 1, "first press is an edge");
    require(mag_win32_key_edge(&state, 26, 1) == 0, "auto-repeat is not");
    require(mag_win32_key_edge(&state, 7, 1) == 1, "keys are independent");
    require(mag_win32_key_edge(&state, 26, 0) == 1, "release is an edge");
    require(mag_win32_key_edge(&state, 26, 0) == 0, "a second release is not");
    mag_win32_key_reset(&state);
    require(mag_win32_key_edge(&state, 7, 0) == 0, "reset forgets held keys");
    mag_win32_key_set(&state, 4);
    require(mag_win32_key_edge(&state, 4, 1) == 0, "a key seeded as held repeats");
    require(mag_win32_key_edge(&state, 4, 0) == 1, "and its release is an edge");
    require(mag_win32_key_edge(&state, 300, 1) == 0, "out-of-range usages are ignored");
}

void mouse() {
    MagWin32MouseEdge edges[10];
    require(mag_win32_raw_mouse_edges(0, edges) == 0, "no buttons");
    unsigned count = mag_win32_raw_mouse_edges(MAG_WIN32_MOUSE_LEFT_DOWN, edges);
    require(count == 1 && edges[0].index == 0 && edges[0].down, "left press");
    count = mag_win32_raw_mouse_edges(MAG_WIN32_MOUSE_RIGHT_UP | MAG_WIN32_MOUSE_MIDDLE_DOWN, edges);
    require(count == 2 && edges[0].index == 1 && !edges[0].down && edges[1].index == 2 && edges[1].down,
            "right release and middle press");
    count = mag_win32_raw_mouse_edges(MAG_WIN32_MOUSE_X1_DOWN | MAG_WIN32_MOUSE_X2_UP, edges);
    require(count == 2 && edges[0].index == 3 && edges[0].down && edges[1].index == 4 && !edges[1].down, "X1 3, X2 4");
    count = mag_win32_raw_mouse_edges(MAG_WIN32_MOUSE_LEFT_DOWN | MAG_WIN32_MOUSE_LEFT_UP, edges);
    require(count == 2 && edges[0].down && !edges[1].down, "a click in one report presses before it releases");
    require(mag_win32_raw_mouse_edges(0x3FF, edges) == 10, "every edge at once");

    double dx = 0, dy = 0;
    require(mag_win32_raw_mouse_motion(0, 5, -3, &dx, &dy) == 1 && dx == 5 && dy == 3,
            "relative motion: right-positive, raw up (negative) becomes up-positive");
    require(mag_win32_raw_mouse_motion(0, -2, 4, &dx, &dy) == 1 && dx == -2 && dy == -4, "left and down");
    dx = dy = 99;
    require(mag_win32_raw_mouse_motion(MAG_WIN32_MOUSE_MOVE_ABSOLUTE, 30000, 20000, &dx, &dy) == 0 && dx == 99 && dy == 99,
            "absolute-position devices do not drive look");
    require(mag_win32_raw_mouse_motion(0, 0, 0, &dx, &dy) == 0, "no movement, no motion");
}

void xinput() {
    MagXInputGamepad pad{};
    MagXInputCapture out{};
    mag_win32_xinput_capture(&pad, &out);
    require(out.buttons == 0, "no buttons");
    for (int axis = 0; axis < 6; ++axis) require(out.axes[axis] == 0, "centred sticks, released triggers");

    const struct {
        uint16_t mask;
        uint32_t button;
    } buttons[] = {{MAG_XINPUT_A, 0}, {MAG_XINPUT_B, 1}, {MAG_XINPUT_X, 2}, {MAG_XINPUT_Y, 3}, {MAG_XINPUT_BACK, 4},
                   {MAG_XINPUT_GUIDE, 5}, {MAG_XINPUT_START, 6}, {MAG_XINPUT_LEFT_THUMB, 7},
                   {MAG_XINPUT_RIGHT_THUMB, 8}, {MAG_XINPUT_LEFT_SHOULDER, 9}, {MAG_XINPUT_RIGHT_SHOULDER, 10},
                   {MAG_XINPUT_DPAD_UP, 11}, {MAG_XINPUT_DPAD_DOWN, 12}, {MAG_XINPUT_DPAD_LEFT, 13},
                   {MAG_XINPUT_DPAD_RIGHT, 14}};
    for (const auto& b : buttons) {
        pad = {};
        pad.buttons = b.mask;
        mag_win32_xinput_capture(&pad, &out);
        require(out.buttons == (1u << b.button), "each XInput button has its SDL GameController number");
    }
    pad = {};
    pad.buttons = 0xFFFF;
    mag_win32_xinput_capture(&pad, &out);
    require(out.buttons == 0x7FFF, "unused bits map to nothing");

    pad = {};
    pad.thumb_lx = 32767;
    pad.thumb_ly = 32767;    // XInput: up
    pad.thumb_rx = -32768;
    pad.thumb_ry = -32768;   // XInput: down
    pad.left_trigger = 255;
    pad.right_trigger = 128;
    mag_win32_xinput_capture(&pad, &out);
    require(out.axes[MAG_INPUT_MOVE_X] == 32767, "left stick right is positive");
    require(out.axes[MAG_INPUT_MOVE_Y] == -32767, "left stick up is negative (SDL: down-positive)");
    require(out.axes[MAG_INPUT_LOOK_X] == -32768, "right stick full left");
    require(out.axes[MAG_INPUT_LOOK_Y] == 32767, "right stick full down is clamped to +32767");
    require(out.axes[MAG_INPUT_LEFT_TRIGGER] == 32767, "full trigger is 32767");
    require(out.axes[MAG_INPUT_RIGHT_TRIGGER] == 16448, "half trigger scales to 0..32767");
    pad.left_trigger = 1;
    mag_win32_xinput_capture(&pad, &out);
    require(out.axes[MAG_INPUT_LEFT_TRIGGER] == 128, "lightest trigger is above zero");
}

void fallback() {
    require(mag_win32_keyboard_fallback_state(int16_t(0x8001), 0) == int16_t(0x8001), "unowned: the system's answer");
    require(mag_win32_keyboard_fallback_state(int16_t(0x8001), 1) == 0, "owned: nothing is held");
    require(mag_win32_keyboard_fallback_state(0, 1) == 0 && mag_win32_keyboard_fallback_state(0, 0) == 0, "released");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string name = argv[1];
    try {
        if (name == "keys") keys();
        else if (name == "repeats") repeats();
        else if (name == "mouse") mouse();
        else if (name == "xinput") xinput();
        else if (name == "fallback") fallback();
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << name << ": " << error.what() << '\n';
        return 1;
    }
    return 0;
}
