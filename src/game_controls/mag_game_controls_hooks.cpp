#include "mag_game_controls_hooks.h"
#include <atomic>
namespace {
std::atomic<MagControlsHookCallback> installed{};
}
void mag_game_controls_install_hooks(MagControlsHookCallback callback) noexcept {
    installed.store(callback, std::memory_order_release);
}
bool mag_game_controls_hook(MagControlsHook event, ppu_context* context) noexcept {
    const auto callback = installed.load(std::memory_order_acquire);
    return context && callback && callback(event, context);
}
// The scope keeps the callback it saw at entry so an exit is never delivered without its entry.
MagGameControlsHookScope::MagGameControlsHookScope(MagControlsHook enter, MagControlsHook exit, ppu_context* context) noexcept
    : callback_(context ? installed.load(std::memory_order_acquire) : nullptr), exit_(exit), context_(context) {
    if (callback_) callback_(enter, context_);
}
MagGameControlsHookScope::~MagGameControlsHookScope() {
    if (callback_) callback_(exit_, context_);
}
