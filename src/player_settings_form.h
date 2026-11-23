#pragma once
// The settings window of Windows and Linux players, without any user interface: what the Win32
// window (player_settings_win32.cpp) and the SDL2 panel (player_settings_sdl.cpp) show and edit.
// Fields are kept as the player typed them; validation and storage are the portable settings
// file's (player_settings_portable.h) and the controls service's (player_controls_settings.h),
// so no rule is restated here.
#include "player_controls.h"
#include "player_settings_portable.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mag {
enum class SettingsDevice { KeyboardMouse, Controller };
// Binding scopes the window edits one at a time (player-controls-settings.md).
constexpr uint32_t settings_scope_on_foot = 1, settings_scope_menu = 16;

struct SettingsBindingRow {
    ActionId action = 0;
    std::string label, group;
    std::string slot[2];  // "Unbound" when empty
    bool bound[2] = {false, false};
};

struct SettingsForm {
    // General.
    std::string server, tcp_port, udp_port, player_name;
    std::string ticket_file;  // kept as stored; the window does not edit it
    bool borderless = false;
    // Controls, when the runtime registered its catalog.
    bool controls = false;
    std::string controls_error;  // the saved profile could not be read; Reset offers defaults
    ControlsProfile profile;
    bool controls_dirty = false;
    std::optional<InputMode> mode_override;  // startup override the panel points out
};

// Reads the settings file (defaults when it does not exist) and, when registered, the controls
// profile. False with an explanation when the settings file is damaged; the form then holds
// defaults and saving it replaces the file only on an explicit Save.
bool settings_form_load(const std::filesystem::path& settings, SettingsForm& form, std::string& error);
// Checks every field and writes both files. Nothing is written when a field is invalid; `error`
// names the first problem.
bool settings_form_save(const std::filesystem::path& settings, SettingsForm& form, std::string& error);

std::vector<SettingsBindingRow> settings_form_rows(const SettingsForm&, SettingsDevice, uint32_t scope);
// Assigns `control` to one slot of an action in a scope. When the control is bound to another
// action the form is unchanged and the conflicting action's label is returned in `conflict`;
// call again with replace=true to take it over. False with `error` when the device or action does
// not support it.
bool settings_form_bind(SettingsForm&, uint32_t scope, ActionId, unsigned slot, PhysicalControl, bool replace,
                        std::string& conflict, std::string& error);
bool settings_form_clear(SettingsForm&, SettingsDevice, uint32_t scope, ActionId, unsigned slot, std::string& error);
bool settings_form_reset(SettingsForm&, SettingsDevice, std::string& error);
// Mouse sensitivity as a multiple of the default look rate (0.1 to 5), and vertical inversion.
double settings_form_mouse_sensitivity(const SettingsForm&);
void settings_form_set_mouse_sensitivity(SettingsForm&, double multiple);
void settings_form_set_invert_y(SettingsForm&, bool invert);
void settings_form_set_mode(SettingsForm&, InputMode);
std::string settings_form_control_name(PhysicalControl);
}  // namespace mag
