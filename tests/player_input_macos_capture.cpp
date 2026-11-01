#include "player_input_capture.h"
#include "player_settings_runtime.h"
#include "libs/input/pad_macos.h"
#include <SDL2/SDL.h>
#include <iostream>
#include <stdexcept>
extern "C" {
PS3RecompMacOSPadState mag_sdk_pad_macos_state() {return {7,128,128,128,128};}
uint16_t mag_sdk_pad_macos_buttons() {return 7;}
Uint8 SDL_GameControllerGetButton(SDL_GameController*,SDL_GameControllerButton) {return 1;}
Sint16 SDL_GameControllerGetAxis(SDL_GameController*,SDL_GameControllerAxis axis) {return axis==SDL_CONTROLLER_AXIS_RIGHTX?16384:0;}
void SDL_GameControllerUpdate() {}
SDL_GameController* SDL_GameControllerOpen(int) {static int controller;return reinterpret_cast<SDL_GameController*>(&controller);}
void SDL_GameControllerClose(SDL_GameController*) {}
SDL_bool SDL_GameControllerGetAttached(SDL_GameController*) {return SDL_TRUE;}
Uint8 mag_player_controller_button(SDL_GameController*,SDL_GameControllerButton);
Sint16 mag_player_controller_axis(SDL_GameController*,SDL_GameControllerAxis);
void mag_player_controller_update(void);
SDL_GameController* mag_player_controller_open(int);
SDL_bool mag_player_controller_attached(SDL_GameController*);
}
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() {try {
    auto* controller=mag_player_controller_open(0);
    for(int blocked:{0,1}) {
        mag_player_set_input_blocked(blocked);
        check(mag_input_capture_poll_begin(uint64_t(blocked+1)),"native poll starts");
        mag_player_controller_update();check(mag_player_controller_attached(controller)==SDL_TRUE,"connection state passes through unchanged");
        for(int axis=0;axis<6;++axis) {
            const auto value=mag_player_controller_axis(controller,SDL_GameControllerAxis(axis));
            check(value==(blocked?0:(axis==SDL_CONTROLLER_AXIS_RIGHTX?16384:0)),"native SDK axis return preserves settings ownership");
        }
        check(mag_player_controller_button(controller,SDL_CONTROLLER_BUTTON_A)==(blocked?0:1),"native SDK button return preserves settings ownership");
        MagInputControllerSample sample{};
        check(mag_input_capture_poll_complete(uint64_t(blocked+1),&sample),"SDK wrapper supplies the full pre-quantized hardware sample");
        check(sample.look_x==16384./32767 && sample.buttons==1,"raw capture precedes settings input masking");
    }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
