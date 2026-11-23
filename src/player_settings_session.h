#pragma once
/* The settings window of Windows and Linux players (docs/player-settings.md). It runs as a
 * separate process of the engine binary (`MagEngine --mag-player-settings`), so it never shares
 * the game window's thread, message pump or SDL event queue, and the game, an online match
 * included, keeps running behind it:
 * - in game, F10 in the game window opens it (mag_player_settings_open); gameplay input is held
 *   while it is open, and saved controls apply to the running game when it closes. Connection
 *   and display changes apply at the next launch.
 * - before the game starts, setup (with Play MAG): on Windows the packaged supervisor opens it at
 *   every launch (`MagEngine --mag-player-settings-setup`) and sets MAG_SKIP_SETUP for the
 *   launcher; elsewhere the launcher opens it on the first launch (mag_player_settings_setup).
 * Inert unless the packaged supervisor supplies MAG_PLAYER_SETTINGS_PATH. */
#ifdef __cplusplus
extern "C" {
#endif

/* main(): when argv[1] is the settings-window switch, runs the window and returns its exit code
 * (0 saved, 3 cancelled, 1 failed) in *code; returns 0 for any other command line. */
int mag_player_settings_window_command(int argc, char** argv, int* code);
#define MAG_PLAYER_SETTINGS_SWITCH "--mag-player-settings"
#define MAG_PLAYER_SETTINGS_SETUP_SWITCH "--mag-player-settings-setup"

/* In game, from any thread: opens the window unless it is already open. */
void mag_player_settings_open(void);
int mag_player_settings_is_open(void);

/* Launcher, before the game starts: runs the window (from the engine binary the supervisor
 * names) and waits. Returns 1 when the player saved, 0 when cancelled or unavailable. */
int mag_player_settings_setup(void);
/* Whether the launcher should open setup: never with MAG_SKIP_SETUP; otherwise when there is no
 * settings file yet, or (Windows) Shift is held. */
int mag_player_settings_setup_wanted(int settings_exist);

/* Window seams (with the controls adapter's pump seams): F10 in the game window. */
#ifdef _WIN32
/* The game window as it is attached: kept for the settings window's display preview, and sized to
 * the saved output resolution (MAG_PLAYER_OUTPUT_SIZE, set by the launch settings) when windowed. */
void mag_player_settings_win32_window(void* window);
/* One message of the game window's pump; nonzero when it was F10 and has been handled. F10 is
 * otherwise the Windows menu key, and its default handling would stall the game window. */
int mag_player_settings_win32_message(const void* msg);
#else
/* The SDL game window's pump, after its events are polled (NULL when the window goes). */
void mag_player_settings_sdl_pump(void* sdl_window);
#endif

#ifdef __cplusplus
}
#endif
