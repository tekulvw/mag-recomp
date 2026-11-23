#pragma once
/* Linux (SDL2 game window) seam of the desktop capture backend, player_input_desktop_sdl.cpp. */
#ifdef __cplusplus
extern "C" {
#endif
/* Window-pump thread, once per pump after the pump's events were polled: `sdl_window` is the game
 * window (NULL when it goes away) and `dialog_active` says a native dialog owns the keyboard.
 * Never throws; inert until the controls adapter enables desktop capture. */
void mag_input_desktop_sdl_pump(void* sdl_window, int dialog_active);
#ifdef __cplusplus
}

namespace mag {
// What a pump sees of the mouse lock. With a global pointer (the X11 driver) the pointer and the
// window are in screen coordinates. Other drivers (Wayland, KMSDRM) report neither: SDL hands back
// the pointer relative to the window that has it, and a window position SDL made up itself (where
// it asked the window to go, or the origin of the display the window entered), so the two cannot
// be compared; there the pointer only has to be inside the game window, and no other window may
// have it.
struct SdlLockProbe {
    bool relative = false, grabbed = false, global_pointer = false;
    bool other_window = false;  // without a global pointer: another window has the pointer
    int pointer_x = 0, pointer_y = 0, window_x = 0, window_y = 0, width = 0, height = 0;
};
// While captured: is the lock (relative mode and the window grab) still in place, with the
// pointer inside the game window? Exposed for the synthetic SDL fixture.
bool sdl_desktop_lock_held(const SdlLockProbe& probe);
}  // namespace mag
#endif
