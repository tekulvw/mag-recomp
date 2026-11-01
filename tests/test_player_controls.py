"""Portable input contracts with synthetic events; no game data or window."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlayerControlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-controls-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name)/'controls'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'src'), str(ROOT/'tests/player_controls.cpp'),
                        str(ROOT/'src/player_controls.cpp'), '-o', str(cls.binary)], check=True)

    def test_device_policy_and_look_arbitration(self):
        subprocess.run([str(self.binary), 'policy'], check=True)

    def test_mouse_displacement_and_mode_gains(self):
        subprocess.run([str(self.binary), 'mouse'], check=True)

    def test_controller_transfer_function(self):
        subprocess.run([str(self.binary), 'controller'], check=True)

    def test_current_frame_and_once_only_consumption(self):
        subprocess.run([str(self.binary), 'frames'], check=True)

    def test_binding_aggregation_and_source_filter(self):
        subprocess.run([str(self.binary), 'bindings'], check=True)

    def test_focus_release_and_fresh_activation(self):
        subprocess.run([str(self.binary), 'focus'], check=True)

    def test_disconnect_preserves_other_binding_and_rejects_late_input(self):
        subprocess.run([str(self.binary), 'device'], check=True)

    def test_captured_motion_with_zero_net_displacement(self):
        subprocess.run([str(self.binary), 'motion'], check=True)

    def test_raw_controller_axes_and_lifecycle(self):
        subprocess.run([str(self.binary), 'axes'], check=True)

    def test_disconnect_preserves_queued_cross_source_order(self):
        subprocess.run([str(self.binary), 'device-order'], check=True)


if __name__ == '__main__':
    unittest.main()
