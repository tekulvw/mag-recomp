#include "player_settings_desktop.h"
#include "player_settings_runtime.h"
#include <SDL2/SDL.h>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <dlfcn.h>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
namespace {
SDL_Window *game;
// Setup starts on the boot thread; in-game editing starts on the backend
// thread. Keep every GTK operation on one process-owned desktop thread.
struct GtkDispatcher {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::function<void()>> jobs;
  std::thread::id owner;
  GtkDispatcher() {
    std::thread([this] {
      {
        std::lock_guard lock(mutex);
        owner = std::this_thread::get_id();
      }
      ready.notify_all();
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [this] { return !jobs.empty(); });
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        job();
      }
    }).detach();
    std::unique_lock lock(mutex);
    ready.wait(lock, [this] { return owner != std::thread::id{}; });
  }
};
GtkDispatcher *gtkDispatcher() {
  // The dispatcher and GTK library live until process exit. They must not be
  // joined while an OS termination path closes a still-visible modal dialog.
  static auto *dispatcher = new GtkDispatcher;
  return dispatcher;
}
template <class F> auto onGtk(F function) -> std::invoke_result_t<F> {
  auto *dispatcher = gtkDispatcher();
  if (std::this_thread::get_id() == dispatcher->owner)
    return function();
  using Result = std::invoke_result_t<F>;
  auto task =
      std::make_shared<std::packaged_task<Result()>>(std::move(function));
  auto result = task->get_future();
  {
    std::lock_guard lock(dispatcher->mutex);
    dispatcher->jobs.emplace_back([task] { (*task)(); });
  }
  dispatcher->ready.notify_one();
  return result.get();
}
void *gtk;
bool gtkInitialized;
template <class T> T symbol(const char *name) {
  auto p = dlsym(gtk, name);
  if (!p)
    throw std::runtime_error(std::string("Missing GTK3 entry point: ") + name);
  return reinterpret_cast<T>(p);
}
#define GTK(name, type) symbol<type>(#name)
void initialize() {
  if (gtkInitialized)
    return;
  if (!gtk)
    gtk = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!gtk)
    throw std::runtime_error(
        "MAG settings require GTK3. Install your distribution's GTK3 desktop "
        "runtime (libgtk-3-0), then retry.");
  if (!GTK(gtk_init_check, int (*)(int *, char ***))(nullptr, nullptr))
    throw std::runtime_error("GTK3 could not connect to the desktop display.");
  gtkInitialized = true;
}
void *entry(void *box, const char *label, const std::string &value) {
  auto text = GTK(gtk_label_new, void *(*)(const char *))(label);
  GTK(gtk_box_pack_start,
      void (*)(void *, void *, int, int, unsigned))(box, text, 0, 0, 3);
  auto widget = GTK(gtk_entry_new, void *(*)())();
  GTK(gtk_entry_set_text, void (*)(void *, const char *))(widget,
                                                          value.c_str());
  GTK(gtk_box_pack_start,
      void (*)(void *, void *, int, int, unsigned))(box, widget, 0, 0, 3);
  return widget;
}
std::string text(void *widget) {
  return GTK(gtk_entry_get_text, const char *(*)(void *))(widget);
}
void label(void *box, const char *value) {
  auto widget = GTK(gtk_label_new, void *(*)(const char *))(value);
  GTK(gtk_label_set_line_wrap, void (*)(void *, int))(widget, 1);
  GTK(gtk_box_pack_start,
      void (*)(void *, void *, int, int, unsigned))(box, widget, 0, 0, 5);
}
void *choice(void *box, const char *title, const char *const *values, int count,
             int index) {
  label(box, title);
  auto widget = GTK(gtk_combo_box_text_new, void *(*)())();
  for (int i = 0; i < count; i++)
    GTK(gtk_combo_box_text_append_text,
        void (*)(void *, const char *))(widget, values[i]);
  GTK(gtk_combo_box_set_active, void (*)(void *, int))(widget, index);
  GTK(gtk_box_pack_start,
      void (*)(void *, void *, int, int, unsigned))(box, widget, 0, 0, 3);
  return widget;
}
int index(void *widget) {
  return GTK(gtk_combo_box_get_active, int (*)(void *))(widget);
}
int number(const std::string &s) {
  if (s.empty())
    return 0;
  for (char c : s)
    if (c < '0' || c > '9')
      throw std::runtime_error("Ports must contain decimal digits.");
  size_t used = 0;
  int n = std::stoi(s, &used);
  if (used != s.size())
    throw std::runtime_error("Ports must contain decimal digits.");
  return n;
}
} // namespace
static void errorOnGtk(const char *message) {
  std::fprintf(stderr, "[MAG settings] %s\n", message);
  try {
    initialize();
    auto dialog = GTK(gtk_message_dialog_new,
                      void *(*)(void *, int, int, int, const char *, ...))(
        nullptr, 1, 3, 1, "%s", message);
    GTK(gtk_dialog_run, int (*)(void *))(dialog);
    GTK(gtk_widget_destroy, void (*)(void *))(dialog);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[MAG settings] %s\n", e.what());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MAG Settings", message,
                             game);
  }
}
static bool editOnGtk(mag::PlayerSettings &settings, bool setup,
                      const char *error) {
  initialize();
  auto dialog = GTK(gtk_dialog_new_with_buttons,
                    void *(*)(const char *, void *, int, const char *, ...))(
      "MAG Settings", nullptr, 1, "Cancel", -6,
      setup ? "Save and Play" : "Apply", -3, nullptr);
  GTK(gtk_window_set_default_size, void (*)(void *, int, int))(dialog, 440,
                                                               560);
  auto box = GTK(gtk_dialog_get_content_area, void *(*)(void *))(dialog);
  if (error && *error)
    label(box, error);
  auto server = entry(box, "Community server (blank uses original)",
                      settings.server_address);
  auto tcp = entry(box, "TCP port (blank uses game default)",
                   settings.tcp_port ? std::to_string(settings.tcp_port) : "");
  auto udp = entry(box, "UDP port (blank uses game default)",
                   settings.udp_port ? std::to_string(settings.udp_port) : "");
  const char *modes[] = {"Windowed", "Borderless fullscreen"};
  auto mode = choice(box, "Display mode", modes, 2,
                     settings.display_mode == "borderless");
  const char *sizes[] = {"1280 x 720", "1920 x 1080", "2560 x 1440",
                         "3840 x 2160"};
  int current = settings.output_width == 1920   ? 1
                : settings.output_width == 2560 ? 2
                : settings.output_width == 3840 ? 3
                                                : 0;
  auto size = choice(box, "Output window size", sizes, 4, current);
  const char *scales[] = {"Native (100%)", "1080p (150%)", "1440p (200%)",
                          "2160p (300%)"};
  auto rendering = choice(box, "Rendering resolution (next launch)", scales, 4,
                          settings.render_scale == 150   ? 1
                          : settings.render_scale == 200 ? 2
                          : settings.render_scale == 300 ? 3
                                                         : 0);
#ifdef MAG_PLAYER_VULKAN_FXAA
  const char *aaModes[] = {"Off", "FXAA"};
  auto aa = choice(box, "Anti-aliasing (next launch)", aaModes, 2,
                   settings.anti_aliasing == "fxaa");
#else
  label(box, "Anti-aliasing: Off\nThis pinned Vulkan renderer does not "
             "implement FXAA.");
#endif
  label(box, "F10 opens settings while playing. Connection, rendering and AA "
             "changes apply next "
             "launch.\nDisplay changes require confirmation; closing the "
             "prompt reverts them.");
  GTK(gtk_widget_show_all, void (*)(void *))(dialog);
  bool applied = false;
  while (GTK(gtk_dialog_run, int (*)(void *))(dialog) == -3) {
    try {
      auto draft = settings;
      draft.server_address = text(server);
      draft.tcp_port = number(text(tcp));
      draft.udp_port = number(text(udp));
      draft.display_mode = index(mode) == 1 ? "borderless" : "windowed";
      const int widths[] = {1280, 1920, 2560, 3840},
                heights[] = {720, 1080, 1440, 2160};
      auto n = index(size);
      if (n < 0 || n > 3)
        throw std::runtime_error("Choose an output size.");
      draft.output_width = widths[n];
      draft.output_height = heights[n];
      const int percentages[] = {100, 150, 200, 300};
      int scale = index(rendering);
      if (scale < 0 || scale > 3)
        throw std::runtime_error("Choose a rendering resolution.");
      draft.render_scale = percentages[scale];
#ifdef MAG_PLAYER_VULKAN_FXAA
      draft.anti_aliasing = index(aa) == 1 ? "fxaa" : "off";
#else
      draft.anti_aliasing = "off";
#endif
      settings = mag::validate_player_settings(draft);
      applied = true;
      break;
    } catch (const std::exception &e) {
      mag_desktop_error(e.what());
    }
  }
  GTK(gtk_widget_destroy, void (*)(void *))(dialog);
  return applied;
}
static bool confirmOnGtk() {
  auto dialog = GTK(gtk_dialog_new_with_buttons,
                    void *(*)(const char *, void *, int, const char *, ...))(
      "Keep MAG display settings?", nullptr, 1, "Revert", -6, "Keep", -3,
      nullptr);
  auto box = GTK(gtk_dialog_get_content_area, void *(*)(void *))(dialog);
  label(box, "Keep this display mode? Previous settings return in 15 seconds.");
  GTK(gtk_widget_show_all, void (*)(void *))(dialog);
  auto timeout = +[](void *value) -> int {
    GTK(gtk_dialog_response, void (*)(void *, int))(value, -6);
    return 0;
  };
  unsigned timer =
      GTK(g_timeout_add_seconds,
          unsigned (*)(unsigned, int (*)(void *), void *))(15, timeout, dialog);
  int response = GTK(gtk_dialog_run, int (*)(void *))(dialog);
  // The callback removes itself when it fires. Remove only a still-live source.
  if (GTK(g_main_context_find_source_by_id,
          void *(*)(void *, unsigned))(nullptr, timer))
    GTK(g_source_remove, int (*)(unsigned))(timer);
  GTK(gtk_widget_destroy, void (*)(void *))(dialog);
  return response == -3;
}
void mag_desktop_error(const char *message) {
  onGtk([&] { errorOnGtk(message); });
}
bool mag_desktop_edit(mag::PlayerSettings &settings, bool setup,
                      const char *error) {
  return onGtk([&] { return editOnGtk(settings, setup, error); });
}
bool mag_desktop_confirm_display() {
  return onGtk([] { return confirmOnGtk(); });
}
void mag_desktop_attach(const mag::PlayerSettings &settings) {
  if (!game)
    throw std::runtime_error("The Vulkan game window is unavailable.");
  mag_desktop_apply_display(settings);
}
void mag_desktop_apply_display(const mag::PlayerSettings &settings) {
  if (!game)
    return;
  if (SDL_SetWindowFullscreen(game, settings.display_mode == "borderless"
                                        ? SDL_WINDOW_FULLSCREEN_DESKTOP
                                        : 0))
    throw std::runtime_error(SDL_GetError());
  if (settings.display_mode == "windowed") {
    SDL_SetWindowSize(game, settings.output_width, settings.output_height);
    SDL_SetWindowPosition(game, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}
// Source-scoped SDK aliases capture only the Vulkan-owned window and its F10.
extern "C" SDL_Window *mag_player_create_window(const char *title, int x, int y,
                                                int w, int h, Uint32 flags) {
  game = SDL_CreateWindow(title, x, y, w, h, flags);
  return game;
}
extern "C" int mag_player_poll_event(SDL_Event *event) {
  while (SDL_PollEvent(event)) {
    if (event && event->type == SDL_KEYDOWN &&
        event->key.keysym.sym == SDLK_F10) {
      if (!event->key.repeat)
        mag_desktop_request_settings();
      continue;
    }
    return 1;
  }
  return 0;
}
extern "C" Uint8 mag_player_controller_button(SDL_GameController *c,
                                              SDL_GameControllerButton b) {
  return mag_player_input_blocked() ? 0 : SDL_GameControllerGetButton(c, b);
}
extern "C" Sint16 mag_player_controller_axis(SDL_GameController *c,
                                             SDL_GameControllerAxis a) {
  return mag_player_input_blocked() ? 0 : SDL_GameControllerGetAxis(c, a);
}
