"""Portable keyboard/mouse desktop capture core with a fake window system; synthetic input only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DesktopCaptureCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-desktop-capture-core-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'core'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(ROOT / 'src'),
                        str(ROOT / 'tests/player_desktop_capture_core.cpp'),
                        str(ROOT / 'src/player_desktop_capture_core.cpp'), '-o', str(cls.binary)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True)

    def test_focus_loss_releases_held_keys_and_buttons(self):
        self.run_case('focus')

    def test_controls_held_when_focus_returns_need_a_fresh_press(self):
        self.run_case('fresh')

    def test_relative_motion_only_while_ready_and_relative(self):
        self.run_case('motion')

    def test_uninstall_releases_everything(self):
        self.run_case('uninstall')

    def test_without_a_request_nothing_is_touched(self):
        self.run_case('inert')

    def test_mouse_mode_toggle_keeps_held_buttons(self):
        self.run_case('mouse-mode')

    def test_click_or_focus_return_recaptures(self):
        self.run_case('recapture')

    def test_escape_quit_suppression_rule(self):
        self.run_case('quit-key')

    def test_mouse_lock_engages_releases_and_reengages_with_one_log_line_each(self):
        self.run_case('mouse-lock')

    def test_relocking_discards_motion_from_the_centring_warp(self):
        self.run_case('reacquire')


if __name__ == '__main__':
    unittest.main()
