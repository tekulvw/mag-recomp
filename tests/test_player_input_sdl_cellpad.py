"""The pinned runtime's cellPad SDL2 backend through mag-recomp's SDL seam and capture layer, with
synthetic SDL virtual controllers (tests/player_input_sdl_cellpad.c). No controller or game files."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('MAG_TEST_SDK') or ROOT / 'local/ps3recomp-runtime')
# The renames cmake/MagControls.cmake and cmake/MagPlayerSettings.cmake put on libs/input/cellPad.c.
RENAMES = {'SDL_GameControllerGetButton': 'mag_player_controller_button',
           'SDL_GameControllerGetAxis': 'mag_player_controller_axis',
           'SDL_GameControllerUpdate': 'mag_player_controller_update',
           'SDL_GameControllerOpen': 'mag_player_controller_open',
           'SDL_GameControllerClose': 'mag_player_controller_close',
           'SDL_GameControllerGetAttached': 'mag_player_controller_attached',
           'SDL_GameControllerSetPlayerIndex': 'mag_player_controller_set_player_index'}


def sdl_flags():
    tool = ['sdl2-config', '--cflags', '--libs'] if shutil.which('sdl2-config') else ['pkg-config', '--cflags', '--libs', 'sdl2']
    if not shutil.which(tool[0]):
        return None
    return shlex.split(subprocess.run(tool, check=True, capture_output=True, text=True).stdout)


class SdlCellPadSeamTests(unittest.TestCase):
    def test_ports_reach_the_capture_layer_and_an_idle_pad_waits(self):
        if os.name == 'nt':
            self.skipTest('POSIX compilers; Windows builds the same sources through CMake')
        if not (SDK / 'libs/input/cellPad.c').is_file():
            self.skipTest('pinned runtime checkout (tools/fetch_ps3recomp.py --role runtime)')
        if 'pad_sdl_claim' not in (SDK / 'libs/input/cellPad.c').read_text(errors='replace'):
            self.skipTest('the checked-out runtime predates the pinned SDL controller backend')
        flags = sdl_flags()
        cc, cxx = shutil.which('clang') or shutil.which('cc'), shutil.which('clang++') or shutil.which('c++')
        if not flags or not cc or not cxx:
            self.skipTest('SDL2 development files and a C/C++ compiler')
        prefix = [f for f in flags if f.startswith('-I')]
        prefix += ['-I' + str(Path(f[2:]).parent) for f in flags if f.startswith('-I') and Path(f[2:]).name == 'SDL2']
        with tempfile.TemporaryDirectory(prefix='mag-sdl-cellpad-') as tmp:
            tmp = Path(tmp)
            includes = ['-I' + str(SDK), '-I' + str(SDK / 'include'), '-I' + str(ROOT / 'src'), *prefix,
                        *[f for f in flags if f.startswith('-D')]]
            renames = ['-D%s=%s' % item for item in RENAMES.items()]
            subprocess.run([cc, '-std=gnu17', '-O1', '-c', str(ROOT / 'tests/player_input_sdl_cellpad.c'), *includes,
                            *renames, '-o', str(tmp / 'cellpad.o')], check=True)
            subprocess.run([cc, '-std=gnu17', '-O1', '-Wall', '-Wextra', '-Werror', '-c',
                            str(ROOT / 'src/player_input_sdl_pad.c'), *includes, '-o', str(tmp / 'seam.o')], check=True)
            subprocess.run([cxx, '-std=c++20', '-O1', '-c', str(ROOT / 'src/player_input_capture.cpp'),
                            '-I' + str(ROOT / 'src'), '-o', str(tmp / 'capture.o')], check=True)
            libs = [f for f in flags if not f.startswith(('-I', '-D'))]
            subprocess.run([cxx, str(tmp / 'cellpad.o'), str(tmp / 'seam.o'), str(tmp / 'capture.o'), *libs, '-lm',
                            '-pthread', '-o', str(tmp / 'fixture')], check=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('PAD_', 'SDL_', 'PS3RECOMP_'))}
            result = subprocess.run([str(tmp / 'fixture')], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if 'SKIP' in result.stdout:
                self.skipTest(result.stdout.strip().splitlines()[-1])
            self.assertIn('player_input_sdl_cellpad: ok', result.stdout)


if __name__ == '__main__':
    unittest.main()
