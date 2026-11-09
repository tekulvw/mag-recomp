/* Portable translations for the Windows desktop capture backend; see player_input_win32_map.h. */
#include "player_input_win32_map.h"
#include <string.h>

/* Scan code set 1 make codes (what Raw Input reports in MakeCode) to USB HID keyboard usages,
 * written from the two public tables. Zero entries are not captured. */
static const uint8_t k_plain[0x80] = {
    [0x01] = 41,                                       /* Escape */
    [0x02] = 30, [0x03] = 31, [0x04] = 32, [0x05] = 33, [0x06] = 34,  /* 1 2 3 4 5 */
    [0x07] = 35, [0x08] = 36, [0x09] = 37, [0x0A] = 38, [0x0B] = 39,  /* 6 7 8 9 0 */
    [0x0C] = 45, [0x0D] = 46,                          /* - = */
    [0x0E] = 42, [0x0F] = 43,                          /* Backspace, Tab */
    [0x10] = 20, [0x11] = 26, [0x12] = 8,  [0x13] = 21, [0x14] = 23,  /* Q W E R T */
    [0x15] = 28, [0x16] = 24, [0x17] = 12, [0x18] = 18, [0x19] = 19,  /* Y U I O P */
    [0x1A] = 47, [0x1B] = 48,                          /* [ ] */
    [0x1C] = 40,                                       /* Return */
    [0x1D] = 224,                                      /* Left Control */
    [0x1E] = 4,  [0x1F] = 22, [0x20] = 7,  [0x21] = 9,  [0x22] = 10,  /* A S D F G */
    [0x23] = 11, [0x24] = 13, [0x25] = 14, [0x26] = 15,               /* H J K L */
    [0x27] = 51, [0x28] = 52, [0x29] = 53,             /* ; ' ` */
    [0x2A] = 225,                                      /* Left Shift */
    [0x2B] = 49,                                       /* \ (also the ISO key left of Return) */
    [0x2C] = 29, [0x2D] = 27, [0x2E] = 6,  [0x2F] = 25, [0x30] = 5,   /* Z X C V B */
    [0x31] = 17, [0x32] = 16,                          /* N M */
    [0x33] = 54, [0x34] = 55, [0x35] = 56,             /* , . / */
    [0x36] = 229,                                      /* Right Shift */
    [0x37] = 85,                                       /* Keypad * */
    [0x38] = 226,                                      /* Left Alt */
    [0x39] = 44,                                       /* Space */
    [0x3A] = 57,                                       /* Caps Lock */
    [0x3B] = 58, [0x3C] = 59, [0x3D] = 60, [0x3E] = 61, [0x3F] = 62,  /* F1..F5 */
    [0x40] = 63, [0x41] = 64, [0x42] = 65, [0x43] = 66, [0x44] = 67,  /* F6..F10 */
    [0x45] = 83,                                       /* Num Lock */
    [0x46] = 71,                                       /* Scroll Lock */
    [0x47] = 95, [0x48] = 96, [0x49] = 97,             /* Keypad 7 8 9 */
    [0x4A] = 86,                                       /* Keypad - */
    [0x4B] = 92, [0x4C] = 93, [0x4D] = 94,             /* Keypad 4 5 6 */
    [0x4E] = 87,                                       /* Keypad + */
    [0x4F] = 89, [0x50] = 90, [0x51] = 91,             /* Keypad 1 2 3 */
    [0x52] = 98, [0x53] = 99,                          /* Keypad 0 . */
    [0x54] = 70,                                       /* Print Screen with Alt held (SysRq) */
    [0x56] = 100,                                      /* ISO key left of Z */
    [0x57] = 68, [0x58] = 69,                          /* F11 F12 */
    [0x59] = 103,                                      /* Keypad = */
    [0x64] = 104, [0x65] = 105, [0x66] = 106, [0x67] = 107, [0x68] = 108, /* F13..F17 */
    [0x69] = 109, [0x6A] = 110, [0x6B] = 111, [0x6C] = 112, [0x6D] = 113, /* F18..F22 */
    [0x6E] = 114, [0x76] = 115,                        /* F23 F24 */
    [0x70] = 136,                                      /* Katakana/Hiragana (International 2) */
    [0x73] = 135,                                      /* Ro (International 1) */
    [0x79] = 138,                                      /* Henkan (International 4) */
    [0x7B] = 139,                                      /* Muhenkan (International 5) */
    [0x7D] = 137,                                      /* Yen (International 3) */
    [0x7E] = 133,                                      /* Keypad , */
};

/* E0-prefixed make codes. Left out: the fake shifts (E0 2A, E0 36) Windows adds around
 * navigation keys, Break (E0 46), the GUI keys (E0 5B, E0 5C), system power keys and media keys. */
static const uint8_t k_e0[0x80] = {
    [0x1C] = 88,                                       /* Keypad Enter */
    [0x1D] = 228,                                      /* Right Control */
    [0x35] = 84,                                       /* Keypad / */
    [0x37] = 70,                                       /* Print Screen */
    [0x38] = 230,                                      /* Right Alt */
    [0x47] = 74,                                       /* Home */
    [0x48] = 82,                                       /* Up */
    [0x49] = 75,                                       /* Page Up */
    [0x4B] = 80,                                       /* Left */
    [0x4D] = 79,                                       /* Right */
    [0x4F] = 77,                                       /* End */
    [0x50] = 81,                                       /* Down */
    [0x51] = 78,                                       /* Page Down */
    [0x52] = 73,                                       /* Insert */
    [0x53] = 76,                                       /* Delete */
    [0x5D] = 101,                                      /* Application (context menu) */
};

