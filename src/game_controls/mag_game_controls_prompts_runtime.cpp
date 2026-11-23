/* Lifetime of the keyboard and mouse button prompts (docs/native-controls.md, section 12). Built with
 * the controls adapter; on with it unless MAG_GAME_PROMPTS=0. The controls hook callback hands the
 * two prompt events here (mag_game_prompts_hook). */
#include "mag_extension.h"
#include "mag_game_controls_memory.h"
#include "mag_game_controls_prompts.h"
#include "player_prompt_labels.h"
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

void start(const char*) {
    if (!enabled()) {
        std::fprintf(stderr, "[game-prompts] off\n");
        return;
    }
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
    if (const char* trace = std::getenv("MAG_GAME_CONTROLS_TRACE"); trace && !std::strcmp(trace, "1"))
        sources.trace = [](const std::string& line) { std::fprintf(stderr, "[game-prompts] %s\n", line.c_str()); };
    prompts = std::make_unique<MagGamePrompts>(MagGameControlsMemory{nullptr, read, write}, base, std::move(sources));
    active.store(prompts.get(), std::memory_order_release);
    std::fprintf(stderr, "[game-prompts] keyboard and mouse prompts on (MAG_GAME_PROMPTS=0 turns them off)\n");
}
void stop() {
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
