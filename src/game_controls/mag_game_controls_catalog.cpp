#include "mag_game_controls_catalog.h"
#include <algorithm>
#include <stdexcept>
#include <utility>
using namespace mag;
namespace {
constexpr ActionId Fire = 0x58de09cf, Aim = 0xc07aa75a, Jump = 0xa7b30616, Sprint = 0xef8055b7,
    Reload = 0xa2b8ce55, Interact = 0x47cc8c92, Stance = 0x541512ed, CycleWeapons = 0x2c4380c4,
    CycleEquipment = 0xcb535315, Melee = 0xdb59e9a9, Start = 0x163aab24, Select = 0x0a7fd35e,
    DpadUp = 0xa5bc1361, DpadDown = 0x8c2adba2, DpadLeft = 0xeab2acf1, DpadRight = 0x3d898bbf,
    Forward = 0xb164d156, Backward = 0x93115ec5, Left = 0x44533986, Right = 0xf3468eb5;
// Situational actions (catalog revision 2), named by the game's prompt placeholders. Command
// screen was one in revision 2; the game watches it on the Select button, so from revision 3 its
// token stands for Select (Tab presses both, as the controller's Select does).
constexpr ActionId Confirm = 0x33e32a99 /* os_enter */, Cancel = 0x75453127 /* os_cancel */,
    Bleedout = 0x40feec35 /* bleedout */, CommandScreen = 0xc6dcd89f /* commandscreen */,
    TacticalAbilities = 0xa79b7198 /* tcm_abilities */, TacticalFrago = 0x6b7b7c6c /* tcm_frago */,
    TacticalVoice = 0xf69d4193 /* tcm_voicechat */, SeatNext = 0x74700e10 /* vseatnext */,
    Smoke = 0xe3dd5118 /* vsmoke */;
constexpr const char *Prompts = "Prompts and downed", *Commands = "Commands", *Vehicles = "Vehicles", *Voice = "Voice";
constexpr ActionId PushToTalk = MagGameControlsPushToTalk;
constexpr MagGameControlsAction catalog[] = {
    {Forward, "Move forward (vehicles: accelerate)", false}, {Backward, "Move backward (vehicles: brake / reverse)", false},
    {Left, "Strafe left (vehicles: steer left)", false}, {Right, "Strafe right (vehicles: steer right)", false},
    {Fire, "Fire / use equipment", true}, {Aim, "Aim / scope", true}, {Jump, "Jump", true},
    {Sprint, "Sprint", true}, {Reload, "Reload", true}, {Interact, "Interact (hold for objectives)", true},
    {Stance, "Crouch / prone (tap / hold)", true}, {CycleWeapons, "Cycle weapons", true},
    {CycleEquipment, "Cycle equipment", true}, {Melee, "Melee", true},
    {Start, "Start (options / respawn)", true, 12, false, 1}, {Select, "Select (commands)", true, 13, false, 1},
    {DpadUp, "D-pad up (scope / commands)", true, 0, false, 1},
    {DpadDown, "D-pad down (scope / commands)", true, 1, false, 1},
    {DpadLeft, "D-pad left (commands)", true, 2, false, 1}, {DpadRight, "D-pad right (commands)", true, 3, false, 1},
    {Confirm, "Confirm (on-screen prompts)", false, -1, true, 2, Prompts, true},
    {Cancel, "Back (on-screen prompts)", false, -1, true, 2, Prompts, true},
    {Bleedout, "Bleed out (when downed)", false, -1, true, 2, Prompts},
    {TacticalAbilities, "Tactical menu: abilities", false, -1, true, 2, Commands},
    {TacticalFrago, "Tactical menu: FRAGO", false, -1, true, 2, Commands},
    {TacticalVoice, "Tactical menu: voice chat", false, -1, true, 2, Commands},
    {SeatNext, "Change seats", false, -1, true, 2, Vehicles},
    {Smoke, "Smoke screen", false, -1, true, 2, Vehicles},
    // Revision 5: a host action. K on the keyboard; a controller button only if the player binds one.
    {PushToTalk, "Push to talk", true, -1, false, 5, Voice, false, true}};
constexpr ActionId retired[] = {CommandScreen};
// turnleft, turnright, aimup, aimdown.
constexpr uint32_t look_tokens[] = {0xc8a3e21d, 0x941734a2, 0x661cb595, 0xc34f3a66};
// Contexts a binding may use: on foot for gameplay actions; on foot and menu for system buttons.
constexpr uint32_t OnFoot = uint32_t(InputContext::OnFoot);
constexpr uint32_t SystemContexts = OnFoot | uint32_t(InputContext::Menu);
// Controller codes the adapter can remap: SDL buttons 0-4 and 6-14 (Guide, 5, stays reserved)
// and the digital triggers 32 (left) and 33 (right). Analog sticks keep the game's response.
bool remappable_controller_code(uint32_t code) { return code <= 4 || (code >= 6 && code <= 14) || code == 32 || code == 33; }
}  // namespace

