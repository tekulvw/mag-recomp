#pragma once
// The settings window's user interface, one per host: the Win32 window
// (player_settings_win32.cpp) and the SDL2 panel (player_settings_sdl.cpp). Both edit a
// SettingsForm (player_settings_form.h) and save through it.
#include "player_settings_form.h"
#include <filesystem>
#include <string>

namespace mag {
enum class SettingsWindowResult { Saved, Cancelled, Failed };
struct SettingsWindowOptions {
    // The launch's setup window (with Play MAG); otherwise the window F10 opens in game.
    bool setup = false;
    // Shown first, as an error (a damaged settings file, or a server without a player name).
    std::string notice;
    // Windows: the game window, for the display preview (none in setup: a preview window stands
    // in), and an event to signal whenever controls are saved, so the game applies them at once.
    void* game_window = nullptr;
    void* controls_saved = nullptr;
    // Linux: a pipe to the game process, for the display preview ("display ...") and saved
    // controls ("controls"); -1 before the game (a preview window stands in).
    int game_events = -1;
};
// Shows the window until the player closes it. Saved: setup chose Play MAG, or (in game) something
// was saved; Cancelled: setup was cancelled, or nothing was saved.
SettingsWindowResult settings_window_run(SettingsForm& form, const std::filesystem::path& settings,
                                         const SettingsWindowOptions& options);
#ifdef _WIN32
// Opens the account portal's page (the default: the browser); tests replace it.
extern void (*settings_window_open_page)(const std::string& url);
// Stores the area the window must fit in, as left, top, right and bottom in the window's pixels
// (the default, null: its monitor's work area); tests replace it to stand in for a smaller screen.
extern void (*settings_window_work_area)(void* window, long area[4]);
#endif
}  // namespace mag
