"""Device-queue receipt/reset races and multiple mice, without any OS capture."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys
ROOT=Path(__file__).resolve().parents[1]
class DesktopCaptureTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform=='darwin','macOS capture lifecycle fixture')
    def test_click_recapture_ignores_stale_command_state_and_preserves_held_keys(self):
        with tempfile.TemporaryDirectory(prefix='mag-desktop-focus-') as tmp:
            binary=Path(tmp)/'focus'
            subprocess.run(['xcrun','clang++','-std=c++20','-fobjc-arc','-fblocks',
                            '-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),
                            str(ROOT/'tests/player_desktop_focus.mm'),'-framework','AppKit',
                            '-framework','GameController','-framework','CoreGraphics',
                            '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_queued_capture_generations_and_per_device_button_holds(self):
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:self.skipTest('C++20 compiler required')
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'capture'
            subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'src'),str(ROOT/'tests/player_desktop_capture.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
