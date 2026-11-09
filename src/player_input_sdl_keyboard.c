/* The SDL game window's keyboard pad on Linux, with the controls adapter. Compile definitions on
 * libs/input/pad_sdl_keyboard.c rename the SDK's two entry points to mag_sdk_*; these take their
 * place:
 * - the window pump's per-pump keyboard sample first runs the desktop capture backend on the same
 *   thread with the same window, then the SDK's own sample, unchanged;
 * - the keyboard pad state cellPad reads is neutral while the adapter owns the keyboard, so keys
 *   bound to actions (Q, E, Space, Return, ...) do not also press pad buttons. The window flag is
 *   kept, so the SDK's ownership of controller port 0 does not change. */
#include "player_input_capture.h"
#include "player_input_desktop_sdl.h"
#include "libs/input/pad_sdl_keyboard.h"

#ifndef PAD_HAS_SDL_KEYBOARD
#error "build with PS3RECOMP_PAD_SDL_KEYBOARD=1 on an SDL (non-Apple, non-Windows) host"
#endif

void mag_sdk_pad_sdl_keyboard_sample(void* sdl_window, int suppressed);
PS3RecompSDLKeyboardPadState mag_sdk_pad_sdl_keyboard_state(void);

void ps3recomp_pad_sdl_keyboard_sample(void* sdl_window, int suppressed)
{
    mag_input_desktop_sdl_pump(sdl_window, suppressed);
    mag_sdk_pad_sdl_keyboard_sample(sdl_window, suppressed);
}

PS3RecompSDLKeyboardPadState ps3recomp_pad_sdl_keyboard_state(void)
{
    PS3RecompSDLKeyboardPadState state = mag_sdk_pad_sdl_keyboard_state();
    if (mag_input_capture_keyboard_owned()) {
        PS3RecompSDLKeyboardPadState neutral = {0, 128, 128, 128, 128, state.window};
        return neutral;
    }
    return state;
}
