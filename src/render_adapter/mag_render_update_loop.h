#pragma once
/* Bridges from the update-loop observer hooks (tools/render_hooks/update_loop.py).
 * A scope marks one call of the engine's update function on a guest thread; the
 * accepted call reports that the update took its time step. Later hooks on the
 * same thread use mag_render_update_current() as the update identity. Observers
 * only: nothing here writes guest memory or changes original control flow. */
#include <cstdint>
struct ppu_context;

class MagRenderUpdateLoopScope {
public:
    explicit MagRenderUpdateLoopScope(ppu_context*) noexcept;
    ~MagRenderUpdateLoopScope() noexcept;
    MagRenderUpdateLoopScope(const MagRenderUpdateLoopScope&) = delete;
    MagRenderUpdateLoopScope& operator=(const MagRenderUpdateLoopScope&) = delete;
private:
    bool active_{};
    const void* previous_context_{};
    uint64_t previous_id_{};
    bool previous_accepted_{};
};

/* The update in progress on this thread accepted its time step. */
void mag_render_update_accepted(ppu_context*, double step) noexcept;

/* Nonzero identity of the accepted update in progress on this thread for this
 * guest context, or zero when there is none (or the master switch is off). */
uint64_t mag_render_update_current(const ppu_context*) noexcept;
