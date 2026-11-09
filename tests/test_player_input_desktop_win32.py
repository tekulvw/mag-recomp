"""Windows desktop capture backend: its portable translations, its SDK seams and (where a MinGW-w64
cross compiler is installed) a compile of the Win32 code itself. Synthetic input only.

The translations and the seams build on any host with a C/C++ compiler: the seams against
synthetic stand-ins for the system and SDK headers. The Win32 compile skips cleanly without
x86_64-w64-mingw32-g++; it checks that the backend builds, not how it behaves."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'

FAKE_WINDOWS = '''#pragma once
#include <stdint.h>
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int16_t SHORT;
#define WINAPI
#define ERROR_SUCCESS 0u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#ifdef __cplusplus
extern "C" {
#endif
SHORT WINAPI GetAsyncKeyState(int key);
#ifdef __cplusplus
}
#endif
'''

FAKE_XINPUT = '''#pragma once
#include <windows.h>
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
typedef struct {WORD wButtons;BYTE bLeftTrigger,bRightTrigger;SHORT sThumbLX,sThumbLY,sThumbRX,sThumbRY;} XINPUT_GAMEPAD;
typedef struct {DWORD dwPacketNumber;XINPUT_GAMEPAD Gamepad;} XINPUT_STATE;
#ifdef __cplusplus
extern "C" {
#endif
DWORD WINAPI XInputGetState(DWORD port, XINPUT_STATE* state);
#ifdef __cplusplus
}
#endif
'''

FAKE_OSK = '''#pragma once
#define OSK_NATIVE_HAS_WIN32_HOST 1
#ifdef __cplusplus
extern "C" {
#endif
void osk_native_win32_set_window(void* window);
void osk_native_win32_pump(void);
int osk_native_win32_handle_message(void* message);
int osk_native_win32_input_active(void);
#ifdef __cplusplus
}
#endif
'''

STRICT = ['-Wall', '-Wextra', '-Werror']


def compilers():
    cc = shutil.which('clang') or shutil.which('gcc')
    cxx = shutil.which('clang++') or shutil.which('g++')
    return cc, cxx


class Win32TranslationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc, cxx = compilers()
        if not cc or not cxx:
            raise unittest.SkipTest('C and C++ compilers required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-win32-map-')
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        subprocess.run([cc, '-std=c11', *STRICT, '-I' + str(SRC), '-c', str(SRC / 'player_input_win32_map.c'),
                        '-o', str(root / 'map.o')], check=True)
        cls.binary = root / 'map'
        subprocess.run([cxx, '-std=c++20', *STRICT, '-I' + str(SRC), str(ROOT / 'tests/player_input_win32_map.cpp'),
                        str(root / 'map.o'), '-o', str(cls.binary)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True)

    def test_scan_codes_translate_to_hid_usages(self):
        self.run_case('keys')

    def test_auto_repeat_is_not_a_new_press(self):
        self.run_case('repeats')

    def test_raw_mouse_buttons_and_relative_motion(self):
        self.run_case('mouse')

    def test_xinput_readings_take_sdl_numbering_and_ranges(self):
        self.run_case('xinput')

    def test_keyboard_fallback_is_silent_while_owned(self):
        self.run_case('fallback')

    def test_unfocused_presses_and_motion_are_ignored_releases_pass(self):
        self.run_case('focus')


class Win32SeamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc, cxx = compilers()
        if not cc or not cxx:
            raise unittest.SkipTest('C and C++ compilers required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-win32-seams-')
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        (root / 'libs/misc').mkdir(parents=True)
        (root / 'windows.h').write_text(FAKE_WINDOWS)
        (root / 'xinput.h').write_text(FAKE_XINPUT)
        (root / 'libs/misc/osk_native.h').write_text(FAKE_OSK)
        includes = ['-I' + str(root), '-I' + str(SRC)]
        objects = []
        for name in ('player_input_win32_pad.c', 'player_input_win32_window.c', 'player_input_win32_map.c'):
            obj = root / (name + '.o')
            subprocess.run([cc, '-std=c11', *STRICT, '-D_WIN32', *includes, '-c', str(SRC / name), '-o', str(obj)],
                           check=True)
            objects.append(str(obj))
        cls.binary = root / 'seams'
        subprocess.run([cxx, '-std=c++20', *STRICT, *includes, str(ROOT / 'tests/player_input_win32_seams.cpp'),
                        *objects, '-o', str(cls.binary)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True)

    def test_xinput_readings_reach_the_capture_layer_and_blocks_mask_the_sdk(self):
        self.run_case('xinput')

    def test_keyboard_fallback_follows_ownership_regardless_of_xinput(self):
        self.run_case('keyboard')

    def test_window_pump_calls_reach_the_backend_then_the_on_screen_keyboard(self):
        self.run_case('window')


class Win32CrossCompileTests(unittest.TestCase):
    def test_win32_backend_and_seams_compile_for_windows(self):
        gcc = shutil.which('x86_64-w64-mingw32-gcc')
        gxx = shutil.which('x86_64-w64-mingw32-g++')
        if not gcc or not gxx:
            self.skipTest('MinGW-w64 cross compiler (x86_64-w64-mingw32-g++) not installed')
        with tempfile.TemporaryDirectory(prefix='mag-win32-cross-') as tmp:
            root = Path(tmp)
            # The real Windows headers; only the SDK's on-screen keyboard header is a stand-in.
            (root / 'libs/misc').mkdir(parents=True)
            (root / 'libs/misc/osk_native.h').write_text(FAKE_OSK)
            includes = ['-I' + str(root), '-I' + str(SRC)]
            for name in ('player_input_desktop_win32.cpp', 'player_desktop_capture_core.cpp'):
                subprocess.run([gxx, '-std=c++20', *STRICT, *includes, '-c', str(SRC / name),
                                '-o', str(root / (name + '.o'))], check=True)
            for name in ('player_input_win32_pad.c', 'player_input_win32_window.c', 'player_input_win32_map.c'):
                subprocess.run([gcc, '-std=c11', *STRICT, *includes, '-c', str(SRC / name),
                                '-o', str(root / (name + '.o'))], check=True)


if __name__ == '__main__':
    unittest.main()
