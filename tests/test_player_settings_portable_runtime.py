"""Synthetic launch snapshot, capability rejection and display rollback checks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PortableRuntimeTests(unittest.TestCase):
    @unittest.skipIf(os.name == 'nt', 'POSIX environment fixture; production Windows is cross-compiled separately')
    def test_snapshot_and_display_rollback(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'runtime-fixture'
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Isrc',
                            'src/player_settings_portable.cpp', 'src/player_settings_portable_runtime.cpp',
                            'tests/player_settings_portable_runtime.cpp', '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary), str(Path(directory) / 'private')], check=True)

if __name__ == '__main__':
    unittest.main()
