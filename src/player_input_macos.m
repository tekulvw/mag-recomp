#include "player_settings_runtime.h"
#include "player_input_capture.h"
#include "libs/input/pad_macos.h"
#include <SDL2/SDL.h>
#include <stdatomic.h>

static atomic_int blocked;
int mag_player_input_blocked(void) {return atomic_load_explicit(&blocked,memory_order_acquire);}
void mag_player_set_input_blocked(int value) {atomic_store_explicit(&blocked,!!value,memory_order_release);}

PS3RecompMacOSPadState mag_sdk_pad_macos_state(void);
uint16_t mag_sdk_pad_macos_buttons(void);
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void) {
    if(mag_player_input_blocked())return (PS3RecompMacOSPadState){0,128,128,128,128};
    return mag_sdk_pad_macos_state();
}
uint16_t ps3recomp_pad_macos_buttons(void) {
    return mag_player_input_blocked()?0:mag_sdk_pad_macos_buttons();
}
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
SDL_GameController* mag_player_controller_open(int port) {
    SDL_GameController* controller=SDL_GameControllerOpen(port);
    if(port>=0)mag_input_capture_controller_open(controller,(unsigned)port);
    return controller;
}
void mag_player_controller_close(SDL_GameController* controller) {
    mag_input_capture_controller_close(controller);SDL_GameControllerClose(controller);
}
SDL_bool mag_player_controller_attached(SDL_GameController* controller) {
    SDL_bool attached=SDL_GameControllerGetAttached(controller);
    mag_input_capture_controller_attached(controller,attached==SDL_TRUE);return attached;
}
