"""Exercise GTK3/SDL2 controls on a synthetic Linux display, never a game."""
import ctypes.util
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(platform.system() == 'Linux', 'Linux settings controls')
class LinuxSettingsTests(unittest.TestCase):
    def test_native_edit_display_confirmation_timeout_and_f10(self):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler or not ctypes.util.find_library('SDL2') or not ctypes.util.find_library('gtk-3'):
            self.skipTest('C++ compiler, SDL2 and GTK3 required')
        if not os.environ.get('DISPLAY'):
            self.skipTest('Run under xvfb-run or a test display')
        with tempfile.TemporaryDirectory(prefix='mag-linux-settings-') as folder:
            binary=Path(folder)/'settings-ui'
            for flags in ([],['-DMAG_PLAYER_VULKAN_FXAA=1']):
                with self.subTest(fxaa=bool(flags)):
                    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',*flags,
                        str(ROOT/'tests/player_settings_linux.cpp'),str(ROOT/'src/player_settings_portable.cpp'),
                        '-lSDL2','-ldl','-pthread','-o',str(binary)],check=True,timeout=60)
                    subprocess.run([str(binary)],check=True,timeout=30,
                        env={**os.environ,'G_DEBUG':'fatal-criticals'})

if __name__ == '__main__':
    unittest.main()
