from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys
import os
ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path(os.environ.get('MAG_INPUT_CAPTURE_TEST_SOURCE',ROOT)).resolve()
class PlayerInputCaptureTests(unittest.TestCase):
    def test_actual_poll_owns_complete_raw_controller_sample(self):
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:self.skipTest('C++20 compiler required')
        with tempfile.TemporaryDirectory(prefix='mag-input-capture-') as tmp:
            binary=Path(tmp)/'capture'
            subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(SOURCE/'src'),
                            str(ROOT/'tests/player_input_capture.cpp'),str(SOURCE/'src/player_input_capture.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    @unittest.skipUnless(sys.platform=='darwin','macOS wrapper fixture')
    def test_macos_sdk_wrappers_capture_before_masking_without_a_window(self):
        with tempfile.TemporaryDirectory(prefix='mag-macos-input-') as tmp:
            root=Path(tmp);(root/'SDL2').mkdir();(root/'libs/input').mkdir(parents=True)
            (root/'libs/input/pad_macos.h').write_text('''#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint16_t buttons;uint8_t analog_lx,analog_ly,analog_rx,analog_ry;} PS3RecompMacOSPadState;
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void);
uint16_t ps3recomp_pad_macos_buttons(void);
#ifdef __cplusplus
}
#endif
''')
            (root/'SDL2/SDL.h').write_text('''#include <stdint.h>
typedef uint8_t Uint8;typedef int16_t Sint16;typedef struct SDL_GameController SDL_GameController;
typedef enum {SDL_CONTROLLER_BUTTON_A} SDL_GameControllerButton;
typedef enum {SDL_CONTROLLER_AXIS_LEFTX,SDL_CONTROLLER_AXIS_LEFTY,SDL_CONTROLLER_AXIS_RIGHTX,SDL_CONTROLLER_AXIS_RIGHTY,SDL_CONTROLLER_AXIS_TRIGGERLEFT,SDL_CONTROLLER_AXIS_TRIGGERRIGHT} SDL_GameControllerAxis;
typedef enum {SDL_FALSE,SDL_TRUE} SDL_bool;
#ifdef __cplusplus
extern "C" {
#endif
Uint8 SDL_GameControllerGetButton(SDL_GameController*,SDL_GameControllerButton);
Sint16 SDL_GameControllerGetAxis(SDL_GameController*,SDL_GameControllerAxis);
void SDL_GameControllerUpdate(void);
SDL_GameController* SDL_GameControllerOpen(int);
void SDL_GameControllerClose(SDL_GameController*);
SDL_bool SDL_GameControllerGetAttached(SDL_GameController*);
#ifdef __cplusplus
}
#endif
''')
            wrapper=root/'wrapper.o';binary=root/'capture'
            subprocess.run(['xcrun','clang','-fobjc-arc','-Wall','-Wextra','-Werror','-I'+str(root),'-I'+str(SOURCE/'src'),'-c',str(SOURCE/'src/player_input_macos.m'),'-o',str(wrapper)],check=True)
            subprocess.run(['xcrun','clang++','-std=c++20','-Wall','-Wextra','-Werror','-I'+str(root),'-I'+str(SOURCE/'src'),str(wrapper),
                            str(SOURCE/'src/player_input_capture.cpp'),str(ROOT/'tests/player_input_macos_capture.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()
