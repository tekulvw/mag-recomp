#include "player_settings_runtime.h"
#include "player_input_capture.h"
#include "player_input_keyboard_pad_gate.h"
#include "libs/input/pad_macos.h"

// The window keyboard pad gives way to the settings panel and to the controls adapter while it
// plays on-foot gameplay with the keyboard; keys still held when it comes back need a release first
// (player_input_keyboard_pad_gate.h). The SDL controller seam and the input block live in
// player_input_sdl_pad.c.
PS3RecompMacOSPadState mag_sdk_pad_macos_state(void);
uint16_t mag_sdk_pad_macos_buttons(void);
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void) {
    static MagKeyboardPadGate gate;
    PS3RecompMacOSPadState state = mag_sdk_pad_macos_state();
    mag_keyboard_pad_gate_snapshot(&gate, mag_player_input_blocked() || mag_input_capture_keyboard_pad_muted(),
                                   &state.buttons, &state.analog_lx, &state.analog_ly, &state.analog_rx,
                                   &state.analog_ry);
    return state;
}
uint16_t ps3recomp_pad_macos_buttons(void) {
    return ps3recomp_pad_macos_state().buttons;
}
