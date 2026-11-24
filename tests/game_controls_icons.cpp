// Picture prompts: icon identity, key cap pictures and when they are shown. Synthetic textures only.
#include "mag_game_controls_icons.h"
#include "player_prompt_keycap.h"
#include "player_sha256.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

namespace {
// A made-up 24x24 linear DXT5 "icon": our own key cap for a letter, as raw texture bytes.
std::vector<uint8_t> fake_icon(const char* label) { return mag_prompt_keycap_dxt5(mag_prompt_keycap(label, 24, 24)); }
MagPromptTexture texture_of(const std::vector<uint8_t>& bytes, uint32_t format = 0xA8, uint32_t size = 24, uint32_t pitch = 96) {
    return {format, size, size, 1, pitch, uint32_t(bytes.size()), bytes.data()};
}
std::vector<uint8_t> flip_alpha_rows(const MagKeycapImage& image) {
    MagKeycapImage f = image;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x) f.alpha[y * image.width + x] = image.alpha[(image.height - 1 - y) * image.width + x];
    return mag_prompt_keycap_dxt5(f);
}

void identity() {
    const auto cross = fake_icon("A"), circle = fake_icon("B");
    const std::vector<MagPromptIconIdentity> table = {
        {mag::sha256_hex(cross.data(), cross.size()), MagPadButton::Cross},
        {mag::sha256_hex(circle.data(), circle.size()), MagPadButton::Circle}};
    require(mag_game_prompts_icon_button(texture_of(cross), table) == MagPadButton::Cross, "a known icon is recognised");
    require(mag_game_prompts_icon_button(texture_of(circle), table) == MagPadButton::Circle, "by its own identity");
    auto other = cross; other[100] ^= 1;
    require(!mag_game_prompts_icon_button(texture_of(other), table), "one changed byte is another texture");
    require(!mag_game_prompts_icon_button(texture_of(cross, 0x88), table), "swizzled format is not the icon layout");
    require(!mag_game_prompts_icon_button(texture_of(cross, 0xA8, 24, 128), table), "other pitch");
    MagPromptTexture wrong = texture_of(cross); wrong.levels = 2;
    require(!mag_game_prompts_icon_button(wrong, table), "mipmapped textures are not icons");
    require(mag_game_prompts_icon_identities().size() == 4, "the game's four face-button icons are listed");
    for (const auto& id : mag_game_prompts_icon_identities()) require(id.sha256.size() == 64, "identities are SHA-256 digests");
}

void pictures() {
    const auto cross = fake_icon("A");
    const std::vector<MagPromptIconIdentity> table = {{mag::sha256_hex(cross.data(), cross.size()), MagPadButton::Cross}};
    bool keyboard = true;
    std::string label = "X";
    MagPromptIconSources sources;
    sources.keyboard = [&] { return keyboard; };
    sources.label = [&](MagPadButton b) { require(b == MagPadButton::Cross, "labels asked for listed buttons only"); return label; };
    MagGamePromptIcons icons(sources, table);
    const uint8_t* replacement = reinterpret_cast<const uint8_t*>(1);
    require(icons.answer(texture_of(cross), &replacement) && !replacement, "before the first refresh: interested, no picture");
    require(icons.refresh() && icons.showing_keys(), "keyboard and mouse shows key caps");
    require(!icons.refresh(), "nothing changed, nothing to redo");
    require(icons.answer(texture_of(cross), &replacement) && replacement, "the icon gets a picture");
    // The picture is the key cap for the label, stored bottom-up as the game stores its icons.
    const auto expected = flip_alpha_rows(mag_prompt_keycap("X", 24, 24));
    require(std::vector<uint8_t>(replacement, replacement + 576) == expected, "the X key cap, rows flipped");
    const auto noise = fake_icon("Q");
    const uint8_t* none = nullptr;
    require(!icons.answer(texture_of(noise), &none) && !none, "other textures are declined");

    const uint8_t* before = replacement;
    label = "W";
    require(icons.refresh(), "a new label redoes the pictures");
    icons.answer(texture_of(cross), &replacement);
    require(replacement && replacement != before, "with the new key");
    require(before[0] == expected[0] && std::vector<uint8_t>(before, before + 576) == expected, "the old picture stays valid");

    label = "";
    require(icons.refresh() && icons.showing_keys(), "no key for the button now");
    require(icons.answer(texture_of(cross), &replacement) && !replacement, "keeps the game's icon");
    label = "X"; keyboard = false;
    require(icons.refresh() && !icons.showing_keys(), "a controller player gets the game's icons back");
    require(icons.answer(texture_of(cross), &replacement) && !replacement, "interested, no picture");
    keyboard = true;
    require(icons.refresh(), "keyboard again");
    icons.answer(texture_of(cross), &replacement);
    require(replacement == before, "an earlier picture is reused");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string which = argc > 1 ? argv[1] : "";
        if (which == "identity") identity();
        else if (which == "pictures") pictures();
        else throw std::runtime_error("unknown test");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
