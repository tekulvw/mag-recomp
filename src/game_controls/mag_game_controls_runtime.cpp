/* Lifetime of the game controls adapter (docs/native-controls.md, section 4): settings
 * registration, preferences, the hook callback and the extension callbacks. Compiled only with
 * MAG_GAME_CONTROLS; MAG_GAME_CONTROLS=0 in the environment turns it off at run time. */
#include "mag_extension.h"
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_catalog.h"
#include "mag_game_controls_memory.h"
#include "player_controls_settings.h"
#include "player_controls_store.h"
#include "player_input_capture.h"
#include "player_input_desktop.h"
#include "ppu_recomp.h"
#include "memory/vm.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
namespace {
std::unique_ptr<MagGameControlsAdapter> adapter;
std::atomic<uint64_t> sessions{};
std::atomic<uint64_t> owner_thread{UINT64_MAX};
const char* last_status = nullptr;
std::mutex settings_lifetime;
uint64_t settings_session{};
std::atomic<bool> explicit_apply{false};

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
void capture(bool on) { mag_input_desktop_set_enabled(on); }
void mouse_capture(bool on) { mag_input_desktop_set_mouse_enabled(on); }
bool waiting(const char* status) {
    return !std::strcmp(status, "waiting for input poll") || !std::strcmp(status, "waiting for normal input poll");
}
bool hook(MagControlsHook event, ppu_context* ctx) noexcept {
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
        if (!last_status || std::strcmp(last_status, visible)) {
            std::fprintf(stderr, "[game-controls] %s\n", visible);
            last_status = visible;
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
        std::sort(catalog.begin(), catalog.end());
        catalog.erase(std::unique(catalog.begin(), catalog.end()), catalog.end());
        if (std::filesystem::exists(path)) profile = mag::mag_controls_load_profile(path, catalog);
        else if (!mag::mag_controls_save_profile(path, profile, catalog))
            throw std::runtime_error("cannot create the controls preferences file");
    }
    mag_game_controls_upgrade_profile(profile);
    if (auto mode = startup_mode()) profile.mode = *mode;
    owner_thread.store(UINT64_MAX, std::memory_order_release);
    last_status = nullptr;
    adapter = std::make_unique<MagGameControlsAdapter>(MagGameControlsMemory{nullptr, read, write}, ++sessions,
                                                       profile, capture, mouse_capture);
    register_settings(path, sessions.load());
    mag_game_controls_install_hooks(hook);
    std::fprintf(stderr, "[game-controls] registered; controller sticks keep the native response; awaiting the local player\n");
}
void stop() {
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
