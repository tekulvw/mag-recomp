/* SDL GameController seam shared by the macOS, Linux and Windows players. cellPad's SDL backend
 * calls these in place of the SDL functions (compile definitions on libs/input/cellPad.c): every
 * raw reading reaches the capture layer (player_input_capture.h) before the settings panel's input
 * block masks what the SDK sees. Nothing changes for the SDK unless input is blocked.
 *
 * cellPad opens a controller by SDL device index and then gives it its port as the player index;
 * the capture layer keys controllers by that port. Device indices shift when a controller is
 * unplugged, so a controller registers with the capture layer at its player index, not at open. */
#include "player_settings_runtime.h"
#include "player_input_capture.h"
#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>  /* the official Windows development package's flat include/ */
#endif
#else
#include <SDL2/SDL.h>
#endif
#include <stdatomic.h>

static atomic_int blocked;
int mag_player_input_blocked(void) {return atomic_load_explicit(&blocked,memory_order_acquire);}
void mag_player_set_input_blocked(int value) {atomic_store_explicit(&blocked,!!value,memory_order_release);}

Uint8 mag_player_controller_button(SDL_GameController *controller,SDL_GameControllerButton button) {
    Uint8 raw=SDL_GameControllerGetButton(controller,button);
    mag_input_capture_controller_button(controller,(unsigned)button,raw!=0);
    return mag_player_input_blocked()?0:raw;
}
Sint16 mag_player_controller_axis(SDL_GameController *controller,SDL_GameControllerAxis axis) {
    Sint16 raw=SDL_GameControllerGetAxis(controller,axis);
    mag_input_capture_controller_axis(controller,(unsigned)axis,raw);
    return mag_player_input_blocked()?0:raw;
}
void mag_player_controller_update(void) {mag_input_capture_backend_begin();SDL_GameControllerUpdate();}
SDL_GameController* mag_player_controller_open(int device) {return SDL_GameControllerOpen(device);}
void mag_player_controller_set_player_index(SDL_GameController* controller,int port) {
    SDL_GameControllerSetPlayerIndex(controller,port);
    if(controller && port>=0)mag_input_capture_controller_open(controller,(unsigned)port);
}
void mag_player_controller_close(SDL_GameController* controller) {
    mag_input_capture_controller_close(controller);SDL_GameControllerClose(controller);
}
SDL_bool mag_player_controller_attached(SDL_GameController* controller) {
    SDL_bool attached=SDL_GameControllerGetAttached(controller);
    mag_input_capture_controller_attached(controller,attached==SDL_TRUE);return attached;
}
