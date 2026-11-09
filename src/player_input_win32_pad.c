/* cellPad's XInput backend and keyboard fallback with the controls adapter (Windows). Compile
 * definitions on libs/input/cellPad.c rename its XInputGetState and GetAsyncKeyState calls to the
 * functions below:
 * - every XInput reading reaches the capture layer (player_input_capture.h) before the settings
 *   input block masks what the SDK sees, as the SDL GameController seam does on macOS and Linux.
 *   cellPad sweeps ports 0..3 in order on each poll, so port 0 starts a capture refresh; each
 *   port keeps one identity for the life of the process.
 * - the keyboard fallback reads nothing while the adapter owns the keyboard, so keys bound to
 *   actions do not also press pad buttons. Whether a controller is present changes nothing here:
 *   the desktop capture backend takes keyboard and mouse beside any XInput device (real or
 *   virtual), and the controller keeps working.
 * Nothing changes for the SDK unless the adapter owns the keyboard or input is blocked. */
#ifndef _WIN32
#error "the XInput seam builds only for Windows"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <stdatomic.h>
#include <string.h>
#include "player_input_capture.h"
#include "player_input_win32_map.h"
#include "player_settings_runtime.h"

_Static_assert(XINPUT_GAMEPAD_A == MAG_XINPUT_A && XINPUT_GAMEPAD_B == MAG_XINPUT_B &&
               XINPUT_GAMEPAD_X == MAG_XINPUT_X && XINPUT_GAMEPAD_Y == MAG_XINPUT_Y &&
               XINPUT_GAMEPAD_BACK == MAG_XINPUT_BACK && XINPUT_GAMEPAD_START == MAG_XINPUT_START &&
               XINPUT_GAMEPAD_LEFT_THUMB == MAG_XINPUT_LEFT_THUMB &&
               XINPUT_GAMEPAD_RIGHT_THUMB == MAG_XINPUT_RIGHT_THUMB &&
               XINPUT_GAMEPAD_LEFT_SHOULDER == MAG_XINPUT_LEFT_SHOULDER &&
               XINPUT_GAMEPAD_RIGHT_SHOULDER == MAG_XINPUT_RIGHT_SHOULDER &&
               XINPUT_GAMEPAD_DPAD_UP == MAG_XINPUT_DPAD_UP && XINPUT_GAMEPAD_DPAD_DOWN == MAG_XINPUT_DPAD_DOWN &&
               XINPUT_GAMEPAD_DPAD_LEFT == MAG_XINPUT_DPAD_LEFT &&
               XINPUT_GAMEPAD_DPAD_RIGHT == MAG_XINPUT_DPAD_RIGHT,
               "XInput button masks");

#define MAG_XINPUT_PORTS 4

static atomic_int s_blocked;
static atomic_int s_opened[MAG_XINPUT_PORTS];
static char s_port_identity[MAG_XINPUT_PORTS];

int mag_player_input_blocked(void) { return atomic_load_explicit(&s_blocked, memory_order_acquire); }
void mag_player_set_input_blocked(int value) { atomic_store_explicit(&s_blocked, !!value, memory_order_release); }

DWORD WINAPI mag_player_xinput_get_state(DWORD port, XINPUT_STATE* state)
{
    if (port == 0) mag_input_capture_backend_begin();
    const DWORD result = XInputGetState(port, state);
    if (port < MAG_XINPUT_PORTS) {
        void* controller = &s_port_identity[port];
        if (!atomic_exchange_explicit(&s_opened[port], 1, memory_order_acq_rel))
            mag_input_capture_controller_open(controller, (unsigned)port);
        const int connected = result == ERROR_SUCCESS && state;
        mag_input_capture_controller_attached(controller, connected);
        if (connected) {
            const MagXInputGamepad reading = {state->Gamepad.wButtons, state->Gamepad.bLeftTrigger,
                                              state->Gamepad.bRightTrigger, state->Gamepad.sThumbLX,
                                              state->Gamepad.sThumbLY, state->Gamepad.sThumbRX,
                                              state->Gamepad.sThumbRY};
            MagXInputCapture capture;
            mag_win32_xinput_capture(&reading, &capture);
            for (unsigned axis = 0; axis < 6; ++axis)
                mag_input_capture_controller_axis(controller, axis, capture.axes[axis]);
            for (unsigned button = 0; button < 15; ++button)
                mag_input_capture_controller_button(controller, button, (capture.buttons >> button) & 1u);
        }
    }
    if (result == ERROR_SUCCESS && state && mag_player_input_blocked())
        memset(&state->Gamepad, 0, sizeof state->Gamepad);
    return result;
}

SHORT WINAPI mag_player_get_async_key_state(int key)
{
    /* Ask the system only when its answer is used. */
    const int owned = mag_input_capture_keyboard_owned();
    return mag_win32_keyboard_fallback_state(owned ? 0 : GetAsyncKeyState(key), owned);
}

/* cellPad.c sees these through the renamed system declarations, and user32's is declared as a DLL
 * import there: its call goes through an import-table pointer. Supply that pointer (and XInput's,
 * whichever way its header declares it) so the link needs no import synthesis, which MinGW's
 * linker does not offer and MSVC's reports as a warning. 64-bit targets name it plainly. */
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
SHORT(WINAPI* __imp_mag_player_get_async_key_state)(int) = mag_player_get_async_key_state;
DWORD(WINAPI* __imp_mag_player_xinput_get_state)(DWORD, XINPUT_STATE*) = mag_player_xinput_get_state;
#endif