std::span<const MagGameControlsAction> mag_game_controls_actions() { return catalog; }
std::span<const ActionId> mag_game_controls_retired_actions() { return retired; }

const MagGameControlsAction* mag_game_controls_action(ActionId id) {
    for (const auto& entry : catalog)
        if (entry.id == id) return &entry;
    return nullptr;
}

ActionId mag_game_controls_token_action(uint32_t token) {
    switch (token) {
    case 0x68c3570d: return Aim;
    case 0x9716c7a1: return Stance;
    case 0x71c8f2f0: return Interact;  // contaction: the held form of action
    case CommandScreen: return Select;  // watched on the Select button
    // Watched on the Start button and the D-pad's up and down, as the census of a match shows: the
    // same buttons, so the same keys (Escape, Up, Down), and prompts naming them show those keys.
    case 0xe88eb86f: return Start;      // respawn
    case 0xf280dd78: return DpadUp;     // scopeup (scope zoom)
    case 0x6bf6ca31: return DpadDown;   // scopedown
    default: return token;
    }
}

mag::ActionId mag_game_controls_downed_alias(ActionId action) { return action == Bleedout ? Confirm : 0; }
mag::ActionId mag_game_controls_seat_alias(ActionId action) { return action == CycleWeapons ? Aim : 0; }
mag::ActionId mag_game_controls_drive_alias(ActionId action) {
    return action == CycleWeapons ? Forward : action == CycleEquipment ? Backward : 0;
}
bool mag_game_controls_menu_button(ActionId action) { return action == Melee; }