uint32_t mag_win32_raw_key_usage(uint16_t make_code, uint16_t flags, uint16_t vkey)
{
    if ((flags & MAG_WIN32_KEY_E1) || vkey == MAG_WIN32_VKEY_NONE || make_code >= 0x80) return 0;
    const uint8_t usage = (flags & MAG_WIN32_KEY_E0) ? k_e0[make_code] : k_plain[make_code];
    /* The table never names the GUI keys; keep the rule explicit should it grow. */
    if (usage == 227 || usage == 231) return 0;
    return usage;
}

unsigned mag_win32_raw_mouse_edges(uint16_t button_flags, MagWin32MouseEdge out[10])
{
    static const uint16_t down[5] = {MAG_WIN32_MOUSE_LEFT_DOWN, MAG_WIN32_MOUSE_RIGHT_DOWN,
                                     MAG_WIN32_MOUSE_MIDDLE_DOWN, MAG_WIN32_MOUSE_X1_DOWN,
                                     MAG_WIN32_MOUSE_X2_DOWN};
    static const uint16_t up[5] = {MAG_WIN32_MOUSE_LEFT_UP, MAG_WIN32_MOUSE_RIGHT_UP,
                                   MAG_WIN32_MOUSE_MIDDLE_UP, MAG_WIN32_MOUSE_X1_UP,
                                   MAG_WIN32_MOUSE_X2_UP};
    unsigned count = 0;
    for (uint32_t index = 0; index < 5; ++index) {
        if (button_flags & down[index]) {
            out[count].index = index;
            out[count++].down = 1;
        }
        if (button_flags & up[index]) {
            out[count].index = index;
            out[count++].down = 0;
        }
    }
    return count;
}

int mag_win32_raw_mouse_motion(uint16_t flags, int32_t last_x, int32_t last_y, double* dx, double* dy)
{
    if ((flags & MAG_WIN32_MOUSE_MOVE_ABSOLUTE) || (last_x == 0 && last_y == 0)) return 0;
    *dx = (double)last_x;
    *dy = -(double)last_y;  /* Raw Input reports down-positive; look wants up-positive. */
    return 1;
}

int mag_win32_key_edge(MagWin32KeyState* state, uint32_t usage, int down)
{
    if (usage > 255) return 0;
    const uint8_t bit = (uint8_t)(1u << (usage & 7));
    uint8_t* byte = &state->down[usage >> 3];
    const int was = (*byte & bit) != 0;
    if (down) *byte |= bit;
    else *byte &= (uint8_t)~bit;
    return was != (down != 0);
}

void mag_win32_key_reset(MagWin32KeyState* state) { memset(state->down, 0, sizeof state->down); }

void mag_win32_key_set(MagWin32KeyState* state, uint32_t usage)
{
    if (usage <= 255) state->down[usage >> 3] |= (uint8_t)(1u << (usage & 7));
}

static int32_t stick_down_positive(int16_t up_positive)
{
    /* XInput's vertical axes are up-positive; the capture layer takes SDL's down-positive. */
    const int32_t value = -(int32_t)up_positive;
    return value > 32767 ? 32767 : value;
}

static int32_t trigger(uint8_t value) { return ((int32_t)value * 32767 + 127) / 255; }

void mag_win32_xinput_capture(const MagXInputGamepad* in, MagXInputCapture* out)
{
    static const struct {
        uint16_t mask;
        uint8_t button;
    } map[] = {
        {MAG_XINPUT_A, 0},           {MAG_XINPUT_B, 1},          {MAG_XINPUT_X, 2},
        {MAG_XINPUT_Y, 3},           {MAG_XINPUT_BACK, 4},       {MAG_XINPUT_GUIDE, 5},
        {MAG_XINPUT_START, 6},       {MAG_XINPUT_LEFT_THUMB, 7}, {MAG_XINPUT_RIGHT_THUMB, 8},
        {MAG_XINPUT_LEFT_SHOULDER, 9}, {MAG_XINPUT_RIGHT_SHOULDER, 10}, {MAG_XINPUT_DPAD_UP, 11},
        {MAG_XINPUT_DPAD_DOWN, 12},  {MAG_XINPUT_DPAD_LEFT, 13}, {MAG_XINPUT_DPAD_RIGHT, 14},
    };
    out->buttons = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (in->buttons & map[i].mask) out->buttons |= 1u << map[i].button;
    out->axes[0] = in->thumb_lx;
    out->axes[1] = stick_down_positive(in->thumb_ly);
    out->axes[2] = in->thumb_rx;
    out->axes[3] = stick_down_positive(in->thumb_ry);
    out->axes[4] = trigger(in->left_trigger);
    out->axes[5] = trigger(in->right_trigger);
}

int16_t mag_win32_keyboard_fallback_state(int16_t system_state, int keyboard_owned)
{
    return keyboard_owned ? 0 : system_state;
}

int mag_win32_raw_input_admitted(int window_focused, int press_or_motion)
{
    return !press_or_motion || window_focused;
}
