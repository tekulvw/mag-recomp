/* The Win32 game window's pump with the controls adapter. The SDK's Vulkan and D3D12 backends
 * create the game window and pump its messages on the window's own thread, and share that pump
 * with the native on-screen keyboard through three calls. Compile definitions on
 * libs/video/rsx_vulkan_backend.c and libs/video/rsx_d3d12_backend.c rename those calls to the
 * functions below, which run the desktop capture backend on the same thread and then the
 * on-screen keyboard's own function, unchanged:
 * - attaching or detaching the window tells the backend first (it releases the cursor before the
 *   window goes);
 * - the per-pump call runs the keyboard's pump, then the backend with the keyboard's dialog flag;
 * - the per-message call lets the backend observe the message first. It only ever withholds a
 *   press of Escape that the window would treat as quit while the adapter owns the keyboard
 *   (player_input_desktop_win32.h); everything else goes on to the keyboard's dialog and the
 *   window, as before. */
#include "player_input_desktop_win32.h"
#include "libs/misc/osk_native.h"

#if !defined(_WIN32) || !defined(OSK_NATIVE_HAS_WIN32_HOST)
#error "build with PS3RECOMP_NATIVE_OSK_WINDOWS=1 on Windows: the window pump seams belong to its host"
#endif

void mag_player_win32_set_window(void* window)
{
    mag_input_desktop_win32_window(window);
    osk_native_win32_set_window(window);
}

void mag_player_win32_pump(void)
{
    osk_native_win32_pump();
    mag_input_desktop_win32_pump(osk_native_win32_input_active());
}

int mag_player_win32_message(void* message)
{
    if (mag_input_desktop_win32_message(message, osk_native_win32_input_active())) return 1;
    return osk_native_win32_handle_message(message);
}
