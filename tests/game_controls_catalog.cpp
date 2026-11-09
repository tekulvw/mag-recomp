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
        require(d.group == (system ? "Native menu / command buttons" : "On-foot actions"), "group");
    }
}
static void defaults() {
    auto profile = mag_game_controls_default_profile();
    mag_game_controls_validate_profile(profile);
    require(profile.catalog_revision == 1, "defaults are the current catalog");
    for (const auto& b : profile.bindings) require(mag_game_controls_action(b.action), "default binds a catalog action");
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
static void upgrade() {
    ControlsProfile old;  // Revision 0: on-foot keys only, Escape already used by the player.
    old.bindings.push_back({{InputSource::Keyboard, 41}, 0xa7b30616});
    mag_game_controls_upgrade_profile(old);
    require(old.catalog_revision == 1, "upgrade records the revision");
    const auto start = std::find_if(old.bindings.begin(), old.bindings.end(), [](const auto& b) {
        return b.action == 0x163aab24 && b.control.source == InputSource::Keyboard;
    });
    require(start != old.bindings.end() && start->contexts == uint32_t(InputContext::Menu),
            "Start keeps Escape out of OnFoot where the player bound Jump");
    const auto count = old.bindings.size();
    mag_game_controls_upgrade_profile(old);
    require(old.bindings.size() == count, "upgrade runs once");
    ControlsProfile future;
    future.catalog_revision = 2;
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
        else throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
