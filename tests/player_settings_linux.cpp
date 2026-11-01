// Real GTK3 and SDL2 synthetic window test, run under Xvfb on Linux.
#include "../src/player_settings_linux.cpp"
#include <atomic>
#include <cassert>
#include <iostream>
namespace {
std::atomic<int> blocked;
bool requested;
int response = -6;
} // namespace
extern "C" int mag_player_input_blocked() { return blocked.load(); }
extern "C" void mag_player_set_input_blocked(int value) { blocked = value; }
void mag_desktop_request_settings() { requested = true; }
bool mag_desktop_settings_requested() {
  bool result = requested;
  requested = false;
  return result;
}
static int respond(void *) {
  auto windows = GTK(gtk_window_list_toplevels, void *(*)())();
  struct List {
    void *data;
    List *next, *previous;
  };
  for (auto *item = static_cast<List *>(windows); item; item = item->next)
    if (GTK(g_type_check_instance_is_a, int (*)(void *, unsigned long))(
            item->data, GTK(gtk_dialog_get_type, unsigned long (*)())()))
      GTK(gtk_dialog_response, void (*)(void *, int))(item->data, response);
  GTK(g_list_free, void (*)(void *))(windows);
  return 0;
}
int main() {
  assert(SDL_Init(SDL_INIT_VIDEO) == 0);
  auto window =
      mag_player_create_window("Synthetic MAG settings", 0, 0, 1280, 720,
                               SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  assert(window);
  mag::PlayerSettings s;
  s.server_address = "";
  mag_desktop_attach(s);
  s.output_width = 1920;
  s.output_height = 1080;
  mag_desktop_apply_display(s);
  int w = 0, h = 0;
  SDL_GetWindowSize(window, &w, &h);
  assert(w == 1920 && h == 1080);
  onGtk([] { initialize(); });
  GTK(g_timeout_add,
      unsigned (*)(unsigned, int (*)(void *), void *))(50, respond, nullptr);
  auto before = s;
  assert(!mag_desktop_edit(s, true, ""));
  assert(s.output_width == before.output_width);
  response = -3;
  s.render_scale = 200;
#ifdef MAG_PLAYER_VULKAN_FXAA
  s.anti_aliasing = "fxaa";
#endif
  GTK(g_timeout_add,
      unsigned (*)(unsigned, int (*)(void *), void *))(50, respond, nullptr);
  assert(mag_desktop_edit(s, false, ""));
  assert(s.output_width == 1920 && s.render_scale == 200);
#ifdef MAG_PLAYER_VULKAN_FXAA
  assert(s.anti_aliasing == "fxaa");
#else
  assert(s.anti_aliasing == "off");
#endif
  response = -6;
  GTK(g_timeout_add,
      unsigned (*)(unsigned, int (*)(void *), void *))(50, respond, nullptr);
  assert(!mag_desktop_confirm_display());
  response = -3;
  GTK(g_timeout_add,
      unsigned (*)(unsigned, int (*)(void *), void *))(50, respond, nullptr);
  assert(mag_desktop_confirm_display());
  SDL_Event key{};
  key.type = SDL_KEYDOWN;
  key.key.keysym.sym = SDLK_F10;
  SDL_PushEvent(&key);
  SDL_Event event{};
  while (mag_player_poll_event(&event)) {
  }
  assert(mag_desktop_settings_requested());
  s.display_mode = "borderless";
  mag_desktop_apply_display(s);
  assert(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP);
  s.display_mode = "windowed";
  mag_desktop_apply_display(s);
  assert(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP));
  assert(
      !mag_desktop_confirm_display()); // No response: real 15-second timeout.
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::cout
      << "GTK3 Apply/Cancel, display mode/size, Keep/Revert, F10 passed\n";
}
