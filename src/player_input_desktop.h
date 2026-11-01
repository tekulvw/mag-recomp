#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Inert until explicitly enabled by the native controls adapter. The window is
// borrowed; this API never creates or activates a window.
void mag_input_desktop_attach_window(void* window);
void mag_input_desktop_set_enabled(int enabled);
void mag_input_desktop_refresh(void);
int mag_input_desktop_raw_mouse_available(void);
#ifdef __cplusplus
}
#endif
