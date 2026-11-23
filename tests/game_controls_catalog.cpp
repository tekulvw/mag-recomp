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
    for (const char* name : {"OS_Enter", "OS_Cancel", "Bleedout", "CommandScreen", "TCM_Abilities", "TCM_Frago",
                             "TCM_VoiceChat", "VSeatNext", "VSmoke"}) {
        const auto* action = mag_game_controls_action(mag_game_controls_name_token(name));
        require(action && action->optional, "situational action missing from the catalog");
    }
    require(mag_game_controls_pad_layout_key(MagPadButton::Cross) == 27 &&
                mag_game_controls_pad_layout_key(MagPadButton::Circle) == 6 &&
                mag_game_controls_pad_layout_key(MagPadButton::Start) == 40 &&
                mag_game_controls_pad_layout_key(MagPadButton::R3) == 10,
            "pad layout keys");
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
    require(has(0x33e32a99) && has(0x74700e10), "revision 2 defaults added");
    require(!has(0x40feec35), "a situational default displaced the player's own key");
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
