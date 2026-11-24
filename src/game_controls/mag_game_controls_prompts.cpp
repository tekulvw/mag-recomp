#include "mag_game_controls_prompts.h"
#include "mag_game_controls_catalog.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

using namespace mag;

namespace {
bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
uint32_t be32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}
void put_be32(uint8_t* bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (24 - 8 * i));
}
}  // namespace

bool mag_game_prompts_keyboard(const MagGamePromptSources& sources) {
    const InputPolicy policy = sources.policy ? sources.policy() : InputPolicy{};
    if (policy.mode == InputMode::KBM) return true;
    if (policy.mode == InputMode::Controller) return false;
    // Before any press, keyboard and mouse: a connected controller counts only once it has sent a
    // button, trigger or stick press (Windows reports an idle XInput pad on many PCs).
    return (sources.last_device ? sources.last_device() : InputDevice::None) != InputDevice::Controller;
}

std::string mag_game_prompts_label(std::string_view name, const MagGamePromptSources& sources) {
    if (!sources.label) return {};
    // The menu confirm and back placeholders name the SDK keyboard pad's Cross and Circle keys: the
    // menus that show them read that pad, and a movie often fetches its menu text while the player
    // is still on foot, so the context at request time cannot tell.
    const uint32_t token = mag_game_controls_name_token(name);
    if (token == mag_game_controls_name_token("os_enter"))
        return sources.label({InputSource::Keyboard, mag_game_controls_pad_layout_key(MagPadButton::Cross)});
    if (token == mag_game_controls_name_token("os_cancel"))
        return sources.label({InputSource::Keyboard, mag_game_controls_pad_layout_key(MagPadButton::Circle)});
    const ActionId action = mag_game_controls_prompt_action(name);
    if (!action || !sources.bindings) return {};
    for (const auto& binding : sources.bindings(action, InputContext::OnFoot)) {
        if (binding.control.source == InputSource::Controller) continue;
        if (auto label = sources.label(binding.control); !label.empty()) return label;
    }
    return {};
}

std::optional<MagPadButton> mag_game_prompts_button_text(std::string_view text) {
    if (text.size() != 2 || (text[0] != 'L' && text[0] != 'R') || text[1] < '1' || text[1] > '3') return std::nullopt;
    constexpr MagPadButton left[] = {MagPadButton::L1, MagPadButton::L2, MagPadButton::L3};
    constexpr MagPadButton right[] = {MagPadButton::R1, MagPadButton::R2, MagPadButton::R3};
    return (text[0] == 'L' ? left : right)[text[1] - '1'];
}

namespace {
// SDL GameController numbers of the pad buttons (the controller codes of section 8.1; triggers are
// codes 32 and 33).
uint32_t controller_code(MagPadButton button) {
    switch (button) {
    case MagPadButton::Cross: return 0; case MagPadButton::Circle: return 1;
    case MagPadButton::Square: return 2; case MagPadButton::Triangle: return 3;
    case MagPadButton::Select: return 4; case MagPadButton::Start: return 6;
    case MagPadButton::L3: return 7; case MagPadButton::R3: return 8;
    case MagPadButton::L1: return 9; case MagPadButton::R1: return 10;
    case MagPadButton::Up: return 11; case MagPadButton::Down: return 12;
    case MagPadButton::Left: return 13; case MagPadButton::Right: return 14;
    case MagPadButton::L2: return 32; case MagPadButton::R2: return 33;
    }
    return UINT32_MAX;
}
}  // namespace

std::string mag_game_prompts_compact(std::string label) {
    if (label.size() > 5) label.resize(4);
    return label;
}

std::string mag_game_prompts_button_key(MagPadButton button, const MagGamePromptSources& sources) {
    if (!sources.label) return {};
    if (sources.keyboard_pad_live && sources.keyboard_pad_live())
        return sources.label({InputSource::Keyboard, mag_game_controls_pad_layout_key(button)});
    if (!sources.bindings) return {};
    // The game's own controller layout (the catalog defaults) says which action the button is.
    static const ControlsProfile layout = mag_game_controls_default_profile();
    const uint32_t code = controller_code(button);
    for (const auto& b : layout.bindings) {
        if (b.control.source != InputSource::Controller || b.control.code != code || !(b.contexts & uint32_t(InputContext::OnFoot)))
            continue;
        for (const auto& bound : sources.bindings(b.action, InputContext::OnFoot)) {
            if (bound.control.source == InputSource::Controller) continue;
            if (auto label = sources.label(bound.control); !label.empty()) return label;
        }
    }
    return {};
}

