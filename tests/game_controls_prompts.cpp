// Keyboard and mouse button prompts: device choice, placeholder resolution, key labels and the
// seam's guest-memory rewrite. Synthetic inputs only.
#include "mag_game_controls_catalog.h"
#include "mag_game_controls_prompts.h"
#include "player_prompt_labels.h"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace mag;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

// The host keyboard layout of the test: empty means "no answer" (US fallback).
static std::map<uint32_t, std::string> host_names;
extern "C" size_t mag_prompt_host_key_name(uint32_t usage, char* out, size_t capacity) {
    const auto found = host_names.find(usage);
    if (found == host_names.end() || found->second.size() >= capacity) return 0;
    std::memcpy(out, found->second.data(), found->second.size());
    return found->second.size();
}

namespace {
constexpr ActionId Interact = 0x47cc8c92, Fire = 0x58de09cf, Bleedout = 0x40feec35, CycleEquipment = 0xcb535315;

struct World {
    InputDevice device = InputDevice::KeyboardMouse;
    InputPolicy policy;
    ControlsProfile profile = mag_game_controls_default_profile();
    MagGamePromptSources sources() {
        MagGamePromptSources s;
        s.last_device = [this] { return device; };
        s.policy = [this] { return policy; };
        s.bindings = [this](ActionId action, InputContext context) {
            std::vector<ControlBinding> out;
            for (const auto& b : profile.bindings)
                if (b.action == action && (b.contexts & uint32_t(context)) &&
                    (b.control.source != InputSource::Controller || profile.controller_remap))
                    out.push_back(b);
            return out;
        };
        s.label = mag_prompt_label;
        return s;
    }
};

void rebind(ControlsProfile& profile, ActionId action, PhysicalControl control) {
    std::erase_if(profile.bindings, [&](const auto& b) { return b.action == action && b.control.source != InputSource::Controller; });
    profile.bindings.push_back({control, action});
}

void device() {
    World w;
    auto s = w.sources();
    w.policy.mode = InputMode::Auto;
    w.device = InputDevice::None;
    w.policy.controller_connected = false;
    require(mag_game_prompts_keyboard(s), "no press and no controller: keyboard");
    w.policy.controller_connected = true;
    require(mag_game_prompts_keyboard(s), "an idle connected controller does not count before a press");
    w.device = InputDevice::KeyboardMouse;
    require(mag_game_prompts_keyboard(s), "a key press switches to keyboard");
    w.device = InputDevice::Controller;
    w.policy.controller_connected = false;
    require(!mag_game_prompts_keyboard(s), "a controller press switches to controller");
    w.policy.mode = InputMode::KBM;
    require(mag_game_prompts_keyboard(s), "KBM mode always shows keys");
    w.policy.mode = InputMode::Controller;
    w.device = InputDevice::KeyboardMouse;
    require(!mag_game_prompts_keyboard(s), "Controller mode always shows icons");
}

void labels() {
    host_names.clear();
    require(mag_prompt_fallback_label({InputSource::Keyboard, 8}) == "E", "letter");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 30}) == "1", "digit 1");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 39}) == "0", "digit 0");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 44}) == "SPACE", "space");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 40}) == "ENTER", "return");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 225}) == "SHIFT", "left shift");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 62}) == "F5", "function key");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 92}) == "NUM4", "keypad");
    require(mag_prompt_fallback_label({InputSource::Keyboard, 79}) == "RIGHT", "arrow");
    require(mag_prompt_fallback_label({InputSource::Mouse, 0}) == "LMB", "left mouse");
    require(mag_prompt_fallback_label({InputSource::Mouse, 1}) == "RMB", "right mouse");
    require(mag_prompt_fallback_label({InputSource::Mouse, 4}) == "M5", "mouse 5");
    require(mag_prompt_fallback_label({InputSource::Controller, 0}).empty(), "controller has no key label");
    require(mag_prompt_normalize_label("e") == "E", "upper case");
    require(mag_prompt_normalize_label("\xC3\xA9").empty(), "non-ASCII refused");
    require(mag_prompt_normalize_label("Right Shift").empty(), "spaces refused");
    require(mag_prompt_normalize_label("ABCDEFGHI").empty(), "longer than 8 refused");
    // The host layout names layout keys: AZERTY puts A where US has Q.
    host_names[20] = "a";
    require(mag_prompt_label({InputSource::Keyboard, 20}) == "A", "host layout letter");
    // Digit row: a symbol unshifted falls back to the digit; a digit or letter is kept.
    host_names[30] = "&";
    require(mag_prompt_label({InputSource::Keyboard, 30}) == "1", "digit row symbol uses the digit");
    host_names[31] = "2";
    require(mag_prompt_label({InputSource::Keyboard, 31}) == "2", "digit row digit kept");
    // Punctuation follows the layout; an unusable host name falls back.
    host_names[51] = "m";
    require(mag_prompt_label({InputSource::Keyboard, 51}) == "M", "punctuation key follows the layout");
    host_names[52] = "\xC3\xB9";
    require(mag_prompt_label({InputSource::Keyboard, 52}) == "'", "non-ASCII host name falls back");
    // Keys outside the layout set never ask the host.
    host_names[44] = "Leertaste";
    require(mag_prompt_label({InputSource::Keyboard, 44}) == "SPACE", "fixed keys keep their short name");
    host_names.clear();
}