uint32_t mag_game_controls_name_token(std::string_view name) {
    uint32_t crc = 0xFFFFFFFFu;
    for (char c : name) {
        const unsigned char byte = (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : static_cast<unsigned char>(c);
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

ActionId mag_game_controls_prompt_action(std::string_view name) {
    const ActionId action = mag_game_controls_token_action(mag_game_controls_name_token(name));
    return mag_game_controls_action(action) ? action : 0;
}

unsigned mag_game_controls_look_bit(uint32_t token) {
    for (unsigned i = 0; i < std::size(look_tokens); ++i)
        if (look_tokens[i] == token) return 1u << i;
    return 0;
}

uint32_t mag_game_controls_pad_layout_key(MagPadButton button) {
    // X Z C V, Return, Space, the arrows, Q E, R T, F G.
    constexpr uint32_t keys[] = {27, 6, 29, 25, 40, 44, 82, 81, 80, 79, 20, 8, 21, 23, 9, 10};
    const auto index = static_cast<size_t>(button);
    return index < std::size(keys) ? keys[index] : 0;
}

ControlsProfile mag_game_controls_default_profile() {
    ControlsProfile profile;
    // Keyboard codes are USB HID usages: W S A D, Space, Left Shift, R, E, C, Q, G, F.
    for (auto [code, id] : {std::pair{26u, Forward}, {22u, Backward}, {4u, Left}, {7u, Right},
                            {44u, Jump}, {225u, Sprint}, {21u, Reload}, {8u, Interact}, {6u, Stance},
                            {20u, CycleWeapons}, {10u, CycleEquipment}, {9u, Melee}})
        profile.bindings.push_back({{InputSource::Keyboard, code}, id});
    profile.bindings.push_back({{InputSource::Mouse, 0}, Fire});
    profile.bindings.push_back({{InputSource::Mouse, 1}, Aim});
    // The game's own controller layout, in SDL button codes: R1/L1 fire/aim, face buttons and stick
    // clicks, and the triggers (33 right, 32 left) for weapon and equipment cycling.
    for (auto [code, id] : {std::pair{10u, Fire}, {9u, Aim}, {0u, Jump}, {7u, Sprint}, {2u, Reload},
                            {1u, Interact}, {3u, Stance}, {33u, CycleWeapons}, {32u, CycleEquipment}, {8u, Melee}})
        profile.bindings.push_back({{InputSource::Controller, code}, id});
    // System buttons: Escape, Tab and the arrows; Start, Back and the D-pad on a controller.
    for (auto [code, id] : {std::pair{41u, Start}, {43u, Select}, {82u, DpadUp}, {81u, DpadDown},
                            {80u, DpadLeft}, {79u, DpadRight}})
        profile.bindings.push_back({{InputSource::Keyboard, code}, id, SystemContexts});
    for (auto [code, id] : {std::pair{6u, Start}, {4u, Select}, {11u, DpadUp}, {12u, DpadDown},
                            {13u, DpadLeft}, {14u, DpadRight}})
        profile.bindings.push_back({{InputSource::Controller, code}, id, SystemContexts});
    // Revision 4: the mouse wheel zooms a scope (scopeup and scopedown, on D-pad up and down), on
    // foot only, beside the arrows.
    profile.bindings.push_back({{InputSource::Mouse, MouseWheelUp}, DpadUp, OnFoot});
    profile.bindings.push_back({{InputSource::Mouse, MouseWheelDown}, DpadDown, OnFoot});
    // Situational actions (keyboard only; a controller presses them natively): X confirms on-screen
    // prompts as it does in menus (the SDK keyboard pad's Cross), Backspace goes back (C, the pad's
    // Circle, is Crouch on foot), Return bleeds out, 1 2 3 for the tactical menu, V and B in vehicles.
    for (auto [code, id] : {std::pair{27u, Confirm}, {42u, Cancel}, {40u, Bleedout},
                            {30u, TacticalAbilities}, {31u, TacticalFrago}, {32u, TacticalVoice},
                            {25u, SeatNext}, {5u, Smoke}})
        profile.bindings.push_back({{InputSource::Keyboard, code}, id});
    // Revision 5: K talks while held (voice chat, a host action). No controller default.
    profile.bindings.push_back({{InputSource::Keyboard, 14}, PushToTalk});
    profile.catalog_revision = MagGameControlsCatalogRevision;
    return profile;
}

std::vector<ActionDescriptor> mag_game_controls_descriptors() {
    constexpr uint32_t Keyboard = 1, Mouse = 2, Controller = 4;
    std::vector<ActionDescriptor> result;
    for (const auto& action : catalog) {
        const bool system = action.system_channel >= 0;
        result.push_back({action.id, action.label, system ? SystemContexts : OnFoot,
                          Keyboard | Mouse | (action.digital_controller ? Controller : 0u),
                          action.group ? action.group : system ? "Native menu / command buttons" : "On-foot actions"});
    }
    return result;
}

void mag_game_controls_validate_profile(const ControlsProfile& profile) {
    mag_controls_validate_profile(profile);
    if (profile.mouse.hip_gain != profile.mouse.ads_gain || profile.mouse.hip_gain != profile.mouse.scoped_gain)
        throw std::invalid_argument("Separate aim-mode mouse gains are not supported yet");
    const auto& c = profile.controller;
    if (c.response != ControllerResponse::Native || c.x_gain != 1 || c.y_gain != 1 || c.hip_gain != 1 ||
        c.ads_gain != 1 || c.scoped_gain != 1 || c.invert_x || c.invert_y || c.inner_x != 0 || c.inner_y != 0 ||
        c.outer_x != 0 || c.outer_y != 0)
        throw std::invalid_argument("Controller sticks currently use the native response and sensitivity");
    for (const auto& binding : profile.bindings) {
        const auto* action = mag_game_controls_action(binding.action);
        const bool contexts_ok = action && (action->system_channel >= 0
            ? binding.contexts && !(binding.contexts & ~SystemContexts)
            : binding.contexts == OnFoot);
        if (!contexts_ok) throw std::invalid_argument("Unsupported on-foot action or context");
        if (binding.control.source == InputSource::Controller &&
            (!action->digital_controller || !remappable_controller_code(binding.control.code)))
            throw std::invalid_argument("Use a supported controller button or trigger; analog sticks and Guide remain native");
    }
}

namespace {
bool uses_on_foot(const ControlsProfile& profile, PhysicalControl control) {
    return std::any_of(profile.bindings.begin(), profile.bindings.end(),
                       [&](const auto& b) { return b.control == control && (b.contexts & OnFoot); });
}
// Revision 2 to 3: Confirm moves from Return to X and Bleed out from X to Return, each only where
// the player left the old default and the new key is free; Command screen is retired, and a key
// the player chose for it becomes an on-foot key for Select where Select has room for it.
void upgrade_from_revision_2(ControlsProfile& profile) {
    const auto at = [&](ActionId action, uint32_t code) {
        return std::find_if(profile.bindings.begin(), profile.bindings.end(), [&](const auto& b) {
            return b.action == action && b.control == PhysicalControl{InputSource::Keyboard, code} && b.contexts == OnFoot;
        });
    };
    struct Move { ActionId action; uint32_t from, to; };
    std::vector<Move> moving;
    for (Move move : {Move{Confirm, 40, 27}, Move{Bleedout, 27, 40}}) {
        const auto it = at(move.action, move.from);
        if (it == profile.bindings.end()) continue;
        profile.bindings.erase(it);
        moving.push_back(move);
    }
    std::vector<ControlBinding> commands;
    for (auto it = profile.bindings.begin(); it != profile.bindings.end();) {
        if (it->action != CommandScreen) { ++it; continue; }
        if (!(it->control == PhysicalControl{InputSource::Keyboard, 16} && it->contexts == OnFoot)) commands.push_back(*it);
        it = profile.bindings.erase(it);
    }
    for (const auto& move : moving) {
        if (uses_on_foot(profile, {InputSource::Keyboard, move.to})) {
            // The new key is taken: keep the old one rather than leave the action unbound.
            if (!uses_on_foot(profile, {InputSource::Keyboard, move.from}))
                profile.bindings.push_back({{InputSource::Keyboard, move.from}, move.action});
            continue;
        }
        profile.bindings.push_back({{InputSource::Keyboard, move.to}, move.action});
    }
    for (auto binding : commands) {
        const auto select_keys = std::count_if(profile.bindings.begin(), profile.bindings.end(), [&](const auto& b) {
            return b.action == Select && b.control.source != InputSource::Controller && (b.contexts & OnFoot);
        });
        if (select_keys >= 2 || uses_on_foot(profile, binding.control)) continue;
        binding.action = Select;
        binding.contexts = OnFoot;
        profile.bindings.push_back(binding);
    }
}

// Revision 4: wheel up and down on foot for D-pad up and down (scope zoom), each only where the
// player uses that wheel step for nothing on foot and the D-pad direction has room for a second
// keyboard or mouse control there.
void add_scope_wheel(ControlsProfile& profile) {
    for (auto [code, action] : {std::pair{MouseWheelUp, DpadUp}, {MouseWheelDown, DpadDown}}) {
        const PhysicalControl wheel{InputSource::Mouse, code};
        const auto keys = std::count_if(profile.bindings.begin(), profile.bindings.end(), [&](const auto& b) {
            return b.action == action && b.control.source != InputSource::Controller && (b.contexts & OnFoot);
        });
        if (keys >= 2 || uses_on_foot(profile, wheel)) continue;
        profile.bindings.push_back({wheel, action, OnFoot});
    }
}
// Revision 5: K for Push to talk, only where the player uses K for nothing on foot. Never a
// controller button: the player chooses one if they want it.
void add_push_to_talk(ControlsProfile& profile) {
    const bool bound = std::any_of(profile.bindings.begin(), profile.bindings.end(), [](const auto& b) {
        return b.action == PushToTalk && b.control.source != InputSource::Controller;
    });
    const PhysicalControl k{InputSource::Keyboard, 14};
    if (bound || uses_on_foot(profile, k)) return;
    profile.bindings.push_back({k, PushToTalk, OnFoot});
}
void upgrade_before_revision_2(ControlsProfile& profile);
}  // namespace

void mag_game_controls_upgrade_profile(ControlsProfile& profile) {
    const unsigned from = profile.catalog_revision;
    if (from > MagGameControlsCatalogRevision)
        throw std::invalid_argument("Controls catalog is newer than this build");
    if (from == MagGameControlsCatalogRevision) return;
    if (from == 2) upgrade_from_revision_2(profile);
    else if (from < 2) upgrade_before_revision_2(profile);
    if (from < 4) add_scope_wheel(profile);
    add_push_to_talk(profile);
    profile.catalog_revision = MagGameControlsCatalogRevision;
}

namespace {
void upgrade_before_revision_2(ControlsProfile& profile) {
    // Before revision 2 no retired action existed; drop any stray one defensively.
    std::erase_if(profile.bindings, [](const auto& b) { return !mag_game_controls_action(b.action); });
    for (auto binding : mag_game_controls_default_profile().bindings) {
        if (mag_game_controls_action(binding.action)->revision <= profile.catalog_revision) continue;
        const bool present = std::any_of(profile.bindings.begin(), profile.bindings.end(), [&](const auto& b) {
            return b.action == binding.action && b.control.source == binding.control.source;
        });
        // Never displace a physical control the player already uses in a context.
        for (const auto& b : profile.bindings)
            if (b.control == binding.control) binding.contexts &= ~b.contexts;
        // A gameplay action's binding is on foot or nothing.
        if (mag_game_controls_action(binding.action)->system_channel < 0 && binding.contexts != OnFoot) continue;
        if (!present && binding.contexts) profile.bindings.push_back(binding);
    }
}
}  // namespace
