"""Synthetic pending-UI/frame-pump, launch snapshot and display rollback checks."""
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PortableRuntimeTests(unittest.TestCase):
    @unittest.skipIf(os.name == 'nt', 'POSIX environment fixture; production Windows is cross-compiled separately')
    def test_pending_ui_keeps_polling_and_preserves_snapshot_and_rollback(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'runtime-fixture'
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Isrc',
                            'src/player_settings_portable.cpp', 'src/player_settings_portable_runtime.cpp',
                            'tests/player_settings_portable_runtime.cpp', '-pthread', '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary), str(Path(directory) / 'private')], check=True, timeout=10)

    @unittest.skipIf(os.name == 'nt', 'SDL event fixture uses the Linux desktop implementation')
    def test_modal_input_filter_keeps_window_close_events_pumping(self):
        compiler=shutil.which('clang++') or shutil.which('g++')
        pkg=shutil.which('pkg-config')
        if not compiler or not pkg:self.skipTest('C++ and SDL2 development package required')
        flags=subprocess.run([pkg,'--cflags','--libs','sdl2'],capture_output=True,text=True)
        if flags.returncode:self.skipTest('SDL2 development package unavailable')
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'event-fixture'
            subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Isrc',
                'src/player_settings_linux.cpp','src/player_settings_portable.cpp',
                'tests/player_settings_events.cpp','-pthread',*shlex.split(flags.stdout),'-o',str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)

if __name__ == '__main__':
    unittest.main()
