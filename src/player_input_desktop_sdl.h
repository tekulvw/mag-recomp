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
#endif
