#pragma once
/* The MAG Settings Debug page's switches and actions (mag_settings_debug_items in
 * player_settings_text.h), for every host's window. Standard C++17; a C interface for the macOS
 * panel.
 * - controls_trace: the empty file `controls-trace` next to the settings file, which the game
 *   controls runtime looks for at launch (docs/native-controls.md, Census). On creates it, off
 *   deletes it; the state shown is whether it exists.
 * - open_logs: the MAG folder's `logs` folder, which the packaged supervisor names in
 *   MAG_PLAYER_LOGS_ROOT. */
#ifdef __cplusplus
#include <filesystem>
#include <string>

namespace mag {
std::filesystem::path controls_trace_path(const std::filesystem::path &settings_file);
// A switch's state; false for an unknown id.
bool debug_switch(const std::string &id, const std::filesystem::path &settings_file);
// Turns a switch on or off; false with `error` when it could not.
bool set_debug_switch(const std::string &id, const std::filesystem::path &settings_file, bool on, std::string &error);
// The logs folder of this installation; empty outside a packaged launch.
std::filesystem::path logs_folder();
}  // namespace mag
extern "C" {
#endif

/* The same, for the macOS panel. Paths are UTF-8. */
int mag_settings_debug_switch(const char *id, const char *settings_file);
int mag_settings_set_debug_switch(const char *id, const char *settings_file, int on);  /* 1 when done */
/* Copies the logs folder (UTF-8) into `out`; 0 when there is none or it does not fit. */
int mag_settings_logs_folder(char *out, unsigned long capacity);

#ifdef __cplusplus
}
#endif
