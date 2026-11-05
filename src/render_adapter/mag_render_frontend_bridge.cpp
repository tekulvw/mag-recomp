/* Configuration lifetime and Shadow view-corner observers (see the header).
 * Reads only guest memory the hooked original is about to use or has just
 * written; never writes guest memory. Every entry returns at once while the
 * adapter's master switch is off. */
#include "mag_render_frontend_bridge.h"
#include "mag_render_adapter_switch.h"
#if defined(MAG_RENDER_ADAPTER_COHORT) && MAG_RENDER_ADAPTER_COHORT
#include "mag_render_cohort_bridge.h"
#endif
#include "ppu_recomp.h"
#include <atomic>
#include <exception>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <utility>
extern "C" uint32_t ppu_vm_size;
extern "C" uint8_t* vm_base;
namespace {
static_assert(sizeof(MagRenderConfigEvent) == 80 && sizeof(MagRenderConfigSnapshot) == 48, "render config ABI");
static_assert(sizeof(MagRenderShadowCallInput) == 160 && sizeof(MagRenderShadowCallResult) == 232, "shadow call ABI");

// Configuration event kinds of mag_render_adapter_config_event.
enum : uint32_t { VmBegin = 0, VmEnd = 1, InitEnter = 2, InitComplete = 3, Shutdown = 4, InitAbandoned = 5 };
// Guest module slots: the TOC-relative pair naming the Shadow configuration
// manager and its state word, and the manager's three parameters.
constexpr uint64_t toc_state_slot = 0x4448, toc_manager_slot = 0x444c;
constexpr uint64_t manager_parameters = 0x1504, manager_corners = 0x15b0, corner_bytes = 14 * 16;
constexpr uint32_t state_initialized = 2;
// The engine's TOC, the helper's only caller's return address, and the
// TOC-relative module slots and constant the helper reads.
constexpr uint64_t engine_toc = 0x1b94d70, helper_return = 0xd15794;
constexpr uint64_t toc_manager = 0x2858, toc_state = 0x285c, toc_half = 0x2490;
constexpr uint32_t half = 0x3f000000;

std::mutex mutex;
std::atomic<bool> begun{};
uint64_t vm_serial{}, invocation_serial{};
std::atomic<uint64_t> active_vm{};
thread_local unsigned init_depth{};

bool range(uint64_t address, uint64_t bytes) {
    const uint64_t end = ppu_vm_size ? ppu_vm_size : (1ull << 32);
    return vm_base && address >= 0x10000 && address < end && bytes <= end - address;
}

MagRenderConfigEvent event(uint32_t kind, uint64_t vm, uint64_t context = 0, uint64_t owner = 0,
                           uint64_t invocation = 0, uint32_t manager = 0, uint32_t state = 0) {
    MagRenderConfigEvent e{};
    e.abi_version = 1;
    e.struct_bytes = sizeof e;
    e.kind = kind;
    e.vm = vm;
    e.context = context;
    e.owner = owner;
    e.invocation = invocation;
    e.manager = manager;
    e.state_address = state;
    return e;
}

bool source(ppu_context* c, uint32_t& manager, uint32_t& state) {
    if (!c || c->gpr[2] > UINT32_MAX || !range(c->gpr[2] + toc_state_slot, 8)) return false;
    state = vm_read32(c->gpr[2] + toc_state_slot);
    manager = vm_read32(c->gpr[2] + toc_manager_slot);
    return range(state, 4) && range(uint64_t(manager) + manager_parameters, 12);
}

void revoke_vm() {
    const auto old = active_vm.exchange(0);
    if (old) (void)mag_render_adapter_config_event(event(VmEnd, old));
#if defined(MAG_RENDER_ADAPTER_COHORT) && MAG_RENDER_ADAPTER_COHORT
    mag_render_cohort_vm(0);
#endif
}

void begin_locked() {
    begun.store(true, std::memory_order_release);
    revoke_vm();
    if (vm_serial == std::numeric_limits<uint64_t>::max()) return;
    const auto next = ++vm_serial;
    if (mag_render_adapter_config_event(event(VmBegin, next)) == 0) {
        active_vm = next;
#if defined(MAG_RENDER_ADAPTER_COHORT) && MAG_RENDER_ADAPTER_COHORT
        mag_render_cohort_vm(next);
#endif
    }
}

// A lifetime begins with the first hook that runs while the switch is on.
void ensure_vm_locked() {
    if (!begun.load(std::memory_order_relaxed)) begin_locked();
}
}  // namespace

void mag_render_frontend_vm_begin(const char*) noexcept {
    if (!mag_render_hooks_enabled()) return;
    try {
        std::lock_guard lock(mutex);
        begin_locked();
    } catch (...) {
        revoke_vm();
    }
}

void mag_render_frontend_vm_end() noexcept {
    if (!mag_render_hooks_enabled()) return;
    try {
        std::lock_guard lock(mutex);
        begun.store(true, std::memory_order_release);
        revoke_vm();
    } catch (...) {
        revoke_vm();
    }
}

