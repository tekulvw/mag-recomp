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
constexpr MagGameControlsAction catalog[] = {
    {Forward, "Move forward", false}, {Backward, "Move backward", false},
    {Left, "Strafe left", false}, {Right, "Strafe right", false},
    {Fire, "Fire / use equipment", true}, {Aim, "Aim / scope", true}, {Jump, "Jump", true},
    {Sprint, "Sprint", true}, {Reload, "Reload", true}, {Interact, "Interact (hold for objectives)", true},
    {Stance, "Crouch / prone (tap / hold)", true}, {CycleWeapons, "Cycle weapons", true},
    {CycleEquipment, "Cycle equipment", true}, {Melee, "Melee", true},
    {Start, "Start (options / respawn)", true, 12}, {Select, "Select (commands)", true, 13},
    {DpadUp, "D-pad up (scope / commands)", true, 0}, {DpadDown, "D-pad down (scope / commands)", true, 1},
    {DpadLeft, "D-pad left (commands)", true, 2}, {DpadRight, "D-pad right (commands)", true, 3}};
constexpr uint32_t look_tokens[] = {0xc8a3e21d, 0x941734a2, 0x661cb595, 0xc34f3a66};
// Contexts a binding may use: on foot for gameplay actions; on foot and menu for system buttons.
constexpr uint32_t OnFoot = uint32_t(InputContext::OnFoot);
constexpr uint32_t SystemContexts = OnFoot | uint32_t(InputContext::Menu);
// Controller codes the adapter can remap: SDL buttons 0-4 and 6-14 (Guide, 5, stays reserved)
// and the digital triggers 32 (left) and 33 (right). Analog sticks keep the game's response.
bool remappable_controller_code(uint32_t code) { return code <= 4 || (code >= 6 && code <= 14) || code == 32 || code == 33; }
}  // namespace

std::span<const MagGameControlsAction> mag_game_controls_actions() { return catalog; }

const MagGameControlsAction* mag_game_controls_action(ActionId id) {
    for (const auto& entry : catalog)
        if (entry.id == id) return &entry;
    return nullptr;
}

ActionId mag_game_controls_token_action(uint32_t token) {
    switch (token) {
    case 0x68c3570d: return Aim;
    case 0x9716c7a1: return Stance;
    case 0x71c8f2f0: return Interact;
    default: return token;
    }
}

unsigned mag_game_controls_look_bit(uint32_t token) {
    for (unsigned i = 0; i < std::size(look_tokens); ++i)
        if (look_tokens[i] == token) return 1u << i;
    return 0;
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
    // System buttons: Escape, Tab and the arrow keys; Start, Back and the D-pad on a controller.
    for (auto [code, id] : {std::pair{41u, Start}, {43u, Select}, {82u, DpadUp}, {81u, DpadDown},
                            {80u, DpadLeft}, {79u, DpadRight}})
        profile.bindings.push_back({{InputSource::Keyboard, code}, id, SystemContexts});
    for (auto [code, id] : {std::pair{6u, Start}, {4u, Select}, {11u, DpadUp}, {12u, DpadDown},
                            {13u, DpadLeft}, {14u, DpadRight}})
        profile.bindings.push_back({{InputSource::Controller, code}, id, SystemContexts});
    profile.catalog_revision = 1;
    return profile;
}

std::vector<ActionDescriptor> mag_game_controls_descriptors() {
    constexpr uint32_t Keyboard = 1, Mouse = 2, Controller = 4;
    std::vector<ActionDescriptor> result;
    for (const auto& action : catalog) {
        const bool system = action.system_channel >= 0;
        result.push_back({action.id, action.label, system ? SystemContexts : OnFoot,
                          Keyboard | Mouse | (action.digital_controller ? Controller : 0u),
                          system ? "Native menu / command buttons" : "On-foot actions"});
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

void mag_game_controls_upgrade_profile(ControlsProfile& profile) {
    if (profile.catalog_revision > 1) throw std::invalid_argument("Controls catalog is newer than this build");
    if (profile.catalog_revision == 1) return;
    for (auto binding : mag_game_controls_default_profile().bindings) {
        if (mag_game_controls_action(binding.action)->system_channel < 0) continue;
        const bool present = std::any_of(profile.bindings.begin(), profile.bindings.end(), [&](const auto& b) {
            return b.action == binding.action && b.control.source == binding.control.source;
        });
        // Never displace a physical control the player already uses in a context.
        for (const auto& b : profile.bindings)
            if (b.control == binding.control) binding.contexts &= ~b.contexts;
        if (!present && binding.contexts) profile.bindings.push_back(binding);
    }
    profile.catalog_revision = 1;
}
