#pragma once
/* Portable translations for the Windows desktop capture backend (player_input_desktop_win32.cpp)
 * and its XInput seam (player_input_win32_pad.c). Nothing here includes a Windows header, so the
 * translations are tested on any host; the Windows build checks the restated constants against
 * the system headers. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Raw Input keyboard flags (RAWKEYBOARD.Flags). */
enum { MAG_WIN32_KEY_BREAK = 0x1, MAG_WIN32_KEY_E0 = 0x2, MAG_WIN32_KEY_E1 = 0x4 };
/* RAWKEYBOARD.VKey of a key report that is only part of an escape sequence. */
enum { MAG_WIN32_VKEY_NONE = 0xFF };
/* Raw Input mouse flags: RAWMOUSE.usFlags and RAWMOUSE.usButtonFlags. */
enum { MAG_WIN32_MOUSE_MOVE_ABSOLUTE = 0x1 };
enum {
    MAG_WIN32_MOUSE_LEFT_DOWN = 0x001, MAG_WIN32_MOUSE_LEFT_UP = 0x002,
    MAG_WIN32_MOUSE_RIGHT_DOWN = 0x004, MAG_WIN32_MOUSE_RIGHT_UP = 0x008,
    MAG_WIN32_MOUSE_MIDDLE_DOWN = 0x010, MAG_WIN32_MOUSE_MIDDLE_UP = 0x020,
    MAG_WIN32_MOUSE_X1_DOWN = 0x040, MAG_WIN32_MOUSE_X1_UP = 0x080,
    MAG_WIN32_MOUSE_X2_DOWN = 0x100, MAG_WIN32_MOUSE_X2_UP = 0x200
};
/* XInput gamepad buttons (XINPUT_GAMEPAD.wButtons). The guide bit is reported only by the
 * extended state query; it is mapped when present. */
enum {
    MAG_XINPUT_DPAD_UP = 0x0001, MAG_XINPUT_DPAD_DOWN = 0x0002, MAG_XINPUT_DPAD_LEFT = 0x0004,
    MAG_XINPUT_DPAD_RIGHT = 0x0008, MAG_XINPUT_START = 0x0010, MAG_XINPUT_BACK = 0x0020,
    MAG_XINPUT_LEFT_THUMB = 0x0040, MAG_XINPUT_RIGHT_THUMB = 0x0080, MAG_XINPUT_LEFT_SHOULDER = 0x0100,
    MAG_XINPUT_RIGHT_SHOULDER = 0x0200, MAG_XINPUT_GUIDE = 0x0400, MAG_XINPUT_A = 0x1000,
    MAG_XINPUT_B = 0x2000, MAG_XINPUT_X = 0x4000, MAG_XINPUT_Y = 0x8000
};

/* The USB HID keyboard usage (4..231) of a Raw Input key report, or 0 when the report is not a
 * captured key: an escape-sequence fragment (VKey 0xFF), an E1 sequence (Pause), the fake shifts
 * Windows adds around navigation keys, system and media keys, and the left and right GUI keys,
 * which stay with the system. `vkey` may be 0 when unknown. */
uint32_t mag_win32_raw_key_usage(uint16_t make_code, uint16_t flags, uint16_t vkey);

/* Mouse-button edges of one Raw Input mouse report, in report order (each button's press before
 * its release). Indices: 0 left, 1 right, 2 middle, 3 X1, 4 X2. Returns the count (at most 10). */
typedef struct {
    uint32_t index;
    int down;
} MagWin32MouseEdge;
unsigned mag_win32_raw_mouse_edges(uint16_t button_flags, MagWin32MouseEdge out[10]);

/* Relative look motion of one Raw Input mouse report: dx right-positive, dy up-positive. Returns 0
 * (no motion) for absolute-position devices (tablets, remote desktop, some virtual machines) and
 * for reports without movement. */
int mag_win32_raw_mouse_motion(uint16_t flags, int32_t last_x, int32_t last_y, double* dx, double* dy);

/* Physical key tracking that tells a new press from the keyboard's auto-repeat, which Raw Input
 * reports as further presses. */
typedef struct {
    uint8_t down[32];
} MagWin32KeyState;
/* Records the report; returns 1 when it changes the key's state (a press of a released key or a
 * release of a held one) and 0 for a repeat. Usages outside 0..255 return 0. */
int mag_win32_key_edge(MagWin32KeyState* state, uint32_t usage, int down);
void mag_win32_key_reset(MagWin32KeyState* state);
void mag_win32_key_set(MagWin32KeyState* state, uint32_t usage);

/* One XInput gamepad reading. */
typedef struct {
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} MagXInputGamepad;
/* The reading as the capture layer takes it (player_input_capture.h): buttons as a mask of SDL
 * GameController button numbers (A 0, B 1, X 2, Y 3, Back 4, Guide 5, Start 6, left stick 7,
 * right stick 8, left shoulder 9, right shoulder 10, D-pad up 11, down 12, left 13, right 14),
 * axes in MagInputAxis order: sticks right/down-positive in -32768..32767, triggers 0..32767. */
typedef struct {
    uint32_t buttons;
    int32_t axes[6];
} MagXInputCapture;
void mag_win32_xinput_capture(const MagXInputGamepad* in, MagXInputCapture* out);

/* What the SDK's keyboard fallback reads for one key: nothing while the controls adapter owns the
 * keyboard, otherwise the system's answer unchanged. */
int16_t mag_win32_keyboard_fallback_state(int16_t system_state, int keyboard_owned);

#ifdef __cplusplus
}
#endif
