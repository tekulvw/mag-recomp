#pragma once
/* Windows (Win32 game window) seams of the desktop capture backend, player_input_desktop_win32.cpp.
 * player_input_win32_window.c calls these from the SDK's window pump; each never throws and is
 * inert until the controls adapter enables desktop capture. */
#ifdef __cplusplus
extern "C" {
#endif
/* The SDK attaches its game window (an HWND) or detaches it (NULL, before destroying it). */
void mag_input_desktop_win32_window(void* hwnd);
/* Window thread, once per pump before its messages are read: `dialog_active` says the native
 * on-screen keyboard owns input. */
void mag_input_desktop_win32_pump(int dialog_active);
/* Window thread, for every message the pump reads, before anything else sees it (`message` is a
 * MSG*). Observes only; returns nonzero for the one message the pump must not process: a press
 * of Escape on the focused game window while the adapter owns the keyboard and captures it. */
int mag_input_desktop_win32_message(void* message, int dialog_active);
#ifdef __cplusplus
}
#endif
