#include "player_settings_desktop.h"
#include "player_settings_portable.h"
#include "player_settings_runtime.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
namespace fs = std::filesystem;
namespace {
struct Gate {
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, released = false;
  void reset() {
    std::lock_guard lock(mutex);
    entered = false;
    released = false;
  }
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    ready.notify_all();
    ready.wait(lock, [this] { return released; });
  }
  void awaitEntry() {
    std::unique_lock lock(mutex);
    ready.wait(lock, [this] { return entered; });
  }
  bool hasEntered() {
    std::lock_guard lock(mutex);
    return entered;
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    ready.notify_all();
  }
};
Gate editing, confirming, acknowledging;
bool edit = true, confirm = true;
int attached, displays;
int displayedWidth = 1920;
std::atomic<int> errors;
unsigned ticks;
std::thread::id frameThread;
mag::PlayerSettings next;
void tick() {
  mag_player_settings_poll();
  ++ticks;
}
void finish() {
  while (mag_player_input_blocked()) {
    tick();
    std::this_thread::yield();
  }
}
void startEdit() {
  editing.reset();
  confirming.reset();
  mag_desktop_request_settings();
  tick();
  editing.awaitEntry();
}
void waitForConfirm() {
  while (!confirming.hasEntered()) {
    tick();
    std::this_thread::yield();
  }
}
void verifyPending() {
  auto before = ticks;
  for (int i = 0; i < 20; ++i)
    tick();
  assert(ticks == before + 20 && mag_player_input_blocked());
}
} // namespace
bool mag_desktop_edit(mag::PlayerSettings &s, bool setup, const char *) {
  assert(mag_player_input_blocked());
  if (!setup) {
    assert(std::this_thread::get_id() != frameThread);
    editing.wait();
  }
  if (edit)
    s = next;
  return edit;
}
void mag_desktop_error(const char *) {
  assert(std::this_thread::get_id() != frameThread);
  ++errors;
  acknowledging.wait();
}
void mag_desktop_attach(const mag::PlayerSettings &) {
  assert(std::this_thread::get_id() == frameThread);
  ++attached;
}
void mag_desktop_apply_display(const mag::PlayerSettings &settings) {
  assert(std::this_thread::get_id() == frameThread);
  ++displays;
  displayedWidth = settings.output_width;
}
bool mag_desktop_confirm_display() {
  assert(mag_player_input_blocked());
  assert(std::this_thread::get_id() != frameThread);
  confirming.wait();
  return confirm;
}
namespace mag {
fs::path create_server_overlay(const fs::path &, const fs::path &,
                               const PlayerSettings &, const std::string &) {
  assert(false);
  return {};
}
} // namespace mag
int main(int argc, char **argv) {
  assert(argc == 2);
  frameThread = std::this_thread::get_id();
  fs::path dir = argv[1];
  fs::create_directories(dir);
  auto path = dir / "settings.json", owned = dir / "overlay";
  fs::create_directory(owned);
  unsetenv("MAG_PLAYER_SETTINGS_PATH");
  assert(mag_player_settings_prepare() == 1);
  setenv("MAG_PLAYER_SETTINGS_PATH", path.c_str(), 1);
  setenv("MAG_PLAYER_OVERLAY_ROOT", owned.c_str(), 1);
  setenv("MAG_PLAYER_SETUP", "1", 1);
  next.server_address = "";
  next.render_scale = 200;
  next.output_width = 1920;
  next.output_height = 1080;
  setenv("PS3RECOMP_RSX_ENGINE", "vtable", 1);
  assert(mag_player_settings_prepare() == 1 && !mag_player_input_blocked());
  bool exists = false;
  auto stored = mag::load_player_settings(path, exists);
  assert(exists && stored.output_width == 1920);
  mag_player_settings_attach();
  assert(attached == 1);
  next = stored;
  next.render_scale = 300;
  next.output_width = 2560;
  next.output_height = 1440;
  confirm = false;
  startEdit();
  verifyPending();
  assert(displays == 0);
  editing.release();
  waitForConfirm();
  verifyPending();
  assert(displays == 1 && displayedWidth == 2560);
  assert(mag::load_player_settings(path, exists).output_width == 1920);
  confirming.release();
  finish();
  assert(displays == 2 && displayedWidth == 1920 &&
         mag::load_player_settings(path, exists).output_width == 1920);
  confirm = true;
  startEdit();
  verifyPending();
  editing.release();
  waitForConfirm();
  verifyPending();
  confirming.release();
  finish();
  stored = mag::load_player_settings(path, exists);
  assert(stored.output_width == 2560 && stored.render_scale == 300);
  auto snapshot =
      mag::load_player_settings(owned / "launch-settings.json", exists);
  assert(snapshot.output_width == 1920 && snapshot.render_scale == 200);
  assert(std::string(getenv("MAG_PLAYER_RENDER_SCALE")) == "200");
  assert(std::string(getenv("PS3RECOMP_RSX_ENGINE")) == "dispatch");
  assert(std::string(getenv("PS3RECOMP_VK_GUEST_PROGRAMS")) == "1");
  // A failed atomic save rolls the preview back on the frame thread. Input
  // stays held until the asynchronous error dialog is acknowledged.
  next = stored;
  next.output_width = 3840;
  next.output_height = 2160;
  auto original = dir / "original-settings.json";
  fs::rename(path, original);
  fs::create_symlink(original, path);
  acknowledging.reset();
  startEdit();
  editing.release();
  waitForConfirm();
  confirming.release();
  while (!acknowledging.hasEntered()) {
    tick();
    std::this_thread::yield();
  }
  assert(errors == 1 && displayedWidth == 2560);
  verifyPending();
  assert(mag::load_player_settings(original, exists).output_width == 2560);
  acknowledging.release();
  finish();
  fs::remove(path);
  fs::rename(original, path);
  unsetenv("MAG_PLAYER_SETUP");
  assert(mag_player_settings_prepare() == 1);
  edit = false;
  startEdit();
  verifyPending();
  editing.release();
  finish();
  next = stored;
  next.anti_aliasing = "fxaa";
  mag::save_player_settings(path, next);
  setenv("MAG_PLAYER_SETUP", "1", 1);
  assert(mag_player_settings_prepare() == 0 && !mag_player_input_blocked());
  return 0;
}
