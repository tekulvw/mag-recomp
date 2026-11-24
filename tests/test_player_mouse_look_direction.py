"""Mouse look direction from the desktop backends' raw motion to the look angle; synthetic numbers.

The Win32 translation is portable C, so this runs on every host. The SDL backend's pass-through of
yrel is checked in tests/player_input_desktop_sdl.cpp."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'
STRICT = ['-Wall', '-Wextra', '-Werror']


class MouseLookDirectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
        cxx = shutil.which('clang++') or shutil.which('g++') or shutil.which('c++')
        if not cc or not cxx:
            raise unittest.SkipTest('C and C++ compilers required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-look-direction-')
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        subprocess.run([cc, '-std=c11', *STRICT, '-I' + str(SRC), '-c', str(SRC / 'player_input_win32_map.c'),
                        '-o', str(root / 'map.o')], check=True)
        cls.binary = root / 'direction'
        subprocess.run([cxx, '-std=c++20', *STRICT, '-I' + str(SRC), str(ROOT / 'tests/player_mouse_look_direction.cpp'),
                        str(SRC / 'player_controls.cpp'), str(root / 'map.o'), '-o', str(cls.binary)], check=True)

    def test_win32_mouse_up_looks_up_without_invert(self):
        subprocess.run([str(self.binary), 'win32'], check=True)

    def test_sdl_mouse_up_looks_up_without_invert(self):
        subprocess.run([str(self.binary), 'sdl'], check=True)


if __name__ == '__main__':
    unittest.main()
