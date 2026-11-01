"""Device-queue receipt/reset races and multiple mice, without any OS capture."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class DesktopCaptureTests(unittest.TestCase):
    def test_queued_capture_generations_and_per_device_button_holds(self):
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:self.skipTest('C++20 compiler required')
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'capture'
            subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'src'),str(ROOT/'tests/player_desktop_capture.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
