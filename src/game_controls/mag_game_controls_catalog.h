#pragma once
/* The game actions the controls adapter supports, their default bindings and profile rules
 * (docs/native-controls.md, 2.3). Action identifiers are the game's action-name tokens. */
#include "player_controls.h"
#include <span>

struct MagGameControlsAction {
    mag::ActionId id;
    const char* label;
    bool digital_controller;  // A controller button may be remapped to it.
    int system_channel = -1;  // System buttons: the pad channel they occupy; -1 otherwise.
};

std::span<const MagGameControlsAction> mag_game_controls_actions();
const MagGameControlsAction* mag_game_controls_action(mag::ActionId);
// The catalog action a watched token stands for (aliases resolved); other tokens map to themselves.
mag::ActionId mag_game_controls_token_action(uint32_t token);
// Bit 0..3 for the four look-axis tokens, 0 for any other token.
unsigned mag_game_controls_look_bit(uint32_t token);
inline bool mag_game_controls_look_token(uint32_t token) { return mag_game_controls_look_bit(token) != 0; }

mag::ControlsProfile mag_game_controls_default_profile();
// Throws std::invalid_argument for a profile the adapter cannot implement.
void mag_game_controls_validate_profile(const mag::ControlsProfile&);
// Adds catalog revision 1 defaults (system buttons) once, never displacing a custom binding.
void mag_game_controls_upgrade_profile(mag::ControlsProfile&);
