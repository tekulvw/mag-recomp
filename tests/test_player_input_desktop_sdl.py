"""SDL desktop capture backend (Linux game window) and its SDK keyboard seam; synthetic input only.

The SDL fixture runs wherever SDL2 development files are installed (sdl2-config), with the dummy
video driver; it skips cleanly otherwise. The seam fixture needs only a C/C++ compiler."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FAKE_PAD_HEADER = '''#include <stdint.h>
#define PAD_HAS_SDL_KEYBOARD 1
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint16_t buttons;uint8_t analog_lx,analog_ly,analog_rx,analog_ry;uint8_t window;} PS3RecompSDLKeyboardPadState;
void ps3recomp_pad_sdl_keyboard_sample(void* sdl_window, int suppressed);
PS3RecompSDLKeyboardPadState ps3recomp_pad_sdl_keyboard_state(void);
#ifdef __cplusplus
}
#endif
'''

SEAM_FIXTURE = r'''#include "libs/input/pad_sdl_keyboard.h"
#include <stdexcept>
#include <string>
#include <iostream>
static std::string calls;static int owned=0;
static PS3RecompSDLKeyboardPadState sdk={0x0101,0,255,10,20,1};
extern "C" {
int mag_input_capture_keyboard_owned(void){return owned;}
void mag_input_desktop_sdl_pump(void* window,int dialog){calls+="pump("+std::to_string(window!=nullptr)+","+std::to_string(dialog)+")";}
void mag_sdk_pad_sdl_keyboard_sample(void* window,int suppressed){calls+="sdk("+std::to_string(window!=nullptr)+","+std::to_string(suppressed)+")";}
PS3RecompSDLKeyboardPadState mag_sdk_pad_sdl_keyboard_state(void){return sdk;}
}
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    int window=0;ps3recomp_pad_sdl_keyboard_sample(&window,1);ps3recomp_pad_sdl_keyboard_sample(nullptr,0);
    require(calls=="pump(1,1)sdk(1,1)pump(0,0)sdk(0,0)","the capture pump runs first on every sample, then the SDK's own");
    auto state=ps3recomp_pad_sdl_keyboard_state();
    require(state.buttons==0x0101 && state.analog_lx==0 && state.analog_ly==255 && state.window==1,"without ownership the SDK keyboard pad is unchanged");
    owned=1;state=ps3recomp_pad_sdl_keyboard_state();
    require(state.buttons==0 && state.analog_lx==128 && state.analog_ly==128 && state.analog_rx==128 && state.analog_ry==128,"ownership neutralizes the keyboard pad");
    require(state.window==1,"the window keeps controller port 0 as before");
    sdk.window=0;require(ps3recomp_pad_sdl_keyboard_state().window==0,"no window, no port");
    return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''


def sdl_flags():
    config = shutil.which('sdl2-config')
    if not config:
        return None
    prefix = subprocess.run([config, '--prefix'], capture_output=True, text=True, check=True).stdout.strip()
    cflags = shlex.split(subprocess.run([config, '--cflags'], capture_output=True, text=True, check=True).stdout)
    libs = shlex.split(subprocess.run([config, '--libs'], capture_output=True, text=True, check=True).stdout)
    # The sources include <SDL2/SDL.h>; sdl2-config names the SDL2 directory itself.
    libs = [flag for flag in libs if flag != '-lSDL2main']
    return [f'-I{prefix}/include', *cflags], libs


class SdlKeyboardSeamTests(unittest.TestCase):
    def test_pump_runs_before_the_sdk_sample_and_ownership_neutralizes_the_keyboard_pad(self):
        cc = shutil.which('clang') or shutil.which('gcc')
        cxx = shutil.which('clang++') or shutil.which('g++')
        if not cc or not cxx:
            self.skipTest('C and C++ compilers required')
        with tempfile.TemporaryDirectory(prefix='mag-sdl-seam-') as tmp:
            root = Path(tmp)
            (root / 'libs/input').mkdir(parents=True)
            (root / 'libs/input/pad_sdl_keyboard.h').write_text(FAKE_PAD_HEADER)
            (root / 'fixture.cpp').write_text(SEAM_FIXTURE)
            seam = root / 'seam.o'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(root), '-I' + str(ROOT / 'src'), '-c',
                            str(ROOT / 'src/player_input_sdl_keyboard.c'), '-o', str(seam)], check=True)
            binary = root / 'seam'
            subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(root), str(root / 'fixture.cpp'),
                            str(seam), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


class SdlDesktopCaptureTests(unittest.TestCase):
    def test_sdl_events_reach_the_core_and_escape_is_withheld_only_while_owned(self):
        flags = sdl_flags()
        cxx = shutil.which('clang++') or shutil.which('g++')
        if not flags or not cxx:
            self.skipTest('SDL2 development files (sdl2-config) and a C++20 compiler required')
        cflags, libs = flags
        with tempfile.TemporaryDirectory(prefix='mag-sdl-desktop-') as tmp:
            binary = Path(tmp) / 'desktop'
            subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(ROOT / 'src'), *cflags,
                            str(ROOT / 'tests/player_input_desktop_sdl.cpp'), str(ROOT / 'src/player_input_desktop_sdl.cpp'),
                            str(ROOT / 'src/player_desktop_capture_core.cpp'), *libs, '-o', str(binary)], check=True)
            env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy'}
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)
            if result.returncode == 77:
                self.skipTest(result.stderr.strip())
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
