#pragma once
/* Picture prompts in menus (docs/native-controls.md, section 12.5). Some menu prompts are pictures a
 * movie places next to a plain label (the face-button icons beside "DELETE", "REWARDS", "EXIT
 * GAME"...), so no text reaches them. While the player uses keyboard and mouse and the SDK keyboard
 * pad serves the menus, the renderer decodes our own key cap, labelled with the pad's key for that
 * button, in place of the game's icon texture. Guest memory is never written; controller players
 * and on-foot play keep the game's pictures. */
#include "mag_game_controls_catalog.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// A guest texture as the renderer describes it (the fields the decision needs).
struct MagPromptTexture {
    uint32_t format = 0, width = 0, height = 0, levels = 0, pitch = 0, span = 0;
    const uint8_t* bytes = nullptr;
};

// A known icon: the SHA-256 (lower-case hex) of its texture bytes and the button it depicts.
struct MagPromptIconIdentity { std::string sha256; MagPadButton button; };
// The game's face-button icons: 24x24, one level, linear DXT5, pitch 96 (section 12.5).
const std::vector<MagPromptIconIdentity>& mag_game_prompts_icon_identities();

// The pad button an icon texture depicts, from its layout (24x24 linear DXT5, pitch 96) and the
// identities; nothing for any other texture.
std::optional<MagPadButton> mag_game_prompts_icon_button(const MagPromptTexture&,
                                                         const std::vector<MagPromptIconIdentity>&);

struct MagPromptIconSources {
    std::function<bool()> keyboard;             // prompts are for keyboard and mouse (section 12.2)
    std::function<bool()> keyboard_pad_live;    // the SDK keyboard pad serves menu buttons
    std::function<std::string(MagPadButton)> label;  // the label of the pad's key for a button
};

class MagGamePromptIcons {
public:
    explicit MagGamePromptIcons(MagPromptIconSources,
                                std::vector<MagPromptIconIdentity> = mag_game_prompts_icon_identities());
    // Renderer question for one texture: whether it is a game icon (true) and, when key caps are
    // shown, the replacement bytes (same span and layout). Replacement bytes stay valid for the
    // object's lifetime. Any thread.
    bool answer(const MagPromptTexture&, const uint8_t** replacement);
    // Re-reads the sources; true when the pictures to show changed (the renderer must ask again).
    bool refresh();
    bool showing_keys() const;
private:
    MagPromptIconSources sources_;
    std::vector<MagPromptIconIdentity> identities_;
    mutable std::mutex lock_;
    bool keys_ = false;
    std::map<MagPadButton, std::string> labels_;
    std::map<MagPadButton, const std::vector<uint8_t>*> shown_;
    // Every picture made so far, by button and label; never erased, so a pointer handed to the
    // renderer outlives any later change.
    std::map<std::pair<MagPadButton, std::string>, std::vector<uint8_t>> pictures_;
};
