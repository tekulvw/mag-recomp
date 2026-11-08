"""Synthetic check: Linux configures the runtime's SPU decode cache on the heap.

A fixture project includes cmake/MagLinuxRuntime.cmake and then declares the
runtime's own option, as the runtime subdirectory does after it. No compiler,
no runtime checkout and no game files are needed."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'cmake/MagLinuxRuntime.cmake'


@unittest.skipUnless(shutil.which('cmake'), 'cmake is required')
class LinuxRuntimeDefaultsTests(unittest.TestCase):
    def configure(self, system, *extra):
        with tempfile.TemporaryDirectory() as tmp:
            source, build = Path(tmp) / 'source', Path(tmp) / 'build'
            source.mkdir()
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(fixture NONE)\n'
                f'include("{MODULE.as_posix()}")\n'
                '# The runtime declares its option after this module, as add_subdirectory does.\n'
                'option(PS3RECOMP_SPU_DECODE_CACHE_HEAP "runtime default" OFF)\n'
                'message(STATUS "decode-cache-heap=${PS3RECOMP_SPU_DECODE_CACHE_HEAP}")\n')
            run = subprocess.run(['cmake', '-S', str(source), '-B', str(build), f'-DCMAKE_SYSTEM_NAME={system}', *extra],
                                 capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stderr)
            return next(line.split('=', 1)[1] for line in run.stdout.splitlines() if 'decode-cache-heap=' in line)

    def test_linux_configures_the_heap_cache(self):
        self.assertEqual(self.configure('Linux'), 'ON')

    def test_other_hosts_keep_the_runtime_default(self):
        self.assertEqual(self.configure('Darwin'), 'OFF')
        self.assertEqual(self.configure('Windows'), 'OFF')

    def test_an_explicit_choice_still_wins(self):
        self.assertEqual(self.configure('Linux', '-DPS3RECOMP_SPU_DECODE_CACHE_HEAP=OFF'), 'OFF')

    def test_the_module_is_applied_before_the_runtime(self):
        text = (ROOT / 'CMakeLists.txt').read_text()
        include = text.index('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MagLinuxRuntime.cmake")')
        self.assertLess(include, text.index('add_subdirectory("${PS3RECOMP_DIR}"'))


if __name__ == '__main__':
    unittest.main()
