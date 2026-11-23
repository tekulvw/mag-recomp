"""The Windows and Linux settings window: its form, the SDL2 panel and the Win32 window.

All synthetic. The form and the SDL2 panel run against synthetic SDL events and a software
renderer (no window, no display). The Win32 window is compiled where a MinGW-w64 cross compiler
or Windows is available, and driven with synthetic messages on Windows.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('MAG_TEST_SDK') or ROOT / 'local/ps3recomp-runtime')
FORM = ['src/player_settings_form.cpp', 'src/player_settings_portable.cpp', 'src/player_server_overlay_portable.cpp',
        'src/player_archive.cpp', 'src/player_inflate.cpp', 'src/player_sha256.cpp',
        'src/player_controls_settings.cpp', 'src/player_controls_store.cpp', 'src/player_controls_rust.cpp',
        'src/player_preferences_file.cpp']


def sdl_flags():
    tool = ['sdl2-config', '--cflags', '--libs'] if shutil.which('sdl2-config') else ['pkg-config', '--cflags', '--libs', 'sdl2']
    if not shutil.which(tool[0]):
        return None
    result = subprocess.run(tool, capture_output=True, text=True)
    if result.returncode:
        return None
    flags = shlex.split(result.stdout)
    # The sources include <SDL2/SDL.h>: also search the folder above SDL2's own.
    return flags + ['-I' + str(Path(f[2:]).parent) for f in flags if f.startswith('-I') and Path(f[2:]).name == 'SDL2']


@unittest.skipIf(os.name == 'nt', 'C++ and SDL2 fixture; the Win32 window has its own test below')
class SdlSettingsPanelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('clang++') or shutil.which('g++')
        cls.sdl = sdl_flags()
        if not cls.compiler or not shutil.which('cargo') or cls.sdl is None:
            raise unittest.SkipTest('C++20, SDL2 development files and the pinned Rust toolchain required')
        if not (SDK / 'libs/misc/osk_font8x8.h').is_file():
            raise unittest.SkipTest('set MAG_TEST_SDK to the pinned runtime (its bitmap font)')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-settings-window-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        env = {**os.environ, 'CARGO_TARGET_DIR': str(cls.root / 'cargo')}
        subprocess.run(['cargo', 'build', '--manifest-path', str(ROOT / 'crates/mag-input/Cargo.toml'), '--release',
                        '--locked', '--offline'], cwd=ROOT, env=env, check=True)
        (cls.root / 'sdata_decrypt.h').write_text('#include <stddef.h>\n#include <stdint.h>\n'
            'static inline uint8_t* sdata_decrypt(const uint8_t*,size_t,size_t*) { return NULL; }\n')
        link = ['-framework', 'Security', '-framework', 'SystemConfiguration', '-liconv', '-lresolv'] \
            if sys.platform == 'darwin' else ['-ldl', '-lm']
        cls.binary = cls.root / 'driver'
        subprocess.run([cls.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-DMAG_PLAYER_ARCHIVE_NO_ZLIB=1', '-DMAG_PLAYER_SETTINGS_CONTROLS=1',
                        '-I' + str(ROOT / 'src'), '-I' + str(SDK), '-I' + str(cls.root),
                        *[f for f in cls.sdl if not f.startswith('-l') and not f.startswith('-L') and f != '-framework'],
                        str(ROOT / 'tests/player_settings_window.cpp'), str(ROOT / 'src/player_settings_sdl.cpp'),
                        *[str(ROOT / s) for s in FORM], str(cls.root / 'cargo/release/libmag_input.a'),
                        *[f for f in cls.sdl if f.startswith(('-l', '-L', '-Wl'))], *link, '-o', str(cls.binary)],
                       check=True, timeout=300)

    def test_form_and_panel(self):
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run([str(self.binary), folder], capture_output=True, text=True, timeout=60,
                                    env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('settings window checks passed', result.stdout)


class Win32SettingsWindowTests(unittest.TestCase):
    """The Win32 window: compiled by MinGW-w64 off Windows; built and driven on Windows."""

    def test_win32_window_compiles(self):
        compiler = shutil.which('x86_64-w64-mingw32-g++')
        if not compiler:
            self.skipTest('MinGW-w64 cross compiler')
        with tempfile.TemporaryDirectory() as folder:
            sources = ('src/player_settings_win32.cpp', 'src/player_settings_session.cpp', 'src/player_settings_form.cpp',
                       'tests/player_settings_win32.cpp')
            for source in sources:
                for controls in ([], ['-DMAG_PLAYER_SETTINGS_CONTROLS=1', '-DMAG_PLAYER_INPUT_HOLD=1']):
                    result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unknown-pragmas',
                                             '-DUNICODE', '-D_UNICODE', *controls, '-I' + str(ROOT / 'src'), '-c',
                                             str(ROOT / source), '-o', str(Path(folder) / 'object.o')],
                                            capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, source + ' ' + str(controls) + '\n' + result.stderr)

    @unittest.skipUnless(os.name == 'nt', 'drives a real Win32 window')
    def test_win32_window_edits_and_saves(self):
        compiler = shutil.which('clang-cl')
        if not compiler or not shutil.which('cargo'):
            self.skipTest('clang-cl and the pinned Rust toolchain')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = {**os.environ, 'CARGO_TARGET_DIR': str(root / 'cargo')}
            subprocess.run(['cargo', 'build', '--manifest-path', str(ROOT / 'crates/mag-input/Cargo.toml'), '--release',
                            '--locked', '--offline'], cwd=ROOT, env=env, check=True)
            (root / 'sdata_decrypt.h').write_text('#include <stddef.h>\n#include <stdint.h>\n'
                'static inline uint8_t* sdata_decrypt(const uint8_t*,size_t,size_t*) { return NULL; }\n')
            binary = root / 'driver.exe'
            subprocess.run([compiler, '/nologo', '/c', '/W3', '/WX', '/MT', str(ROOT / 'src/player_input_win32_map.c'),
                            '/Fo' + str(root / 'map.obj')], check=True, timeout=120)
            subprocess.run([compiler, '/nologo', '/std:c++20', '/EHsc', '/W3', '/WX', '/MT', '/DUNICODE', '/D_UNICODE',
                            '/DMAG_PLAYER_ARCHIVE_NO_ZLIB=1', '/DMAG_PLAYER_SETTINGS_CONTROLS=1', '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(ROOT / 'src'), '/I' + str(root),
                            str(ROOT / 'tests/player_settings_win32.cpp'), str(ROOT / 'src/player_settings_win32.cpp'),
                            str(root / 'map.obj'), *[str(ROOT / s) for s in FORM],
                            '/Fo' + str(root) + '\\', '/Fe' + str(binary), '/link', '/SUBSYSTEM:CONSOLE', str(root / 'cargo/release/mag_input.lib'),
                            'user32.lib', 'gdi32.lib', 'comctl32.lib', 'xinput.lib', 'userenv.lib', 'ws2_32.lib',
                            'bcrypt.lib', 'ntdll.lib', 'advapi32.lib', 'dbghelp.lib'], check=True, timeout=300)
            result = subprocess.run([str(binary), str(root / 'work')], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('win32 settings window checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
