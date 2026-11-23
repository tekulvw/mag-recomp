#pragma once
// The settings window's user interface, one per host: the Win32 window
// (player_settings_win32.cpp) and the SDL2 panel (player_settings_sdl.cpp). Both edit a
// SettingsForm (player_settings_form.h) and save through it.
#include "player_settings_form.h"
#include <filesystem>
#include <string>

namespace mag {
enum class SettingsWindowResult { Saved, Cancelled, Failed };
// Shows the window until the player saves or cancels. `setup` is the first-launch variant (its
// Save button reads "Save and play"); `notice` is shown first (a damaged settings file, say).
SettingsWindowResult settings_window_run(SettingsForm& form, const std::filesystem::path& settings, bool setup,
                                         const std::string& notice);
}  // namespace mag
