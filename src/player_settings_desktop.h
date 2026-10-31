#pragma once
#include "player_settings_portable.h"
// Setup may block before guest startup. In-game modal UI runs on a worker;
// only attach/apply_display are called on the owned backend/window thread.
// The runtime holds game input until the asynchronous transaction completes.
bool mag_desktop_edit(mag::PlayerSettings &settings, bool setup,
                      const char *error);
void mag_desktop_error(const char *message);
void mag_desktop_attach(const mag::PlayerSettings &settings);
bool mag_desktop_confirm_display();
void mag_desktop_apply_display(const mag::PlayerSettings &settings);
bool mag_desktop_settings_requested();
void mag_desktop_request_settings();