void resolve() {
    host_names.clear();
    World w;
    auto s = w.sources();
    require(mag_game_prompts_label("Action", s) == "E", "[Action] names the Interact key");
    require(mag_game_prompts_label("ContAction", s) == "E", "[ContAction] is the held Interact");
    require(mag_game_prompts_label("Fire", s) == "LMB", "[Fire] names the mouse button");
    require(mag_game_prompts_label("fire", s) == "LMB", "names are case-insensitive");
    require(mag_game_prompts_label("Bleedout", s) == "ENTER", "[Bleedout]");
    require(mag_game_prompts_label("CommandScreen", s) == "TAB", "[CommandScreen] is the Select key");
    require(mag_game_prompts_label("Respawn", s) == "ESC", "[Respawn] is the Start key");
    require(mag_game_prompts_label("NoSuchAction", s).empty(), "unknown name keeps the icon");
    // Menu buttons: the SDK keyboard pad's Cross and Circle keys, whatever the profile binds.
    require(mag_game_prompts_label("OS_Enter", s) == "X", "menu confirm is the pad's Cross key");
    require(mag_game_prompts_label("os_cancel", s) == "C", "menu back is the pad's Circle key");
    // Rebinding: the live profile decides at every request.
    rebind(w.profile, Interact, {InputSource::Keyboard, 9});
    require(mag_game_prompts_label("Action", s) == "F", "rebinding changes the label");
    rebind(w.profile, Fire, {InputSource::Mouse, 2});
    require(mag_game_prompts_label("Fire", s) == "MMB", "rebinding to a mouse button");
    // An action with only a controller binding keeps the icon.
    std::erase_if(w.profile.bindings, [](const auto& b) { return b.action == Bleedout; });
    require(mag_game_prompts_label("Bleedout", s).empty(), "unbound action keeps the icon");
    // Primary before secondary.
    w.profile.bindings.push_back({{InputSource::Keyboard, 11}, CycleEquipment});
    require(mag_game_prompts_label("SwapNextSec", s) == "G", "the first binding in profile order");
}

void rewrite() {
    host_names.clear();
    World w;
    auto s = w.sources();
    require(*mag_game_prompts_rewrite("[OS_Enter] SELECT", s) == "(X) SELECT", "leading placeholder");
    require(*mag_game_prompts_rewrite("PRESS [Action] TO HEAL", s) == "PRESS (E) TO HEAL", "inner placeholder");
    require(*mag_game_prompts_rewrite("[Fire][Action]", s) == "(LMB)(E)", "adjacent placeholders");
    require(*mag_game_prompts_rewrite("[Unknown] and [Fire]", s) == "[Unknown] and (LMB)", "unknown stays");
    require(!mag_game_prompts_rewrite("NO PROMPT HERE", s), "no placeholder: unchanged");
    require(!mag_game_prompts_rewrite("[Unknown] ONLY", s), "nothing resolvable: unchanged");
    require(!mag_game_prompts_rewrite("[Fire", s), "unterminated");
    require(!mag_game_prompts_rewrite("[] [ Fire ] [Fi-re]", s), "malformed names");
    require(!mag_game_prompts_rewrite("[ThisNameIsLongerThanThirtyTwoChars]", s), "name too long");
    require(*mag_game_prompts_rewrite("[[Fire]]", s) == "[(LMB)]", "nested brackets");
}