std::optional<std::string> mag_game_prompts_rewrite(std::string_view text, const MagGamePromptSources& sources) {
    // A whole text that is a shoulder or stick button's name ("L1"): the movie draws it in a small
    // box as that button's prompt (section 12.6).
    if (const auto button = mag_game_prompts_button_text(text)) {
        std::string label = mag_game_prompts_compact(mag_game_prompts_button_key(*button, sources));
        if (label.empty() || label == text) return std::nullopt;
        return label;
    }
    std::string result;
    bool changed = false;
    size_t done = 0;
    for (size_t open = text.find('['); open != std::string_view::npos; open = text.find('[', open + 1)) {
        size_t close = open + 1;
        while (close < text.size() && close - open - 1 < 33 && name_char(text[close])) ++close;
        const size_t length = close - open - 1;
        if (close >= text.size() || text[close] != ']' || length == 0 || length > 32) continue;
        const std::string label = mag_game_prompts_label(text.substr(open + 1, length), sources);
        if (label.empty()) continue;
        result.append(text.substr(done, open - done));
        result.append("(").append(label).append(")");
        done = close + 1;
        changed = true;
    }
    if (!changed) return std::nullopt;
    result.append(text.substr(done));
    return result;
}

struct MagGamePrompts::State {
    MagGameControlsMemory memory;
    uint32_t scratch;
    MagGamePromptSources sources;
    std::mutex lock;
    unsigned next_slot = 0, traced = 0;
    // The record changed at PromptText, its original string and the scratch text written there.
    uint32_t record = 0, original = 0, written = 0;
    std::atomic<uint64_t> rewritten{0}, failures{0};

    bool read(uint32_t address, void* out, size_t size) {
        return memory.read && memory.read(memory.user, address, out, size);
    }
    bool write(uint32_t address, const void* in, size_t size) {
        return memory.write && memory.write(memory.user, address, in, size);
    }
    void fail(const char* what) {
        if (failures.fetch_add(1) == 0) std::fprintf(stderr, "[game-prompts] left a text unchanged: %s\n", what);
    }
    // Reads a NUL-terminated string of at most 1023 bytes.
    std::optional<std::string> read_string(uint32_t address) {
        std::string text;
        std::array<char, 64> chunk{};
        while (text.size() < 1024) {
            if (!read(address + uint32_t(text.size()), chunk.data(), chunk.size())) {
                // Near the end of readable memory: fall back to single bytes.
                char c = 0;
                if (!read(address + uint32_t(text.size()), &c, 1)) return std::nullopt;
                if (!c) return text;
                text.push_back(c);
                continue;
            }
            for (char c : chunk) {
                if (!c) return text;
                text.push_back(c);
            }
        }
        return std::nullopt;
    }
    void text(MagGameControlsCpu cpu) {
        std::lock_guard guard(lock);
        record = 0;
        const bool keyboard = mag_game_prompts_keyboard(sources);
        if (!keyboard && !sources.trace) return;
        const uint32_t value = uint32_t(cpu.gpr[3]);
        std::array<uint8_t, 12> head{};
        if (!read(value, head.data(), head.size())) return fail("value record unreadable");
        if (be32(head.data()) != MagGamePromptStringKind) return;  // not a string result
        const uint32_t pointer = be32(head.data() + 8);
        const auto original_text = read_string(pointer);
        if (!original_text) return fail("text unreadable");
        if (original_text->find('[') == std::string::npos && !mag_game_prompts_button_text(*original_text)) return;
        const auto next = keyboard ? mag_game_prompts_rewrite(*original_text, sources) : std::nullopt;
        if (sources.trace && traced < 400) {
            ++traced;
            sources.trace(std::string(keyboard ? "keyboard" : "controller") + " text \"" + *original_text + "\"" +
                          (next ? " -> \"" + *next + "\"" : " unchanged"));
        }
        if (!next) return;
        if (next->size() + 1 > MagGamePromptSlotSize) return fail("rewritten text too long");
        const uint32_t slot = scratch + MagGamePromptSlotSize * (next_slot++ % MagGamePromptSlots);
        if (!write(slot, next->c_str(), next->size() + 1)) return fail("scratch unwritable");
        std::array<uint8_t, 4> swapped{};
        put_be32(swapped.data(), slot);
        if (!write(value + 8, swapped.data(), swapped.size())) return fail("value record unwritable");
        record = value, original = pointer, written = slot;
        rewritten.fetch_add(1);
    }
    void done() {
        std::lock_guard guard(lock);
        if (!record) return;
        std::array<uint8_t, 4> current{};
        if (read(record + 8, current.data(), current.size()) && be32(current.data()) == written) {
            put_be32(current.data(), original);
            if (!write(record + 8, current.data(), current.size())) fail("value record not restored");
        }
        record = 0;
    }
};

MagGamePrompts::MagGamePrompts(MagGameControlsMemory memory, uint32_t scratch, MagGamePromptSources sources)
    : state_(std::make_unique<State>()) {
    state_->memory = memory;
    state_->scratch = scratch;
    state_->sources = std::move(sources);
}
MagGamePrompts::~MagGamePrompts() = default;

void MagGamePrompts::dispatch(MagControlsHook event, MagGameControlsCpu cpu) noexcept {
    try {
        if (event == MagControlsHook::PromptText) state_->text(cpu);
        else if (event == MagControlsHook::PromptTextDone) state_->done();
    } catch (...) {
        state_->fail("internal error");
    }
}
uint64_t MagGamePrompts::rewritten() const noexcept { return state_->rewritten.load(); }
uint64_t MagGamePrompts::failures() const noexcept { return state_->failures.load(); }
