#pragma once
/* The game actions the controls adapter supports, their default bindings and profile rules
 * (docs/native-controls.md, 2.3). Action identifiers are the game's action-name tokens: CRC-32 of
 * the lower-cased action name, the same names the game's prompts use as [Name] placeholders. */
#include "player_controls.h"
#include "player_controls_settings.h"
#include <span>
#include <string_view>
#include <vector>

struct MagGameControlsAction {
    mag::ActionId id;
    const char* label;
    bool digital_controller;  // A controller button may be remapped to it.
    int system_channel = -1;  // System buttons: the pad channel they occupy; -1 otherwise.
    // Situational actions (downed, HUD prompts, commands, vehicles): recognized by their own token
    // wherever the game watches them, never required for the on-foot contract and never used to
    // name a channel (docs/native-controls.md, 7.3 and 7.4).
    bool optional = false;
    unsigned revision = 0;  // Catalog revision that added it (upgrade adds its defaults once).
    const char* group = nullptr;  // Settings group; null: by kind.
    // Menu buttons (Confirm, Back): only menus read them, from the keyboard pad, so the adapter never
    // takes them over (docs/native-controls.md, 2.3 and 7.3); their keys still name prompts.
    bool menu_prompt = false;
    // Host actions (Push to talk): MAG's own, read by the host and never fed to the game. They
    // have no game token, so the adapter never maps, requires or answers them.
    bool host = false;
};

constexpr unsigned MagGameControlsCatalogRevision = 5;
// Push to talk (catalog revision 5): CRC-32 of "mag_push_to_talk", a name no game prompt uses.
constexpr mag::ActionId MagGameControlsPushToTalk = 0xd8c5452f;

std::span<const MagGameControlsAction> mag_game_controls_actions();
// Actions an older catalog had. A saved profile may still bind them, so loading accepts them;
// upgrading removes them (or moves a player's own key to the action that replaced them).
std::span<const mag::ActionId> mag_game_controls_retired_actions();
const MagGameControlsAction* mag_game_controls_action(mag::ActionId);
// The catalog action a watched token stands for (aliases resolved); other tokens map to themselves.
mag::ActionId mag_game_controls_token_action(uint32_t token);
// The game's token for an action name (CRC-32 of the lower-cased name).
uint32_t mag_game_controls_name_token(std::string_view name);
// The catalog action a prompt placeholder names ("Action" in "[Action]"), aliases resolved; 0 when
// the name is no catalog action.
mag::ActionId mag_game_controls_prompt_action(std::string_view name);
// While the player is downed, the second action whose keys also press `action` on the player's own
// handler (docs/native-controls.md, 2.3): Confirm for Bleed out, as the console's Cross does both.
// 0 for every other action.
mag::ActionId mag_game_controls_downed_alias(mag::ActionId action);
// On a mounted weapon's or vehicle seat's own handler (docs/native-controls.md, 2.3 and 7.3), the
// second action whose keys also press `action` there: Aim (the right mouse button) for the control on
// Cycle weapons' button, R2, which fires a turret's secondary weapon. 0 for every other action.
mag::ActionId mag_game_controls_seat_alias(mag::ActionId action);
// Bit 0..3 for the four look-axis tokens, 0 for any other token.
unsigned mag_game_controls_look_bit(uint32_t token);
inline bool mag_game_controls_look_token(uint32_t token) { return mag_game_controls_look_bit(token) != 0; }

// PlayStation buttons, and the key the SDK's keyboard pad uses for each (pad_keyboard_layout.h):
// the key that presses a button while that pad is live (an in-match menu: docs/native-controls.md,
// 8.2). USB HID usages.
enum class MagPadButton { Cross, Circle, Square, Triangle, Start, Select, Up, Down, Left, Right, L1, R1, L2, R2, L3, R3 };
uint32_t mag_game_controls_pad_layout_key(MagPadButton);

mag::ControlsProfile mag_game_controls_default_profile();
// Throws std::invalid_argument for a profile the adapter cannot implement.
void mag_game_controls_validate_profile(const mag::ControlsProfile&);
// One settings descriptor per catalog action: system buttons in OnFoot and Menu with their own
// group, controller sources only where a controller button may be remapped.
std::vector<mag::ActionDescriptor> mag_game_controls_descriptors();
// Adds the defaults of actions newer than the profile's catalog revision once, never displacing a
// custom binding; moves defaults the catalog changed only where the player left them; removes
// retired actions, keeping a player's own key on the action that replaced them where it fits.
void mag_game_controls_upgrade_profile(mag::ControlsProfile&);
