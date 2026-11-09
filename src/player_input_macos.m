#include "player_settings_runtime.h"
#include "player_input_capture.h"
#include "libs/input/pad_macos.h"

// The window keyboard pad gives way to the settings panel and to the controls adapter's keyboard
// ownership. The SDL controller seam and the input block live in player_input_sdl_pad.c.
PS3RecompMacOSPadState mag_sdk_pad_macos_state(void);
uint16_t mag_sdk_pad_macos_buttons(void);
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void) {
    if(mag_player_input_blocked() || mag_input_capture_keyboard_owned())return (PS3RecompMacOSPadState){0,128,128,128,128};
    return mag_sdk_pad_macos_state();
}
uint16_t ps3recomp_pad_macos_buttons(void) {
    return (mag_player_input_blocked() || mag_input_capture_keyboard_owned())?0:mag_sdk_pad_macos_buttons();
}
