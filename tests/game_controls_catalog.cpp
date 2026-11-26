// Settings catalog of the game controls adapter: descriptors, defaults, validation and upgrade.
#include "mag_game_controls_catalog.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace mag;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static bool rejects(const ControlsProfile& p) {
    try { mag_game_controls_validate_profile(p); } catch (const std::invalid_argument&) { return true; }
    return false;
}
static void descriptors() {
    const auto list = mag_game_controls_descriptors();
    require(list.size() == mag_game_controls_actions().size(), "one descriptor per action");
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& action = mag_game_controls_actions()[i];
        const auto& d = list[i];
        require(d.id == action.id && d.label == action.label, "descriptor names its action");
        const bool system = action.system_channel >= 0;
        require(d.contexts == (system ? 17u : 1u), "system buttons span OnFoot and Menu");
        require(d.sources == (action.digital_controller ? 7u : 3u), "controller only where remappable");
        require(d.group == (action.group ? action.group : system ? "Native menu / command buttons" : "On-foot actions"), "group");
        require(!action.optional || (!action.digital_controller && !system && action.group), "situational actions are keyboard-only gameplay actions with a group");
    }
}
static void defaults() {
    auto profile = mag_game_controls_default_profile();
    mag_game_controls_validate_profile(profile);
    require(profile.catalog_revision == MagGameControlsCatalogRevision, "defaults are the current catalog");
    for (const auto& b : profile.bindings) require(mag_game_controls_action(b.action), "default binds a catalog action");
    // Every action has a keyboard or mouse default (the Controls page shows each one bound).
    for (const auto& action : mag_game_controls_actions())
        require(std::any_of(profile.bindings.begin(), profile.bindings.end(), [&](const auto& b) {
                    return b.action == action.id && b.control.source != InputSource::Controller;
                }), "an action has no keyboard or mouse default");
    // A situational action cannot take a controller button: the controller presses it natively.
    auto bleed = profile;
    bleed.bindings.push_back({{InputSource::Controller, 2}, 0x40feec35});
    require(rejects(bleed), "controller binding for a situational action accepted");
    auto stick = profile;
    stick.bindings.push_back({{InputSource::Controller, 5}, 0x58de09cf});  // Guide stays reserved.
    require(rejects(stick), "Guide accepted");
    auto movement = profile;
    movement.bindings.push_back({{InputSource::Controller, 0}, 0xb164d156});  // Movement is analog.
    require(rejects(movement), "controller movement accepted");
    auto menu = profile;
    menu.bindings.push_back({{InputSource::Keyboard, 30}, 0x58de09cf, uint32_t(InputContext::Menu)});
    require(rejects(menu), "gameplay action accepted in Menu");
    auto gains = profile;
    gains.mouse.ads_gain = 2;
    require(rejects(gains), "separate aim gains accepted");
}
// Prompt placeholders name actions through the game's tokens: CRC-32 of the lower-cased name.
static void prompts() {
    require(mag_game_controls_name_token("") == 0, "empty name");
    require(mag_game_controls_name_token("fire") == 0x58de09cf && mag_game_controls_name_token("FIRE") == 0x58de09cf,
            "name tokens are case-insensitive CRC-32");
    require(mag_game_controls_prompt_action("Fire") == 0x58de09cf, "[Fire]");
    require(mag_game_controls_prompt_action("Action") == 0x47cc8c92, "[Action] is Interact");
    require(mag_game_controls_prompt_action("ContAction") == 0x47cc8c92, "[ContAction] is held Interact");
    require(mag_game_controls_prompt_action("SwapNextPri") == 0x2c4380c4, "[SwapNextPri] cycles weapons");
    require(mag_game_controls_prompt_action("Bleedout") == 0x40feec35, "[Bleedout]");
    require(mag_game_controls_prompt_action("OS_Enter") == 0x33e32a99, "[OS_Enter]");
    require(mag_game_controls_prompt_action("NotAnAction") == 0, "unknown names are no action");
    // Each situational action's identifier is the token of its prompt name.
    for (const char* name : {"OS_Enter", "OS_Cancel", "Bleedout", "TCM_Abilities", "TCM_Frago",
                             "TCM_VoiceChat", "VSeatNext", "VSmoke"}) {
        const auto* action = mag_game_controls_action(mag_game_controls_name_token(name));
        require(action && action->optional, "situational action missing from the catalog");
    }
    // The command screen is watched on the Select button: its token stands for Select.
    require(mag_game_controls_prompt_action("CommandScreen") == 0x0a7fd35e, "[CommandScreen] is Select");
    // Every placeholder the game's text uses names a catalog action.
    require(mag_game_controls_prompt_action("Respawn") == 0x163aab24, "[Respawn] is Start");
    require(mag_game_controls_prompt_action("ScopeUp") == 0xa5bc1361 && mag_game_controls_prompt_action("ScopeDown") == 0x8c2adba2,
            "scope zoom is the D-pad's up and down");
    require(mag_game_controls_prompt_action("AimHold") == 0xc07aa75a && mag_game_controls_prompt_action("ScopeHold") == 0xc07aa75a,
            "[AimHold] and [ScopeHold] are Aim");
    require(mag_game_controls_prompt_action("GoProne") == 0x541512ed && mag_game_controls_prompt_action("ChangeStance") == 0x541512ed,
            "[GoProne] and [ChangeStance] are Crouch / prone");
    for (const char* name : {"Jump", "Sprint", "MoveForward", "Fire", "Action", "ContAction", "SwapNextPri", "SwapNextSec",
                             "OS_Enter", "OS_Cancel", "Bleedout", "VSeatNext", "VSmoke", "TCM_VoiceChat", "TCM_Frago",
                             "TCM_Abilities", "CommandScreen", "Respawn", "AimHold", "ScopeHold", "GoProne", "ChangeStance"})
        require(mag_game_controls_prompt_action(name) != 0, "a placeholder in the game's text names no action");
    require(!mag_game_controls_action(0xc6dcd89f), "the retired command screen is still an action");
    require(mag_game_controls_retired_actions().size() == 1 && mag_game_controls_retired_actions()[0] == 0xc6dcd89f,
            "the command screen is listed as retired");
    require(mag_game_controls_pad_layout_key(MagPadButton::Cross) == 27 &&
                mag_game_controls_pad_layout_key(MagPadButton::Circle) == 6 &&
                mag_game_controls_pad_layout_key(MagPadButton::Start) == 40 &&
                mag_game_controls_pad_layout_key(MagPadButton::R3) == 10,
            "pad layout keys");
    require(mag_game_controls_seat_alias(0x2c4380c4) == 0xc07aa75a, "a seat's R2 does not read Aim's keys");
    require(mag_game_controls_seat_alias(0x58de09cf) == 0 && mag_game_controls_seat_alias(0xc07aa75a) == 0,
            "a seat alias for an action other than Cycle weapons");
    require(mag_game_controls_drive_alias(0x2c4380c4) == 0xb164d156 && mag_game_controls_drive_alias(0xcb535315) == 0x93115ec5,
            "a driver's triggers do not read Move forward's and Move backward's keys");
    require(mag_game_controls_drive_alias(0x58de09cf) == 0, "a drive alias for an action other than the triggers");
    require(mag_game_controls_menu_button(0xdb59e9a9), "Melee's keys do not press a menu's R3");
    require(!mag_game_controls_menu_button(0xa7b30616) && !mag_game_controls_menu_button(0xcb535315),
            "a menu button other than R3 reads the player's keys");
}
// A revision 2 profile: today's defaults with revision 2's keys (Confirm Return, Bleed out X,
// Command screen M).
static ControlsProfile revision_2() {
    auto p = mag_game_controls_default_profile();
    for (auto& b : p.bindings) {
        if (b.action == 0x33e32a99) b.control.code = 40;
        else if (b.action == 0x40feec35) b.control.code = 27;
    }
    p.bindings.push_back({{InputSource::Keyboard, 16}, 0xc6dcd89f});
    p.catalog_revision = 2;
    return p;
}
static uint32_t key_of(const ControlsProfile& p, ActionId action) {
    for (const auto& b : p.bindings)
        if (b.action == action && b.control.source == InputSource::Keyboard && (b.contexts & 1)) return b.control.code;
    return 0;
}
static size_t count_of(const ControlsProfile& p, ActionId action) {
    return size_t(std::count_if(p.bindings.begin(), p.bindings.end(), [&](const auto& b) { return b.action == action; }));
}
static void upgrade_from_2() {
    constexpr ActionId Confirm = 0x33e32a99, Bleedout = 0x40feec35, Command = 0xc6dcd89f, Select = 0x0a7fd35e,
                       Melee = 0xdb59e9a9;
    {   // Untouched defaults become today's defaults: X confirms, Return bleeds out, no M.
        auto p = revision_2();
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        auto defaults = mag_game_controls_default_profile();
        require(p.catalog_revision == MagGameControlsCatalogRevision, "revision 3 recorded");
        require(key_of(p, Confirm) == 27 && key_of(p, Bleedout) == 40 && !count_of(p, Command), "untouched defaults moved");
        require(p.bindings.size() == defaults.bindings.size(), "nothing else changed");
    }
    {   // The player put Bleed out on K: it stays; Confirm still moves to X.
        auto p = revision_2();
        for (auto& b : p.bindings) if (b.action == Bleedout) b.control.code = 14;
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(key_of(p, Bleedout) == 14 && key_of(p, Confirm) == 27, "a rebound Bleed out moved");
    }
    {   // The player put Confirm on K and Melee's second key on Return: Confirm stays; Bleed out keeps
        // X because Return is taken.
        auto p = revision_2();
        for (auto& b : p.bindings) if (b.action == Confirm) b.control.code = 14;
        p.bindings.push_back({{InputSource::Keyboard, 40}, Melee});
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(key_of(p, Confirm) == 14, "a rebound Confirm moved");
        require(key_of(p, Bleedout) == 27, "Bleed out left without a key or displaced Melee");
    }
    {   // The player's own command screen key (K) becomes an on-foot Select key; the default M goes.
        auto p = revision_2();
        for (auto& b : p.bindings) if (b.action == Command) b.control.code = 14;
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        const auto kept = std::find_if(p.bindings.begin(), p.bindings.end(), [&](const auto& b) {
            return b.action == Select && b.control == PhysicalControl{InputSource::Keyboard, 14};
        });
        require(!count_of(p, Command) && kept != p.bindings.end() && kept->contexts == 1, "the player's command key was lost");
    }
    {   // Select already has two keys on foot: the player's command key cannot join and is dropped.
        auto p = revision_2();
        for (auto& b : p.bindings) if (b.action == Command) b.control.code = 14;
        p.bindings.push_back({{InputSource::Keyboard, 15}, Select});
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(!count_of(p, Command) && count_of(p, Select) == 3, "Select took a third key");  // + controller Back
    }
}
// Revision 3 to 4: the wheel zooms a scope (D-pad up and down on foot), never taking a wheel step
// the player uses and never a third key.
static void upgrade_from_3() {
    constexpr ActionId DpadUp = 0xa5bc1361, DpadDown = 0x8c2adba2, Fire = 0x58de09cf;
    const PhysicalControl up{InputSource::Mouse, MouseWheelUp}, down{InputSource::Mouse, MouseWheelDown};
    const auto revision_3 = [] {
        auto p = mag_game_controls_default_profile();
        std::erase_if(p.bindings, [](const auto& b) { return b.control.source == InputSource::Mouse && b.control.code >= 8; });
        p.catalog_revision = 3;
        return p;
    };
    const auto bound = [](const ControlsProfile& p, PhysicalControl c, ActionId action) {
        return std::any_of(p.bindings.begin(), p.bindings.end(), [&](const auto& b) {
            return b.control == c && b.action == action && b.contexts == 1;
        });
    };
    {
        auto p = revision_3();
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(p.catalog_revision == MagGameControlsCatalogRevision && bound(p, up, DpadUp) && bound(p, down, DpadDown),
                "revision 3 gains the scope wheel");
        require(p.bindings.size() == mag_game_controls_default_profile().bindings.size(), "and nothing else");
    }
    {   // The player fires with wheel up: it stays; wheel down still zooms out.
        auto p = revision_3();
        p.bindings.push_back({up, Fire});
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(bound(p, up, Fire) && !bound(p, up, DpadUp) && bound(p, down, DpadDown), "a wheel step the player uses was taken");
    }
    {   // D-pad down already has two keys on foot: no third.
        auto p = revision_3();
        p.bindings.push_back({{InputSource::Keyboard, 14}, DpadDown, 1});
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(!bound(p, down, DpadDown) && bound(p, up, DpadUp), "D-pad down took a third key");
    }
    {   // An older profile (revision 1) gains the wheel too.
        auto p = revision_3();
        std::erase_if(p.bindings, [](const auto& b) { return mag_game_controls_action(b.action)->revision >= 2; });
        p.catalog_revision = 1;
        mag_game_controls_upgrade_profile(p);
        mag_game_controls_validate_profile(p);
        require(bound(p, up, DpadUp) && bound(p, down, DpadDown), "revision 1 missed the scope wheel");
    }
}
static void upgrade() {
    ControlsProfile old;  // Revision 0: on-foot keys only, Escape and X already used by the player.
    old.bindings.push_back({{InputSource::Keyboard, 41}, 0xa7b30616});
    old.bindings.push_back({{InputSource::Keyboard, 27}, 0xdb59e9a9});
    mag_game_controls_upgrade_profile(old);
    require(old.catalog_revision == MagGameControlsCatalogRevision, "upgrade records the revision");
    mag_game_controls_validate_profile(old);
    const auto has = [&](ActionId action) {
        return std::any_of(old.bindings.begin(), old.bindings.end(), [&](const auto& b) { return b.action == action; });
    };
    require(has(0x40feec35) && has(0x74700e10), "situational defaults added");
    require(!has(0x33e32a99), "a situational default displaced the player's own key (X)");
    require(!has(0x58de09cf), "an unbound revision 0 action was bound again");
    ControlsProfile one = mag_game_controls_default_profile();  // Revision 1 (before situational actions).
    std::erase_if(one.bindings, [](const auto& b) { return mag_game_controls_action(b.action)->revision == 2; });
    one.catalog_revision = 1;
    one.bindings.erase(one.bindings.begin());  // The player unbound Move forward.
    mag_game_controls_upgrade_profile(one);
    require(one.bindings.size() == mag_game_controls_default_profile().bindings.size() - 1,
            "revision 1 gains exactly the situational defaults");
    const auto start = std::find_if(old.bindings.begin(), old.bindings.end(), [](const auto& b) {
        return b.action == 0x163aab24 && b.control.source == InputSource::Keyboard;
    });
    require(start != old.bindings.end() && start->contexts == uint32_t(InputContext::Menu),
            "Start keeps Escape out of OnFoot where the player bound Jump");
    const auto count = old.bindings.size();
    mag_game_controls_upgrade_profile(old);
    require(old.bindings.size() == count, "upgrade runs once");
    ControlsProfile future;
    future.catalog_revision = MagGameControlsCatalogRevision + 1;
    upgrade_from_2();
    upgrade_from_3();
    bool refused = false;
    try { mag_game_controls_upgrade_profile(future); } catch (const std::invalid_argument&) { refused = true; }
    require(refused, "newer catalog accepted");
}
int main(int argc, char** argv) {
    try {
        const char* which = argc > 1 ? argv[1] : "";
        if (!std::strcmp(which, "descriptors")) descriptors();
        else if (!std::strcmp(which, "defaults")) defaults();
        else if (!std::strcmp(which, "upgrade")) upgrade();
        else if (!std::strcmp(which, "prompts")) prompts();
        else throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