void mag_render_frontend_vm_ensure() noexcept {
    if (begun.load(std::memory_order_acquire) || !mag_render_hooks_enabled()) return;
    try {
        std::lock_guard lock(mutex);
        ensure_vm_locked();
    } catch (...) {
        revoke_vm();
    }
}

MagRenderShadowConfigInitScope::MagRenderShadowConfigInitScope(ppu_context* c) noexcept
    : context(c), exceptions(std::uncaught_exceptions()), host_owner(std::this_thread::get_id()) {
    if (!mag_render_hooks_enabled()) return;
    try {
        std::lock_guard lock(mutex);
        ensure_vm_locked();
        if (!active_vm) return;
        // Read the actual module slots; no caller-provided ready flag or generation.
        if (init_depth || !source(c, manager, state) || invocation_serial == std::numeric_limits<uint64_t>::max()) {
            revoke_vm();
            return;
        }
        vm = active_vm;
        invocation = ++invocation_serial;
        owner = c->thread_id;
        auto e = event(InitEnter, vm, reinterpret_cast<uintptr_t>(c), owner, invocation, manager, state);
        e.state_value = vm_read32(state);
        active = mag_render_adapter_config_event(e) == 0;
        if (active) ++init_depth;
    } catch (...) {
        revoke_vm();
    }
}

MagRenderShadowConfigInitScope::~MagRenderShadowConfigInitScope() noexcept {
    if (!active) return;
    try {
        std::lock_guard lock(mutex);
        if (init_depth) --init_depth;
        if (vm != active_vm) return;
        auto e = event(InitAbandoned, vm, reinterpret_cast<uintptr_t>(context), owner, invocation, manager, state);
        uint32_t current_manager{}, current_state{};
        // Complete only when the same guest thread returns normally with the
        // same manager and state slots and the state reads initialized.
        if (std::uncaught_exceptions() == exceptions && host_owner == std::this_thread::get_id()
            && context->thread_id == owner && source(context, current_manager, current_state)
            && manager == current_manager && state == current_state && vm_read32(state) == state_initialized) {
            e.kind = InitComplete;
            e.state_value = state_initialized;
            for (unsigned i = 0; i < 3; ++i) e.parameters[i] = vm_read32(uint64_t(manager) + manager_parameters + 4 * i);
        }
        (void)mag_render_adapter_config_event(e);
    } catch (...) {
        revoke_vm();
    }
}

MagRenderShadowConfigShutdownScope::MagRenderShadowConfigShutdownScope(ppu_context* c) noexcept {
    if (!mag_render_hooks_enabled()) return;
    try {
        std::lock_guard lock(mutex);
        ensure_vm_locked();
        uint32_t manager{}, state{};
        if (!active_vm || !source(c, manager, state) || vm_read32(state) != state_initialized) return;
        if (invocation_serial == std::numeric_limits<uint64_t>::max()) {
            revoke_vm();
            return;
        }
        // The original shutdown changes the state before it tears the
        // configuration down, so report the end first.
        auto e = event(Shutdown, active_vm, reinterpret_cast<uintptr_t>(c), c->thread_id, ++invocation_serial, manager, state);
        e.state_value = state_initialized;
        (void)mag_render_adapter_config_event(e);
    } catch (...) {
        revoke_vm();
    }
}

