#pragma once
#include "player_controls.h"
#include <filesystem>
#include <functional>
#include <string>
namespace mag {
struct ActionDescriptor {
    ActionId id=0;
    std::string label;
    uint32_t contexts=1;
    uint32_t sources=7; // Keyboard=1, Mouse=2, Controller=4.
    std::string group={};
};
struct ControlsSettingsRegistration {
    ControlsProfile defaults;
    std::vector<ActionDescriptor> actions;
    std::filesystem::path path;
    std::optional<InputMode> mode_override; // Startup-only override; Apply uses the selected saved mode.
    // Pure capability validation, followed after durable save by a queue-only
    // apply callback. The runtime installs the copied profile on its own thread.
    std::function<void(const ControlsProfile&)> validate;
    std::function<void(const ControlsProfile&)> apply;
    std::function<void(ControlsProfile&)> upgrade; // Pure, copied in-memory catalog migration.
};
void mag_controls_settings_register(ControlsSettingsRegistration);
void mag_controls_settings_unregister();
bool mag_controls_settings_available();
ControlsSettingsRegistration mag_controls_settings_registration();
ControlsProfile mag_controls_settings_load();
void mag_controls_settings_save(const ControlsProfile&);
// Slot 0=primary,1=secondary. conflict returns an action ID without mutating p.
ActionId mag_controls_settings_edit(ControlsProfile& p,ControlBinding,unsigned slot,bool clear=false,bool replace=false);
void mag_controls_settings_reset(ControlsProfile& p,bool controller);
std::string mag_controls_settings_control_name(PhysicalControl);
}
