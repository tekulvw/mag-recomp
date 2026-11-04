"""POSIX guest arena is 4 GB aligned, lazily read/write and guarded (synthetic)."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'posix' and shutil.which('c++'), 'POSIX C++ compiler')
class GuestArenaTests(unittest.TestCase):
    def test_arena_alignment_access_and_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'arena'
            built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                    str(ROOT / 'tests/guest_arena.cpp'), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('guest arena ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