namespace {
MagRenderShadowViewStats view_stats{};
#if defined(MAG_RENDER_ADAPTER_SHADOW_VIEW) && MAG_RENDER_ADAPTER_SHADOW_VIEW
constexpr bool shadow_view_enabled = true;
#else
constexpr bool shadow_view_enabled = false;
#endif

void increment(uint64_t& counter) {
    if (counter == std::numeric_limits<uint64_t>::max()) view_stats.lost_records = true;
    else ++counter;
}

bool shadow_readset(ppu_context* c, MagRenderShadowCallInput& input) {
    if (!c || c->gpr[2] != engine_toc || c->lr != helper_return || c->gpr[3] > UINT32_MAX || c->gpr[4] > UINT32_MAX)
        return false;
    const auto manager = uint32_t(c->gpr[3]), view = uint32_t(c->gpr[4]);
    if ((manager & 15) || (view & 15) || !range(engine_toc - toc_state, 8) || !range(engine_toc - toc_half, 4)
        || !range(uint64_t(manager) + manager_parameters, 12) || !range(uint64_t(manager) + manager_corners, corner_bytes)
        || !range(uint64_t(view) + 0x10, 64) || !range(uint64_t(view) + 0x60, 4) || !range(uint64_t(view) + 0xa24, 4))
        return false;
    // Refuse inputs that overlap the corner output, so the result never depends on read order.
    for (auto span : {std::pair<uint64_t, uint64_t>{uint64_t(view) + 0x10, 64},
                      {uint64_t(view) + 0x60, 4}, {uint64_t(view) + 0xa24, 4}})
        if (span.first < uint64_t(manager) + manager_corners + corner_bytes
            && uint64_t(manager) + manager_corners < span.first + span.second)
            return false;
    const auto current = mag_render_adapter_config_snapshot(active_vm, manager);
    if (current.status || vm_read32(engine_toc - toc_manager) != manager
        || vm_read32(engine_toc - toc_state) != current.state_address || !range(current.state_address, 4)
        || vm_read32(current.state_address) != state_initialized || vm_read32(engine_toc - toc_half) != half)
        return false;
    input.abi_version = 1;
    input.struct_bytes = sizeof input;
    input.vm = current.vm;
    input.initialization = current.initialization;
    input.manager = manager;
    input.view = view;
    input.state_address = current.state_address;
    input.state_value = state_initialized;
    for (unsigned i = 0; i < 3; ++i) {
        input.parameters[i] = vm_read32(uint64_t(manager) + manager_parameters + 4 * i);
        if (input.parameters[i] != current.parameters[i]) return false;
    }
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned lane = 0; lane < 4; ++lane)
            input.pose_rows[row][lane] = vm_read32(uint64_t(view) + 0x10 + row * 16 + lane * 4);
    input.view_scalar_60 = vm_read32(uint64_t(view) + 0x60);
    input.view_scalar_a24 = vm_read32(uint64_t(view) + 0xa24);
    return true;
}
}  // namespace

MagRenderShadowViewStats mag_render_frontend_shadow_view_stats() noexcept {
    try {
        std::lock_guard lock(mutex);
        return view_stats;
    } catch (...) {
        MagRenderShadowViewStats failed{};
        failed.lost_records = true;
        return failed;
    }
}

MagRenderShadowCornerScope::MagRenderShadowCornerScope(ppu_context* c) noexcept
    : context(c), exceptions(std::uncaught_exceptions()), host_owner(std::this_thread::get_id()) {
    if (!shadow_view_enabled || !mag_render_hooks_enabled()) return;
    try {
        std::lock_guard lock(mutex);
        ensure_vm_locked();
        if (!active_vm) return;
        if (view_stats.vm != active_vm) {
            view_stats = {};
            view_stats.vm = active_vm;
        }
        // Counts entries of the instrumented lifted helper only; other routes
        // into the original (the interpreter, for one) are not covered.
        increment(view_stats.original_lifted_entries);
        if (!shadow_readset(c, input) || invocation_serial == std::numeric_limits<uint64_t>::max()) {
            increment(view_stats.declined);
            return;
        }
        input.context = reinterpret_cast<uintptr_t>(c);
        input.owner = c->thread_id;
        input.invocation = ++invocation_serial;
        stack = c->gpr[1];
        link = c->lr;
        result = mag_render_adapter_shadow_call(input);
        if (result.status) {
            increment(view_stats.declined);
            return;
        }
        increment(view_stats.publications);
        active = true;
    } catch (...) {
        revoke_vm();
    }
}

MagRenderShadowCornerScope::~MagRenderShadowCornerScope() noexcept {
    if (!active) return;
    try {
        std::lock_guard lock(mutex);
        if (input.vm != active_vm) return;
        MagRenderShadowCallInput current{};
        bool valid = std::uncaught_exceptions() == exceptions && host_owner == std::this_thread::get_id()
            && context->thread_id == input.owner && context->gpr[1] == stack && context->lr == link
            && context->gpr[2] == engine_toc;
        // The helper need not preserve its argument registers; re-read the
        // inputs from the captured addresses through a local context view.
        ppu_context check{};
        check.gpr[2] = engine_toc;
        check.gpr[3] = input.manager;
        check.gpr[4] = input.view;
        check.lr = link;
        valid = valid && shadow_readset(&check, current) && current.vm == input.vm
            && current.initialization == input.initialization;
        for (unsigned i = 0; i < 3 && valid; ++i) valid = current.parameters[i] == input.parameters[i];
        for (unsigned row = 0; row < 4 && valid; ++row)
            for (unsigned lane = 0; lane < 4 && valid; ++lane)
                valid = current.pose_rows[row][lane] == input.pose_rows[row][lane];
        valid = valid && current.view_scalar_60 == input.view_scalar_60 && current.view_scalar_a24 == input.view_scalar_a24;
        if (!valid) {
            increment(view_stats.invalidated);
            return;
        }
        bool equal = true;
        for (unsigned vector = 0; vector < 14; ++vector)
            for (unsigned lane = 0; lane < 4; ++lane)
                equal = equal && vm_read32(uint64_t(input.manager) + manager_corners + 16 * vector + 4 * lane)
                    == result.vectors[vector][lane];
        increment(equal ? view_stats.matched : view_stats.mismatched);
    } catch (...) {
        revoke_vm();
    }
}
