/* Lifetime of the game controls adapter (docs/native-controls.md, section 4): settings
 * registration, preferences, the hook callback and the extension callbacks. Compiled only with
 * MAG_GAME_CONTROLS; MAG_GAME_CONTROLS=0 in the environment turns it off at run time. */
#include "mag_session_monitor.h"
#include "mag_extension.h"
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_catalog.h"
#include "mag_game_controls_memory.h"
#include "mag_game_controls_push_to_talk.h"
#include "player_controls_settings.h"
#include "player_controls_store.h"
#include "player_input_capture.h"
#include "player_input_desktop.h"
#include "ppu_recomp.h"
#include "memory/vm.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
bool mag_game_prompts_hook(MagControlsHook, ppu_context*) noexcept;  // mag_game_controls_prompts_runtime.cpp
namespace {
std::unique_ptr<MagGameControlsAdapter> adapter;
std::atomic<uint64_t> sessions{};
std::atomic<uint64_t> owner_thread{UINT64_MAX};
std::string last_status;  // what was printed last (statuses may name an error)
std::mutex settings_lifetime;
uint64_t settings_session{};
std::atomic<bool> explicit_apply{false};
std::atomic<bool> running{false};  // the adapter exists: push to talk can be read

bool enabled() {
    const char* value = std::getenv("MAG_GAME_CONTROLS");
    return !value || std::strcmp(value, "0");
}
bool in_range(uint32_t address, size_t size) {
    return mag_game_controls_memory_range(VM_READY, ppu_vm_size, address, size);
}
bool read(void*, uint32_t address, void* output, size_t size) {
    if (!in_range(address, size)) return false;
    std::memcpy(output, vm_to_host(address), size);
    return true;
}
bool write(void*, uint32_t address, const void* input, size_t size) {
    if (!in_range(address, size)) return false;
    std::memcpy(vm_to_host(address), input, size);
    return true;
}
std::filesystem::path preferences_path() {
    if (const char* path = std::getenv("MAG_GAME_CONTROLS_PATH"); path && *path) return path;
    if (const char* path = std::getenv("MAG_PLAYER_SETTINGS_PATH"); path && *path)
        return std::filesystem::path(path).parent_path() / "controls.json";
    return {};
}
std::optional<mag::InputMode> startup_mode() {
    if (explicit_apply.load(std::memory_order_acquire)) return {};
    const char* mode = std::getenv("MAG_GAME_CONTROLS_MODE");
    if (!mode || !*mode) return {};
    if (!std::strcmp(mode, "auto")) return mag::InputMode::Auto;
    if (!std::strcmp(mode, "kbm")) return mag::InputMode::KBM;
    if (!std::strcmp(mode, "controller")) return mag::InputMode::Controller;
    throw std::runtime_error("MAG_GAME_CONTROLS_MODE must be auto, kbm or controller");
}
void register_settings(const std::filesystem::path& path, uint64_t session) {
    if (path.empty()) return;
    mag::ControlsSettingsRegistration registration;
    registration.defaults = mag_game_controls_default_profile();
    registration.path = path;
    registration.validate = mag_game_controls_validate_profile;
    registration.upgrade = mag_game_controls_upgrade_profile;
    registration.mode_override = startup_mode();
    registration.actions = mag_game_controls_descriptors();
    const auto retired = mag_game_controls_retired_actions();
    registration.retired.assign(retired.begin(), retired.end());
    if (session) {
        {
            std::lock_guard lock(settings_lifetime);
            settings_session = session;
        }
        registration.apply = [session](const mag::ControlsProfile& next) {
            std::lock_guard lock(settings_lifetime);
            if (settings_session != session || !adapter)
                throw std::runtime_error("The game session ended; saved controls apply next launch");
            adapter->request_profile(next);
            explicit_apply.store(true, std::memory_order_release);
        };
    } else {
        // Before guest allocation Apply records the player's choice only; VM begin loads it, and
        // the launch environment's mode override no longer replaces it.
        registration.apply = [](const mag::ControlsProfile&) {
            std::lock_guard lock(settings_lifetime);
            if (adapter || settings_session)
                throw std::runtime_error("Startup ended; reopen Controls to apply the saved profile to the active game");
            explicit_apply.store(true, std::memory_order_release);
        };
    }
    mag::mag_controls_settings_register(std::move(registration));
}
void census(const char* line) { std::fprintf(stderr, "[game-controls] %s\n", line); }
// The census is on with MAG_GAME_CONTROLS_TRACE=1, or when a file named controls-trace sits next to
// the controls preferences: the packaged players pass no MAG_ variables to the engine.
bool tracing(const std::filesystem::path& preferences) {
    const char* value = std::getenv("MAG_GAME_CONTROLS_TRACE");
    if (value && !std::strcmp(value, "1")) return true;
    std::error_code ignored;
    return !preferences.empty() && std::filesystem::exists(preferences.parent_path() / "controls-trace", ignored);
}
void capture(bool on) { mag_input_desktop_set_enabled(on); }
void mouse_capture(bool on) { mag_input_desktop_set_mouse_enabled(on); }
bool waiting(const char* status) {
    return !std::strcmp(status, "waiting for input poll") || !std::strcmp(status, "waiting for normal input poll");
}
const bool trace_hooks = [] { const char* v = std::getenv("MAG_GAME_CONTROLS_TRACE"); return v && *v && std::strcmp(v, "0"); }();
// Opt-in cost profile (MAG_GAME_CONTROLS_TRACE=1): the owner thread's hook calls and the time spent
// handling them, every five seconds per update tick, with each event above 2% of that time listed by
// its number in MagControlsHook.
constexpr unsigned hook_kinds = unsigned(MagControlsHook::AssistCleanupExit) + 1;
unsigned cost_calls[hook_kinds]{};
int64_t cost_ns[hook_kinds]{};
unsigned cost_ticks{};
std::chrono::steady_clock::time_point cost_next{};
void trace_cost(MagControlsHook event, std::chrono::steady_clock::time_point start) {
    const auto now = std::chrono::steady_clock::now();
    const unsigned e = unsigned(event);
    ++cost_calls[e];
    cost_ns[e] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    cost_ticks += event == MagControlsHook::OuterEnter;
    if (now < cost_next) return;
    if (cost_ticks) {
        unsigned calls = 0;
        int64_t ns = 0;
        for (unsigned i = 0; i < hook_kinds; ++i) calls += cost_calls[i], ns += cost_ns[i];
        std::fprintf(stderr, "[game-controls] trace cost: %u ticks, %u hook calls (%.1f per tick), %.3f ms per tick in hooks;",
                     cost_ticks, calls, double(calls) / cost_ticks, double(ns) / 1e6 / cost_ticks);
        for (unsigned i = 0; i < hook_kinds; ++i)
            if (cost_ns[i] * 50 > ns) std::fprintf(stderr, " e%u %u calls %.3f ms/tick", i, cost_calls[i], double(cost_ns[i]) / 1e6 / cost_ticks);
        std::fprintf(stderr, "\n");
    }
    std::fill(std::begin(cost_calls), std::end(cost_calls), 0u);
    std::fill(std::begin(cost_ns), std::end(cost_ns), int64_t{0});
    cost_ticks = 0;
    cost_next = now + std::chrono::seconds(5);
}
bool hook_owned(MagControlsHook event, ppu_context* ctx) noexcept;
bool hook(MagControlsHook event, ppu_context* ctx) noexcept {
    // Button prompts (section 12) run on whichever thread asks for UI text.
    if (event == MagControlsHook::PromptText || event == MagControlsHook::PromptTextDone)
        return mag_game_prompts_hook(event, ctx);
    if (!trace_hooks) return hook_owned(event, ctx);
    const auto start = std::chrono::steady_clock::now();
    const bool owned_before = owner_thread.load(std::memory_order_acquire) == (ctx ? ctx->thread_id : UINT64_MAX);
    const bool handled = hook_owned(event, ctx);
    if (owned_before || (ctx && owner_thread.load(std::memory_order_acquire) == ctx->thread_id)) trace_cost(event, start);
    return handled;
}
bool hook_owned(MagControlsHook event, ppu_context* ctx) noexcept {
    if (!adapter || !ctx) return false;
    if (event == MagControlsHook::OuterEnter) {
        uint64_t none = UINT64_MAX;
        (void)owner_thread.compare_exchange_strong(none, ctx->thread_id);
    }
    if (owner_thread.load(std::memory_order_acquire) != ctx->thread_id) return false;
    const bool handled = adapter->dispatch(event, {ctx->gpr, ctx->fpr, ctx->lr, ctx->thread_id});
    const char* status = adapter->status();
    if (!waiting(status)) {
        const bool attached = !std::strcmp(status, "on-foot Rust input attached") ||
            !std::strcmp(status, "on-foot mouse prepared");
        const char* visible = attached ? "on-foot controls attached" : status;
        if (last_status != visible) {
            std::fprintf(stderr, "[game-controls] %s\n", visible);
            last_status = visible;
            // The session summary's phase: on foot is a match; the native menu buttons are menus. The
            // local player's handler going away is the match's teardown (the level unloads), which
            // ends the match at once rather than when the menu's buttons attach seconds later.
            if (attached) mag_session_set_phase(MAG_SESSION_PHASE_MATCH);
            else if (!std::strcmp(status, "native menu buttons attached")) mag_session_set_phase(MAG_SESSION_PHASE_MENU);
            else if (!std::strcmp(status, "owned input handler removed")) mag_session_match_ended();
        }
    }
    return handled;
}

void prepare(const char*) {
    if (enabled()) register_settings(preferences_path(), 0);
}
void start(const char*) {
    if (!enabled()) {
        std::fprintf(stderr, "[game-controls] off (MAG_GAME_CONTROLS=0)\n");
        return;
    }
    if (adapter) throw std::runtime_error("game controls already active");
    auto profile = mag_game_controls_default_profile();
    const auto path = preferences_path();
    if (!path.empty()) {
        std::vector<mag::ActionId> catalog;
        for (const auto& binding : profile.bindings) catalog.push_back(binding.action);
        // A profile saved by an older catalog may bind retired actions; the upgrade below removes them.
        for (const auto action : mag_game_controls_retired_actions()) catalog.push_back(action);
        std::sort(catalog.begin(), catalog.end());
        catalog.erase(std::unique(catalog.begin(), catalog.end()), catalog.end());
        if (std::filesystem::exists(path)) profile = mag::mag_controls_load_profile(path, catalog);
        else if (!mag::mag_controls_save_profile(path, profile, catalog))
            throw std::runtime_error("cannot create the controls preferences file");
    }
    mag_game_controls_upgrade_profile(profile);
    if (auto mode = startup_mode()) profile.mode = *mode;
    owner_thread.store(UINT64_MAX, std::memory_order_release);
    last_status.clear();
    adapter = std::make_unique<MagGameControlsAdapter>(MagGameControlsMemory{nullptr, read, write}, ++sessions,
                                                       profile, capture, mouse_capture);
    if (tracing(path)) {
        adapter->set_census(census);
        std::fprintf(stderr, "[game-controls] census on: each distinct input handler is described once\n");
    }
    register_settings(path, sessions.load());
    mag_game_controls_install_hooks(hook);
    running.store(true, std::memory_order_release);
    std::fprintf(stderr, "[game-controls] registered; controller sticks keep the native response; awaiting the local player\n");
}
void stop() {
    running.store(false, std::memory_order_release);
    mag::mag_controls_settings_unregister();
    mag_game_controls_install_hooks(nullptr);
    {
        std::lock_guard lock(settings_lifetime);
        settings_session = 0;
        adapter.reset();
    }
    mag_input_capture_set_keyboard_owner(0);
    mag_input_desktop_set_enabled(0);
}

}  // namespace

extern "C" int mag_game_controls_push_to_talk(int* held, int* focused) {
    if (held) *held = 0;
    if (focused) *focused = 0;
    if (!running.load(std::memory_order_acquire)) return 0;
    const auto state = mag::mag_controls_host_action(MagGameControlsPushToTalk);
    if (held) *held = state.held;
    if (focused) *focused = state.focused;
    return 1;
}

namespace {
const bool registered = [] {
    MagExtensionCallbacks callbacks{};
    callbacks.prepare = prepare;
    callbacks.start = start;
    callbacks.stop = stop;
    if (mag_extension_register(&callbacks) != 0) {
        std::fprintf(stderr, "[game-controls] extension callbacks are full\n");
        std::abort();
    }
    return true;
}();
}  // namespace
