// SDL's event subsystem only: no game, window, GTK initialization or display.
#include "player_settings_runtime.h"
#include <SDL2/SDL.h>
#include <cassert>
namespace {
int blocked;
bool requested;
} // namespace
extern "C" int mag_player_input_blocked() { return blocked; }
extern "C" void mag_player_set_input_blocked(int value) { blocked = value; }
void mag_desktop_request_settings() { requested = true; }
bool mag_desktop_settings_requested() { return requested; }
extern "C" int mag_player_poll_event(SDL_Event *event);
int main() {
  assert(SDL_Init(SDL_INIT_EVENTS) == 0);
  SDL_Event key{};
  key.type = SDL_KEYDOWN;
  key.key.keysym.sym = SDLK_ESCAPE;
  blocked = 1;
  assert(SDL_PushEvent(&key) == 1);
  SDL_Event close{};
  close.type = SDL_QUIT;
  assert(SDL_PushEvent(&close) == 1);
  SDL_Event event{};
  bool closed = false;
  for (int frame = 0; frame < 2; ++frame)
    while (mag_player_poll_event(&event)) {
      assert(event.type != SDL_KEYDOWN);
      if (event.type == SDL_QUIT)
        closed = true;
    }
  assert(closed && !requested);
  key.key.keysym.sym = SDLK_F10;
  assert(SDL_PushEvent(&key) == 1);
  for (int frame = 0; frame < 2; ++frame)
    while (mag_player_poll_event(&event)) {
    }
  assert(!requested);
  blocked = 0;
  key.key.keysym.sym = SDLK_ESCAPE;
  assert(SDL_PushEvent(&key) == 1);
  bool escaped = false;
  for (int frame = 0; frame < 2; ++frame)
    while (mag_player_poll_event(&event))
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
        escaped = true;
  assert(escaped && !requested);
  key.key.keysym.sym = SDLK_F10;
  assert(SDL_PushEvent(&key) == 1);
  // SDL may end the previous pump cycle at its internal poll sentinel. The
  // next frame's drain must process the newly queued shortcut.
  for (int frame = 0; frame < 2; ++frame)
    while (mag_player_poll_event(&event)) {
    }
  assert(requested);
  SDL_Quit();
}
