#include "player_settings_desktop.h"
#include "player_settings_portable.h"
#include "player_settings_runtime.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
namespace fs = std::filesystem;
namespace {
bool edit = true, confirm = true;
int attached, displays, errors;
mag::PlayerSettings next;
} // namespace
bool mag_desktop_edit(mag::PlayerSettings &s, bool, const char *) {
  assert(mag_player_input_blocked());
  if (edit)
    s = next;
  return edit;
}
void mag_desktop_error(const char *) { ++errors; }
void mag_desktop_attach(const mag::PlayerSettings &) { ++attached; }
void mag_desktop_apply_display(const mag::PlayerSettings &) { ++displays; }
bool mag_desktop_confirm_display() {
  assert(mag_player_input_blocked());
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
  setenv("PS3RECOMP_RSX_ENGINE", "vtable", 1);
  next.output_width = 1920;
  next.output_height = 1080;
  assert(mag_player_settings_prepare() == 1);
  assert(!mag_player_input_blocked());
  bool exists = false;
  auto stored = mag::load_player_settings(path, exists);
  assert(exists && stored.output_width == 1920);
  mag_player_settings_attach();
  assert(attached == 1);
  next = stored;
  next.render_scale = 300;
  next.output_width = 2560;
  next.output_height = 1440;
  mag_desktop_request_settings();
  confirm = false;
  mag_player_settings_poll();
  stored = mag::load_player_settings(path, exists);
  assert(stored.output_width == 1920 && displays == 2 &&
         !mag_player_input_blocked());
  mag_desktop_request_settings();
  confirm = true;
  mag_player_settings_poll();
  stored = mag::load_player_settings(path, exists);
  assert(stored.output_width == 2560);
  auto snapshot =
      mag::load_player_settings(owned / "launch-settings.json", exists);
  assert(snapshot.output_width == 1920);
  assert(snapshot.render_scale == 200);
  assert(std::string(getenv("MAG_PLAYER_RENDER_SCALE")) == "200");
  assert(std::string(getenv("PS3RECOMP_RSX_ENGINE")) == "dispatch");
  assert(std::string(getenv("PS3RECOMP_VK_GUEST_PROGRAMS")) == "1");
  unsetenv("MAG_PLAYER_SETUP");
  assert(mag_player_settings_prepare() == 1); // Engine uses immutable snapshot.
  mag_desktop_request_settings();
  edit = false;
  mag_player_settings_poll();
  assert(!mag_player_input_blocked());
  next = stored;
  next.render_scale = 200;
  next.anti_aliasing = "fxaa";
  mag::save_player_settings(path, next);
  setenv("MAG_PLAYER_SETUP", "1", 1);
  edit = false;
  assert(
      mag_player_settings_prepare() ==
      0); // Unsupported saved capability opens review, Cancel does not start.
  assert(!errors);
  return 0;
}
