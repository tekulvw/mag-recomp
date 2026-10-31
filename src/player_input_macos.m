#include "player_settings_runtime.h"
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
    return mag_player_input_blocked()?0:SDL_GameControllerGetButton(controller,button);
}
Sint16 mag_player_controller_axis(SDL_GameController *controller,SDL_GameControllerAxis axis) {
    return mag_player_input_blocked()?0:SDL_GameControllerGetAxis(controller,axis);
}
