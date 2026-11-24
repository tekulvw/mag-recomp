#include "mag_game_controls_icons.h"
#include "player_prompt_keycap.h"
#include "player_sha256.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace {
// RSX texture format byte of a linear (pitch layout) DXT5 texture: base format 0x88 with the
// linear bit 0x20. Public hardware format values.
constexpr uint32_t LinearDxt5 = 0x88 | 0x20;

// The game's face-button icons (SHA-256 of the texture bytes: identities that verify inputs).
const std::vector<MagPromptIconIdentity> game_identities = {
    {"8dd68f3c725f8cd29630e7986c146a36381799ac124e21b8ad49c133fde7142e", MagPadButton::Cross},
    {"19a630eb3bbbb0153175f196c6ecc1dd1cb16866f7e6008ea61b204afd780c8d", MagPadButton::Circle},
    {"3b2669e847aad580cf980fb107dc1ea1fffcbc95d4c4648ef3be361af2d89c77", MagPadButton::Square},
    {"c533b3f519f932b402a40635319f8c4844e7931ae72e822b0f2080b1fa8868ec", MagPadButton::Triangle},
};
constexpr uint32_t IconSize = 24, IconSpan = 576, IconPitch = 96;

// The game draws these textures with the rows bottom-up: our picture is stored the same way.
MagKeycapImage flipped(MagKeycapImage image) {
    for (uint32_t y = 0; y < image.height / 2; ++y)
        std::swap_ranges(image.alpha.begin() + size_t(y) * image.width,
                         image.alpha.begin() + size_t(y + 1) * image.width,
                         image.alpha.begin() + size_t(image.height - 1 - y) * image.width);
    return image;
}
}  // namespace

const std::vector<MagPromptIconIdentity>& mag_game_prompts_icon_identities() { return game_identities; }

std::optional<MagPadButton> mag_game_prompts_icon_button(const MagPromptTexture& texture,
                                                         const std::vector<MagPromptIconIdentity>& identities) {
    if (texture.format != LinearDxt5 || texture.width != IconSize || texture.height != IconSize ||
        texture.levels > 1 || texture.pitch != IconPitch || texture.span != IconSpan || !texture.bytes)
        return std::nullopt;
    const std::string digest = mag::sha256_hex(texture.bytes, texture.span);
    for (const auto& identity : identities)
        if (digest == identity.sha256) return identity.button;
    return std::nullopt;
}

MagGamePromptIcons::MagGamePromptIcons(MagPromptIconSources sources, std::vector<MagPromptIconIdentity> identities)
    : sources_(std::move(sources)), identities_(std::move(identities)) {}

bool MagGamePromptIcons::refresh() {
    const bool keys = sources_.keyboard && sources_.keyboard();
    std::map<MagPadButton, std::string> labels;
    if (keys && sources_.label)
        for (const auto& identity : identities_)
            if (auto label = sources_.label(identity.button); !label.empty()) labels[identity.button] = std::move(label);
    std::lock_guard guard(lock_);
    if (keys == keys_ && labels == labels_) return false;
    keys_ = keys;
    labels_ = std::move(labels);
    shown_.clear();
    for (const auto& [button, label] : labels_) {
        auto [entry, made] = pictures_.try_emplace({button, label});
        if (made) {
            entry->second = mag_prompt_keycap_dxt5(flipped(mag_prompt_keycap(label, IconSize, IconSize)));
            if (entry->second.size() != IconSpan) entry->second.clear();
        }
        if (!entry->second.empty()) shown_[button] = &entry->second;
    }
    return true;
}

bool MagGamePromptIcons::showing_keys() const {
    std::lock_guard guard(lock_);
    return keys_;
}

bool MagGamePromptIcons::answer(const MagPromptTexture& texture, const uint8_t** replacement) {
    const auto button = mag_game_prompts_icon_button(texture, identities_);
    if (!button) return false;
    if (replacement) {
        std::lock_guard guard(lock_);
        const auto found = shown_.find(*button);
        *replacement = keys_ && found != shown_.end() ? found->second->data() : nullptr;
    }
    return true;
}
