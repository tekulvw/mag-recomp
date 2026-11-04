/* Update-loop identity (see the header). Every entry returns at once while the
 * adapter's master switch is off. */
#include "mag_render_update_loop.h"
#include "mag_render_adapter_switch.h"
#include <atomic>

namespace {
std::atomic<uint64_t> sequence{};
thread_local const void* current_context{};
thread_local uint64_t current_id{};
thread_local bool accepted{};
}  // namespace

MagRenderUpdateLoopScope::MagRenderUpdateLoopScope(ppu_context* c) noexcept {
    if (!c || !mag_render_hooks_enabled()) return;
    active_ = true;
    previous_context_ = current_context;
    previous_id_ = current_id;
    previous_accepted_ = accepted;
    current_context = c;
    current_id = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    accepted = false;
}

MagRenderUpdateLoopScope::~MagRenderUpdateLoopScope() noexcept {
    if (!active_) return;
    current_context = previous_context_;
    current_id = previous_id_;
    accepted = previous_accepted_;
}

void mag_render_update_accepted(ppu_context* c, double) noexcept {
    if (c && c == current_context && current_id) accepted = true;
}

uint64_t mag_render_update_current(const ppu_context* c) noexcept {
    return c && c == current_context && accepted ? current_id : 0;
}
