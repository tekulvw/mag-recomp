#include "player_settings_desktop.h"
#include "player_settings_portable.h"
#include "player_settings_runtime.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
namespace {
std::atomic<int> blocked;
std::atomic<bool> requested;
mag::PlayerSettings saved;
std::filesystem::path settingsPath;
bool enabled, attached;
[[maybe_unused]] std::filesystem::path utf8path(const char *s) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(s)));
}
const char *env(const char *key) {
  const char *s = std::getenv(key);
  return s && *s ? s : nullptr;
}
std::filesystem::path environmentPath(const char *key) {
#ifdef _WIN32
  std::wstring name;
  for (const char *p = key; *p; ++p)
    name += static_cast<wchar_t>(*p);
  const wchar_t *value = _wgetenv(name.c_str());
  return value && *value ? std::filesystem::path(value)
                         : std::filesystem::path{};
#else
  const char *value = env(key);
  return value ? utf8path(value) : std::filesystem::path{};
#endif
}
void setenvValue(const char *key, const std::string &value) {
#ifdef _WIN32
  if (_putenv_s(key, value.c_str()))
    throw std::runtime_error("Cannot apply launch settings environment.");
#else
  if (setenv(key, value.c_str(), 1))
    throw std::runtime_error("Cannot apply launch settings environment.");
#endif
}
// Platform controls expose only implemented renderer capabilities. Existing
// preferences remain available for review if a different platform saved them.
void capabilities([[maybe_unused]] const mag::PlayerSettings &s) {
  if (!s.server_address.empty() && s.local_account_name.empty())
    throw std::runtime_error("Enter a community account name before playing.");
#if defined(_WIN32) || !defined(MAG_PLAYER_VULKAN_FXAA)
  if (s.anti_aliasing != "off")
    throw std::runtime_error("This renderer supports AA Off. Review settings "
                             "to use a supported value.");
#endif
#ifdef _WIN32
  if (s.render_scale != 100)
    throw std::runtime_error(
        "The Windows renderer supports Native (100%) rendering. Review "
        "settings to use a supported value.");
#endif
}
} // namespace
extern "C" int mag_player_input_blocked() {
  return blocked.load(std::memory_order_acquire);
}
extern "C" void mag_player_set_input_blocked(int value) {
  blocked.store(!!value, std::memory_order_release);
}
void mag_desktop_request_settings() {
  requested.store(true, std::memory_order_release);
}
bool mag_desktop_settings_requested() {
  return requested.exchange(false, std::memory_order_acq_rel);
}
extern "C" int mag_player_settings_prepare() {
  try {
    settingsPath = environmentPath("MAG_PLAYER_SETTINGS_PATH");
    if (settingsPath.empty())
      return 1;
    enabled = true;
    const bool setup = env("MAG_PLAYER_SETUP") != nullptr;
    auto folder = environmentPath("MAG_PLAYER_OVERLAY_ROOT");
    if (folder.empty())
      throw std::runtime_error("The private launch settings folder is unavailable.");
    auto snapshot = folder / "launch-settings.json";
    bool exists = false;
    std::string error;
    try {
      saved = mag::load_player_settings(setup ? settingsPath : snapshot, exists);
      if (!setup && !exists)
        throw std::runtime_error("The launch settings snapshot is missing.");
    } catch (const std::exception &e) {
      saved = mag::PlayerSettings{};
      error = e.what();
    }
    if (error.empty())
      try {
        capabilities(saved);
      } catch (const std::exception &e) {
        error = e.what();
      }
    if (!setup && !error.empty())
      throw std::runtime_error(error);
    if (!error.empty() || (setup && env("MAG_PLAYER_SHOW_SETTINGS")) ||
        (!exists && setup)) {
      mag_player_set_input_blocked(1);
      bool apply = mag_desktop_edit(saved, true, error.c_str());
      mag_player_set_input_blocked(0);
      if (!apply)
        return 0;
      capabilities(saved);
      mag::save_player_settings(settingsPath, saved);
    }
    if (setup)
      mag::save_player_settings(snapshot, saved);
    capabilities(saved);
    if (!saved.server_address.empty()) {
      auto vfs = environmentPath("PS3_VFS_ROOT");
      if (vfs.empty())
        throw std::runtime_error("The original game folder is unavailable.");
      auto overlay = mag::create_server_overlay(
          vfs, folder, saved,
          "220ea0f1cfff8dfac86d4ea72fb68200a092304cbf51a1a3046fcf4783af8cfe");
      auto utf8 = overlay.u8string();
      setenvValue("PS3_VFS_ROOT", std::string(utf8.begin(), utf8.end()));
      setenvValue("PS3_NET_ONLINE", "1");
      setenvValue("MAG_LOCAL_NO_TICKET", "1");
      setenvValue("MAG_LOCAL_ACCOUNT_NAME", saved.local_account_name);
      setenvValue("PS3_NP_ONLINE_ID", saved.local_account_name);
      setenvValue("PS3RECOMP_OSK_INITIAL_TEXT", saved.local_account_name);
      setenvValue("PS3RECOMP_NP_LOCAL_TICKET", "1");
    }
    setenvValue("PS3_AA", saved.anti_aliasing);
    setenvValue("MAG_PLAYER_RENDER_SCALE", std::to_string(saved.render_scale));
#ifndef _WIN32
    if (saved.render_scale != 100 || saved.anti_aliasing == "fxaa") {
      setenvValue("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
      setenvValue("PS3RECOMP_RSX_ENGINE", "dispatch");
    }
#endif
    return 1;
  } catch (const std::exception &e) {
    mag_player_set_input_blocked(0);
    mag_desktop_error(e.what());
    return -1;
  }
}
extern "C" void mag_player_settings_attach() {
  if (enabled && !attached) {
    try {
      mag_desktop_attach(saved);
      attached = true;
    } catch (const std::exception &e) {
      mag_desktop_error(e.what());
      std::exit(1);
    }
  }
}
namespace {
enum class Phase { idle, editing, confirming, error };
struct UIReply {
  bool accepted = false;
  mag::PlayerSettings settings;
};
struct Transaction {
  Phase phase = Phase::idle;
  std::future<UIReply> pending;
  mag::PlayerSettings draft;
  bool displayApplied = false;
};
Transaction &transaction() {
  // OS exit must not join a worker waiting for a still-visible native dialog.
  // The transaction owns its response channel for the lifetime of this process.
  static auto *value = new Transaction;
  return *value;
}
void complete(Transaction &tx) {
  tx.phase = Phase::idle;
  tx.displayApplied = false;
  mag_player_set_input_blocked(0);
}
template <class Job> void launch(Transaction &tx, Phase phase, Job job) {
  tx.pending = std::async(std::launch::async, std::move(job));
  tx.phase = phase;
}
void report(Transaction &tx, const std::string &message) {
  // Roll back only on the frame thread; a modal acknowledgement is another
  // asynchronous transaction phase, so frame pumping continues throughout it.
  std::string detail = message;
  if (tx.displayApplied) {
    try {
      mag_desktop_apply_display(saved);
    } catch (const std::exception &e) {
      detail += " Display restore failed: ";
      detail += e.what();
    }
    tx.displayApplied = false;
  }
  try {
    launch(tx, Phase::error, [detail] {
      mag_desktop_error(detail.c_str());
      return UIReply{};
    });
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[MAG settings] %s (%s)\n", detail.c_str(), e.what());
    complete(tx);
  }
}
void persist(Transaction &tx) {
  mag::save_player_settings(settingsPath, tx.draft);
  saved = tx.draft;
  complete(tx);
}
} // namespace
extern "C" void mag_player_settings_poll() {
  if (!enabled || !attached)
    return;
  auto &tx = transaction();
  if (tx.phase == Phase::idle) {
    if (!mag_desktop_settings_requested())
      return;
    mag_player_set_input_blocked(1);
    auto initial = saved;
    try {
      launch(tx, Phase::editing, [initial]() mutable {
        bool accepted = mag_desktop_edit(initial, false, "");
        return UIReply{accepted, std::move(initial)};
      });
    } catch (const std::exception &e) {
      report(tx, e.what());
    }
    return;
  }
  // Discard repeat shortcuts while this editor owns input; do not reopen the
  // panel immediately when a held key or queued menu event is released.
  mag_desktop_settings_requested();
  if (tx.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  const auto phase = tx.phase;
  try {
    auto response = tx.pending.get();
    if (phase == Phase::error) {
      complete(tx);
      return;
    }
    if (phase == Phase::editing) {
      if (!response.accepted) {
        complete(tx);
        return;
      }
      capabilities(response.settings);
      tx.draft = std::move(response.settings);
      const bool changed = tx.draft.display_mode != saved.display_mode ||
                           tx.draft.output_width != saved.output_width ||
                           tx.draft.output_height != saved.output_height;
      if (!changed) {
        persist(tx);
        return;
      }
      // Set before applying so even a partial platform failure is restored.
      tx.displayApplied = true;
      mag_desktop_apply_display(tx.draft);
      launch(tx, Phase::confirming,
             [] { return UIReply{mag_desktop_confirm_display(), {}}; });
      return;
    }
    if (!response.accepted) {
      mag_desktop_apply_display(saved);
      complete(tx);
      return;
    }
    persist(tx);
  } catch (const std::exception &e) {
    if (phase == Phase::error) {
      std::fprintf(stderr, "[MAG settings] Error acknowledgement failed: %s\n",
                   e.what());
      complete(tx);
    } else
      report(tx, e.what());
  }
}
