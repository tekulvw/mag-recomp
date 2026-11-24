/* Lifetime of the keyboard and mouse button prompts (docs/native-controls.md, section 12). Built with
 * the controls adapter; on with it unless MAG_GAME_PROMPTS=0. The controls hook callback hands the
 * two prompt events here (mag_game_prompts_hook). */
#include "mag_extension.h"
#include "mag_game_controls_memory.h"
#include "mag_game_controls_icons.h"
#include "mag_game_controls_prompts.h"
#include "player_input_capture.h"
#include "player_prompt_labels.h"
#include "libs/video/rsx_draw_engine.h"
#include "ppu_recomp.h"
#include "memory/vm.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

bool mag_game_prompts_hook(MagControlsHook, ppu_context*) noexcept;

namespace {
std::unique_ptr<MagGamePrompts> prompts;
// Picture prompts (section 12.5). Made once and kept for the process: the renderer may still be
// decoding one of its pictures when the session stops.
MagGamePromptIcons* icons;
std::atomic<bool> icons_on{false};
std::atomic<MagGamePrompts*> active{};
// The rewritten texts live in the last 16 KB of the runtime's HLE window: guest memory the game is
// never given (it learns HLE addresses only through the SDK's own queries) and that the SDK's fixed
// uses (GCM labels and tables, RSX device records, codec scratch) leave free at the bottom.
constexpr uint32_t ScratchSize = MagGamePromptSlotSize * MagGamePromptSlots;
uint32_t scratch_base() { return VM_HLE_INJECT_BASE + VM_HLE_INJECT_SIZE - ScratchSize; }

bool in_scratch(uint32_t address, size_t size) {
    const uint32_t base = scratch_base();
    return address >= base && size <= ScratchSize && address - base <= ScratchSize - size;
}
bool in_guest(uint32_t address, size_t size) {
    return mag_game_controls_memory_range(VM_READY, ppu_vm_size, address, size) || in_scratch(address, size);
}
bool read(void*, uint32_t address, void* output, size_t size) {
    if (!in_guest(address, size)) return false;
    std::memcpy(output, vm_to_host(address), size);
    return true;
}
bool write(void*, uint32_t address, const void* input, size_t size) {
    if (!in_guest(address, size)) return false;
    std::memcpy(vm_to_host(address), input, size);
    return true;
}
bool enabled() {
    const char* controls = std::getenv("MAG_GAME_CONTROLS");
    const char* value = std::getenv("MAG_GAME_PROMPTS");
    return (!controls || std::strcmp(controls, "0")) && (!value || std::strcmp(value, "0"));
}

int answer_texture(void* user, const rsx_texture_source* source, const u8** replacement) {
    auto* handler = static_cast<MagGamePromptIcons*>(user);
    if (!handler || !source) return 0;
    const MagPromptTexture texture{source->format, source->width, source->height, source->levels,
                                   source->pitch, source->span, source->bytes};
    return handler->answer(texture, replacement) ? RSX_TEXTURE_OVERRIDE_INTEREST : 0;
}
bool keyboard_prompts() {
    MagGamePromptSources sources;
    sources.last_device = mag::mag_controls_last_device;
    sources.policy = mag::mag_controls_current_policy;
    return mag_game_prompts_keyboard(sources);
}
void start_icons() {
    if (!icons) {
        MagPromptIconSources sources;
        sources.keyboard = keyboard_prompts;
        sources.label = [](MagPadButton button) {
            MagGamePromptSources keys;
            keys.keyboard_pad_live = [] { return !mag_input_capture_keyboard_pad_muted(); };
            keys.bindings = mag::mag_controls_action_bindings;
            keys.label = mag::mag_prompt_label;
            return mag_game_prompts_button_key(button, keys);
        };
        icons = new MagGamePromptIcons(std::move(sources));
    }
    icons->refresh();
    rsx_draw_engine_set_texture_override(answer_texture, icons);
    icons_on.store(true, std::memory_order_release);
}
// Once per host frame: a change of device, menu or key labels makes the renderer ask again.
void frame_poll() {
    if (!icons_on.load(std::memory_order_acquire) || !icons->refresh()) return;
    rsx_draw_engine_set_texture_override(answer_texture, icons);
    if (const char* trace = std::getenv("MAG_GAME_CONTROLS_TRACE"); trace && !std::strcmp(trace, "1"))
        std::fprintf(stderr, "[game-prompts] menu icons: %s\n", icons->showing_keys() ? "key caps" : "the game's");
}

void start(const char*) {
    if (!enabled()) {
        std::fprintf(stderr, "[game-prompts] off\n");
        return;
    }
    start_icons();
    const uint32_t base = scratch_base();
    if ((ppu_vm_size && uint64_t(base) + ScratchSize > ppu_vm_size) || vm_commit(base, ScratchSize) != CELL_OK) {
        std::fprintf(stderr, "[game-prompts] off: no guest scratch for prompt text\n");
        return;
    }
    MagGamePromptSources sources;
    sources.last_device = mag::mag_controls_last_device;
    sources.policy = mag::mag_controls_current_policy;
    sources.bindings = mag::mag_controls_action_bindings;
    sources.label = mag::mag_prompt_label;
    sources.keyboard_pad_live = [] { return !mag_input_capture_keyboard_pad_muted(); };
    if (const char* trace = std::getenv("MAG_GAME_CONTROLS_TRACE"); trace && !std::strcmp(trace, "1"))
        sources.trace = [](const std::string& line) { std::fprintf(stderr, "[game-prompts] %s\n", line.c_str()); };
    prompts = std::make_unique<MagGamePrompts>(MagGameControlsMemory{nullptr, read, write}, base, std::move(sources));
    active.store(prompts.get(), std::memory_order_release);
    std::fprintf(stderr, "[game-prompts] keyboard and mouse prompts on (MAG_GAME_PROMPTS=0 turns them off)\n");
}
void stop() {
    if (icons_on.exchange(false)) rsx_draw_engine_set_texture_override(nullptr, nullptr);
    active.store(nullptr, std::memory_order_release);
    if (prompts && (prompts->rewritten() || prompts->failures()))
        std::fprintf(stderr, "[game-prompts] %llu texts rewritten, %llu left unchanged after errors\n",
                     static_cast<unsigned long long>(prompts->rewritten()),
                     static_cast<unsigned long long>(prompts->failures()));
    prompts.reset();
}

const bool registered = [] {
    MagExtensionCallbacks callbacks{};
    callbacks.start = start;
    callbacks.frame_poll = frame_poll;
    callbacks.stop = stop;
    if (mag_extension_register(&callbacks) != 0) {
        std::fprintf(stderr, "[game-prompts] extension callbacks are full\n");
        std::abort();
    }
    return true;
}();
}  // namespace

bool mag_game_prompts_hook(MagControlsHook event, ppu_context* ctx) noexcept {
    if (MagGamePrompts* handler = active.load(std::memory_order_acquire); handler && ctx)
        handler->dispatch(event, {ctx->gpr, ctx->fpr, ctx->lr, ctx->thread_id});
    return false;
}
