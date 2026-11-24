#include "player_input_capture.h"
#include "player_settings_runtime.h"
#include "libs/input/pad_macos.h"
#include <SDL2/SDL.h>
#include <iostream>
#include <stdexcept>
static PS3RecompMacOSPadState keyboard={0x1234,0,255,1,254};
extern "C" {
PS3RecompMacOSPadState mag_sdk_pad_macos_state() {return keyboard;}
uint16_t mag_sdk_pad_macos_buttons() {return keyboard.buttons;}
Uint8 SDL_GameControllerGetButton(SDL_GameController*,SDL_GameControllerButton) {return 1;}
Sint16 SDL_GameControllerGetAxis(SDL_GameController*,SDL_GameControllerAxis axis) {return axis==SDL_CONTROLLER_AXIS_RIGHTX?16384:0;}
void SDL_GameControllerUpdate() {}
SDL_GameController* SDL_GameControllerOpen(int) {static int controller;return reinterpret_cast<SDL_GameController*>(&controller);}
void SDL_GameControllerClose(SDL_GameController*) {}
SDL_bool SDL_GameControllerGetAttached(SDL_GameController*) {return SDL_TRUE;}
void SDL_GameControllerSetPlayerIndex(SDL_GameController*,int) {}
Uint8 mag_player_controller_button(SDL_GameController*,SDL_GameControllerButton);
Sint16 mag_player_controller_axis(SDL_GameController*,SDL_GameControllerAxis);
void mag_player_controller_update(void);
SDL_GameController* mag_player_controller_open(int);
void mag_player_controller_set_player_index(SDL_GameController*,int);
SDL_bool mag_player_controller_attached(SDL_GameController*);
}
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() {try {
    // cellPad opens by device index, then names the port; the capture layer follows the port.
    auto* controller=mag_player_controller_open(0);
    mag_player_controller_set_player_index(controller,0);
    uint64_t sequence=0;
    for(int owned:{0,1}) for(int blocked:{0,1}) {
        mag_input_capture_set_keyboard_owner(owned);
        check(mag_input_capture_keyboard_owned()==owned,"keyboard ownership is explicit and observable");
        mag_player_set_input_blocked(blocked);
        const auto legacy=ps3recomp_pad_macos_state();
        if(owned || blocked)check(legacy.buttons==0 && legacy.analog_lx==128 && legacy.analog_ly==128 && legacy.analog_rx==128 && legacy.analog_ry==128,"Rust ownership/settings suppress only the legacy keyboard snapshot");
        else check(legacy.buttons==keyboard.buttons && legacy.analog_lx==0 && legacy.analog_ly==255 && legacy.analog_rx==1 && legacy.analog_ry==254,"frontend keyboard snapshot remains available");
        check(ps3recomp_pad_macos_buttons()==(owned || blocked?0:keyboard.buttons),"legacy button-only fallback obeys ownership");
        check(mag_input_capture_poll_begin(++sequence),"native poll starts");
        mag_player_controller_update();check(mag_player_controller_attached(controller)==SDL_TRUE,"connection state passes through unchanged");
        for(int axis=0;axis<6;++axis) {
            const auto value=mag_player_controller_axis(controller,SDL_GameControllerAxis(axis));
            check(value==(blocked?0:(axis==SDL_CONTROLLER_AXIS_RIGHTX?16384:0)),"native SDK axis return preserves settings ownership");
        }
        check(mag_player_controller_button(controller,SDL_CONTROLLER_BUTTON_A)==(blocked?0:1),"native SDK button return preserves settings ownership");
        MagInputControllerSample sample{};
        check(mag_input_capture_poll_complete(sequence,&sample),"SDK wrapper supplies the full pre-quantized hardware sample");
        check(sample.look_x==16384./32767 && sample.buttons==1,"raw capture precedes settings input masking");
    }
    mag_player_set_input_blocked(0);keyboard={0,128,128,128,128};
    mag_input_capture_set_keyboard_owner(0);
    (void)ps3recomp_pad_macos_state();
    keyboard={0x1234,0,255,1,254};
    check(ps3recomp_pad_macos_state().buttons==keyboard.buttons && ps3recomp_pad_macos_buttons()==keyboard.buttons,"releasing ownership restores fresh frontend keyboard input");
    // An in-match menu: the keyboard stays owned but the pad is live; keys held from gameplay wait
    // for a release, fresh ones count.
    mag_input_capture_set_keyboard_owner(1);
    check(ps3recomp_pad_macos_state().buttons==0,"on foot the pad is muted");
    mag_input_capture_set_keyboard_pad_live(1);
    keyboard={0x0001,0,128,128,128};
    auto menu=ps3recomp_pad_macos_state();
    check(menu.buttons==0 && menu.analog_lx==128,"a key held from gameplay does not press in the menu");
    keyboard={0x0010,128,128,128,128};
    check(ps3recomp_pad_macos_state().buttons==0x0010,"a fresh press counts in the menu");
    keyboard={0x0011,128,128,128,128};
    check(ps3recomp_pad_macos_state().buttons==0x0011,"the earlier key counts after its release");
    mag_input_capture_set_keyboard_pad_live(0);
    check(ps3recomp_pad_macos_state().buttons==0,"back on foot the pad is muted again");
    mag_input_capture_set_keyboard_owner(0);
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
