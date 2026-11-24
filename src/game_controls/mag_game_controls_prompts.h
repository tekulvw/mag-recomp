#pragma once
/* Keyboard and mouse button prompts (docs/native-controls.md, section 12). The game's UI text
 * carries [Name] placeholders that its movies draw as PlayStation button icons. While the player
 * uses keyboard and mouse, the text handed to a movie has each placeholder the controls can name
 * replaced by the bound key, drawn by the movie in the game's own font. Controller players keep
 * the game's text and icons. */
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_catalog.h"
#include "player_controls.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a prompt decision reads. The runtime fills it from the input engine; tests fill it directly.
struct MagGamePromptSources {
    std::function<mag::InputDevice()> last_device;
    std::function<mag::InputPolicy()> policy;  // the profile mode
    std::function<std::vector<mag::ControlBinding>(mag::ActionId, mag::InputContext)> bindings;
    std::function<std::string(mag::PhysicalControl)> label;
    // Whether the SDK keyboard pad serves menu buttons now (not muted, section 8.2).
    std::function<bool()> keyboard_pad_live;
    // Optional diagnostics (MAG_GAME_CONTROLS_TRACE=1): one line per text that holds a placeholder.
    std::function<void(const std::string&)> trace;
};

// True when prompts should name keyboard and mouse controls (section 12.2).
bool mag_game_prompts_keyboard(const MagGamePromptSources&);
// The label for one placeholder name ("Action" for "[Action]"), or empty to keep the game's icon.
std::string mag_game_prompts_label(std::string_view name, const MagGamePromptSources&);
// The text with every resolvable placeholder replaced by "(LABEL)"; nothing when no placeholder
// changed. Does not check the device.
std::optional<std::string> mag_game_prompts_rewrite(std::string_view text, const MagGamePromptSources&);
// The button a whole text names when it is a shoulder or stick button's name ("L1" ... "R3").
std::optional<MagPadButton> mag_game_prompts_button_text(std::string_view text);
// The key that presses a pad button now (section 12.6): in a menu (keyboard pad live) the keyboard
// pad's key for it; on foot the first keyboard or mouse binding of the action the game's own
// controller layout puts on that button. Empty when there is none.
std::string mag_game_prompts_button_key(MagPadButton, const MagGamePromptSources&);
// A label short enough for a button's box: at most five characters (longer names keep their first
// four).
std::string mag_game_prompts_compact(std::string label);

// The guest value record at the seam (section 12.1): u32 kind at +0, u32 string address at +8; the
// kind of a UTF-8 string result.
constexpr uint32_t MagGamePromptStringKind = 4;
// The scratch ring the rewritten texts live in: slots of this size, NUL included.
constexpr uint32_t MagGamePromptSlotSize = 256, MagGamePromptSlots = 64;

// The seam handler. At PromptText r3 is the value record: PromptText swaps the record's string for
// the rewritten text in the scratch ring, PromptTextDone puts the game's pointer back.
class MagGamePrompts {
    struct State;
    std::unique_ptr<State> state_;
public:
    // `scratch` is the guest address of MagGamePromptSlots * MagGamePromptSlotSize writable bytes.
    MagGamePrompts(MagGameControlsMemory, uint32_t scratch, MagGamePromptSources);
    ~MagGamePrompts();
    MagGamePrompts(const MagGamePrompts&) = delete;
    MagGamePrompts& operator=(const MagGamePrompts&) = delete;
    // Handles PromptText and PromptTextDone; other events are ignored. Never skips game code.
    void dispatch(MagControlsHook, MagGameControlsCpu) noexcept;
    uint64_t rewritten() const noexcept;  // texts changed so far
    uint64_t failures() const noexcept;   // reads or writes that failed
};
