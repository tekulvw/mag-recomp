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
 * MSG*). Observes only; returns nonzero for the messages the pump must not process: a press of
 * Escape on the focused game window while the adapter owns the keyboard and captures it, and while
 * capture is installed the messages that would put the window in its menu mode (a lone Alt or F10
 * released, Alt with a character key), whose modal loop stalls the pump. */
int mag_input_desktop_win32_message(void* message, int dialog_active);
#ifdef __cplusplus
}
#endif
