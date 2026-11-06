#pragma once
/* Bridges from the configuration and shadow-view observer hooks in prepared
 * lifted sources (tools/render_hooks) to the adapter's C entry points
 * (crates/mag-render-adapter-core, src/ffi.rs). Observers only: original calls,
 * drains, guest writes and return values are unchanged, and nothing here can
 * suppress original work. */
#include <cstdint>
#include <thread>
struct ppu_context;
extern "C" {
struct MagRenderConfigEvent {
    uint32_t abi_version, struct_bytes, kind, reserved0;
    uint64_t vm, context, owner, invocation;
    uint32_t manager, state_address, state_value, reserved1, parameters[3], reserved2;
};
struct MagRenderConfigSnapshot {
    uint32_t status, reserved0;
    uint64_t vm, initialization;
    uint32_t manager, state_address, parameters[3], reserved1;
};
uint32_t mag_render_adapter_config_event(MagRenderConfigEvent);
MagRenderConfigSnapshot mag_render_adapter_config_snapshot(uint64_t, uint32_t);

struct MagRenderShadowCallInput {
    uint32_t abi_version, struct_bytes, reserved[2];
    uint64_t vm, initialization, context, owner, invocation;
    uint32_t manager, view, state_address, state_value, parameters[3], pose_rows[4][4];
    uint32_t view_scalar_60, view_scalar_a24, reserved_tail;
};
struct MagRenderShadowCallResult { uint32_t status, reserved, vectors[14][4]; };
MagRenderShadowCallResult mag_render_adapter_shadow_call(MagRenderShadowCallInput);
}

/* Guest lifetime. The first hook to run with the master switch on begins a
 * lifetime when none has begun; a host may also begin and end one explicitly. */
void mag_render_frontend_vm_begin(const char* elf) noexcept;
void mag_render_frontend_vm_end() noexcept;
/* Begins a lifetime if none has begun yet; for hooks of other stages, called
 * with no bridge lock held. */
void mag_render_frontend_vm_ensure() noexcept;

/* Scope at the entry of the configuration manager's init function. */
class MagRenderShadowConfigInitScope {
    ppu_context* context{};
    uint64_t vm{}, invocation{}, owner{};
    uint32_t manager{}, state{};
    int exceptions{};
    std::thread::id host_owner;
    bool active{};
public:
    explicit MagRenderShadowConfigInitScope(ppu_context*) noexcept;
    ~MagRenderShadowConfigInitScope() noexcept;
    MagRenderShadowConfigInitScope(const MagRenderShadowConfigInitScope&) = delete;
    MagRenderShadowConfigInitScope& operator=(const MagRenderShadowConfigInitScope&) = delete;
};

/* Scope at the entry of the configuration manager's shutdown function. */
class MagRenderShadowConfigShutdownScope {
public:
    explicit MagRenderShadowConfigShutdownScope(ppu_context*) noexcept;
};

/* Counts for the shadow-view observer, per guest lifetime. */
struct MagRenderShadowViewStats {
    uint64_t vm, original_lifted_entries, publications, matched, mismatched, declined, invalidated;
    bool lost_records;
};
MagRenderShadowViewStats mag_render_frontend_shadow_view_stats() noexcept;

/* Scope at the entry of the Shadow view-corner helper: asks the adapter for
 * its result on entry and compares it with the original's on return. */
class MagRenderShadowCornerScope {
    ppu_context* context{};
    uint64_t stack{}, link{};
    int exceptions{};
    std::thread::id host_owner;
    bool active{};
    MagRenderShadowCallInput input{};
    MagRenderShadowCallResult result{};
public:
    explicit MagRenderShadowCornerScope(ppu_context*) noexcept;
    ~MagRenderShadowCornerScope() noexcept;
    MagRenderShadowCornerScope(const MagRenderShadowCornerScope&) = delete;
    MagRenderShadowCornerScope& operator=(const MagRenderShadowCornerScope&) = delete;
};
