"""The Windows getenv cache (src/mag_env_cache_win.cpp): lookups, invalidation, threads.

On Windows the fixture is built as the engine builds it (clang-cl, static CRT, the
header force-included). Elsewhere the same checks run over setenv/unsetenv.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCES = [str(ROOT / 'tests/env_cache_win.cpp'), str(ROOT / 'src/mag_env_cache_win.cpp')]


def _compiler():
    if os.name == 'nt':
        return os.environ.get('MAG_TEST_CLANG_CL') or shutil.which('clang-cl')
    return shutil.which('c++')


@unittest.skipUnless(_compiler(), 'C++ compiler (clang-cl on Windows)')
class EnvCacheTests(unittest.TestCase):
    def build(self, tmp):
        binary = pathlib.Path(tmp) / ('env_cache.exe' if os.name == 'nt' else 'env_cache')
        if os.name == 'nt':
            command = [_compiler(), '/nologo', '/std:c++17', '/O2', '/MT', '/EHsc', '/W4', '/WX',
                       '-Wno-deprecated-declarations',
                       '/FI' + str(ROOT / 'src/mag_env_cache_win.h'), *SOURCES, '/Fe' + str(binary),
                       '/Fo' + str(tmp) + os.sep]
        else:
            command = [_compiler(), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                       *SOURCES, '-o', str(binary)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return binary

    def run_fixture(self, binary, **extra):
        env = {**os.environ, 'MAG_ENV_TEST_INHERITED': 'from-parent', **extra}
        env.pop('MAG_ENV_TEST_ABSENT', None)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('[env_cache] PASS', result.stdout)
        return result.stdout

    def test_cached_lookups_follow_every_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = self.run_fixture(self.build(tmp))
            print(out, end='')

    def test_cache_off_forwards_to_the_crt(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_fixture(self.build(tmp), MAG_GETENV_CACHE='0')


class EnvCacheBuildTests(unittest.TestCase):
    def test_windows_build_force_includes_the_cache(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('src/mag_env_cache_win.cpp', cmake)
        self.assertIn('mag_env_cache_win.h', cmake)


if __name__ == '__main__':
    unittest.main()