// Fake big-endian guest memory for the seam.
struct Guest {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x20000);
    uint64_t failures = 0;
    static bool read(void* user, uint32_t address, void* out, size_t size) {
        auto* g = static_cast<Guest*>(user);
        if (address < 0x1000 || address + size > g->bytes.size()) return false;
        std::memcpy(out, g->bytes.data() + address, size);
        return true;
    }
    static bool write(void* user, uint32_t address, const void* in, size_t size) {
        auto* g = static_cast<Guest*>(user);
        if (address < 0x1000 || address + size > g->bytes.size()) return false;
        std::memcpy(g->bytes.data() + address, in, size);
        return true;
    }
    void put32(uint32_t address, uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[address + i] = uint8_t(value >> (24 - 8 * i));
    }
    uint32_t get32(uint32_t address) const {
        return uint32_t(bytes[address]) << 24 | uint32_t(bytes[address + 1]) << 16 | uint32_t(bytes[address + 2]) << 8 | bytes[address + 3];
    }
    void put_string(uint32_t address, const char* text) { std::memcpy(bytes.data() + address, text, std::strlen(text) + 1); }
    std::string string(uint32_t address) const { return reinterpret_cast<const char*>(bytes.data() + address); }
};

constexpr uint32_t Record = 0x2000, Text = 0x3000, Scratch = 0x10000;

void seam() {
    host_names.clear();
    World w;
    Guest guest;
    MagGamePrompts prompts({&guest, Guest::read, Guest::write}, Scratch, w.sources());
    std::array<uint64_t, 32> gpr{};
    std::array<double, 32> fpr{};
    const MagGameControlsCpu cpu{gpr, fpr, 0x0053BF44, 1};
    auto deliver = [&](const char* text, uint32_t kind = MagGamePromptStringKind) {
        guest.put32(Record, kind);
        guest.put32(Record + 8, Text);
        guest.put_string(Text, text);
        gpr[3] = Record;
        prompts.dispatch(MagControlsHook::PromptText, cpu);
        const uint32_t seen = guest.get32(Record + 8);
        const std::string result = guest.string(seen);
        gpr[3] = 0;  // the register is not relied on afterwards
        prompts.dispatch(MagControlsHook::PromptTextDone, cpu);
        require(guest.get32(Record + 8) == Text, "the game's pointer is restored after the call");
        require(guest.string(Text) == text, "the game's text is never modified");
        return std::pair{seen, result};
    };
    auto [where, text] = deliver("[OS_Enter] CLOSE");
    require(where >= Scratch && where < Scratch + MagGamePromptSlots * MagGamePromptSlotSize, "text written to the scratch ring");
    require(text == "(X) CLOSE", "the movie sees the key");
    require(prompts.rewritten() == 1, "counted");
    auto [where2, text2] = deliver("PRESS [Action] TO HEAL");
    require(where2 != where && text2 == "PRESS (E) TO HEAL", "next slot");
    // Nothing to change: the record is left alone.
    require(deliver("SORT STATS").first == Text, "no placeholder: untouched");
    require(deliver("[Fire]", 3).first == Text, "not a string result: untouched");
    // A controller player keeps the game's text.
    w.device = InputDevice::Controller;
    require(deliver("[OS_Enter] CLOSE").first == Text, "controller: untouched");
    w.device = InputDevice::KeyboardMouse;
    // A rewrite that does not fit a slot is refused, visibly.
    std::string longer(250, 'A');
    longer = "[Fire] " + longer;
    require(deliver(longer.c_str()).first == Text, "too long: untouched");
    require(prompts.failures() == 1, "too long counted");
    // Unreadable record or text: untouched, counted.
    gpr[3] = 0x500;
    prompts.dispatch(MagControlsHook::PromptText, cpu);
    prompts.dispatch(MagControlsHook::PromptTextDone, cpu);
    require(prompts.failures() == 2, "unreadable record counted");
    // The ring wraps: 64 slots, then the first slot again.
    for (unsigned i = 0; i < MagGamePromptSlots; ++i) deliver("[Fire]");
    require(prompts.rewritten() == 2 + MagGamePromptSlots, "every rewrite counted");
    // Other events are ignored.
    prompts.dispatch(MagControlsHook::OuterEnter, cpu);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string which = argc > 1 ? argv[1] : "";
        if (which == "device") device();
        else if (which == "labels") labels();
        else if (which == "resolve") resolve();
        else if (which == "rewrite") rewrite();
        else if (which == "seam") seam();
        else throw std::runtime_error("unknown test");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
