"""The host frame clock reads a fine-grained monotonic clock (src/mag_frame_clock.h)."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def _compiler():
    if os.name == 'nt':
        return os.environ.get('MAG_TEST_CLANG_CL') or shutil.which('clang-cl')
    return shutil.which('c++')


@unittest.skipUnless(_compiler(), 'C++ compiler (clang-cl on Windows)')
class FrameClockTests(unittest.TestCase):
    def test_ticks_are_evenly_spaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / ('frame_clock.exe' if os.name == 'nt' else 'frame_clock')
            if os.name == 'nt':
                command = [_compiler(), '/nologo', '/std:c++17', '/O2', '/EHsc', '/W4', '/WX',
                           '/I' + str(ROOT / 'src'), str(ROOT / 'tests/frame_clock.cpp'),
                           '/Fe' + str(binary), '/Fo' + str(tmp) + os.sep]
            else:
                command = [_compiler(), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                           '-I' + str(ROOT / 'src'), str(ROOT / 'tests/frame_clock.cpp'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            print(result.stdout, end='')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[frame_clock] PASS', result.stdout)


class FrameClockSourceTests(unittest.TestCase):
    def test_frame_clock_does_not_read_the_tick_count(self):
        source = (ROOT / 'src/main.cpp').read_text()
        start = source.index('static DWORD WINAPI frame_clock(LPVOID)')
        body = source[start:source.index('    return 0;\n}', start)]
        self.assertNotIn('GetTickCount', body)
        self.assertEqual(len(re.findall(r'mag_frame_clock_ms\(\)', body)), 4)


if __name__ == '__main__':
    unittest.main()
