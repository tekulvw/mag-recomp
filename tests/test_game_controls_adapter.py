"""Game controls adapter against the real Rust input engine, over a synthetic guest memory."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONTROLS = ('mag_game_controls_adapter', 'mag_game_controls_catalog', 'mag_game_controls_guest',
            'mag_game_controls_hooks', 'mag_game_controls_mouse_aim')


@unittest.skipIf(os.name == 'nt', 'POSIX C++ linker fixture; Rust core matrix separately covers Windows')
class GameControlsAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cargo'):
            raise unittest.SkipTest('Rust toolchain required')
        if shutil.which('rustup') and subprocess.run(['rustup', 'run', '1.99.0', 'cargo', '--version'],
                                                     capture_output=True).returncode:
            raise unittest.SkipTest('installed pinned Rust toolchain required')
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-game-controls-')
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        env = {**os.environ, 'CARGO_TARGET_DIR': str(work / 'cargo')}
        subprocess.run(['cargo', 'build', '--manifest-path', str(ROOT / 'crates/mag-input/Cargo.toml'),
                        '--release', '--locked', '--offline'], cwd=ROOT, env=env, check=True)
        src = ROOT / 'src'
        sources = [ROOT / 'tests/game_controls_adapter.cpp', src / 'player_controls_rust.cpp',
                   src / 'player_input_capture.cpp'] + [src / 'game_controls' / f'{name}.cpp' for name in CONTROLS]
        cls.binary = work / 'game_controls_adapter'
        args = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', f'-I{src}',
                f'-I{src / "game_controls"}', *map(str, sources), str(work / 'cargo/release/libmag_input.a'),
                '-o', str(cls.binary)]
        if sys.platform == 'darwin':
            args += ['-framework', 'Security', '-framework', 'SystemConfiguration', '-liconv', '-lresolv']
        else:
            args += ['-ldl', '-lm']
        subprocess.run(args, check=True)

    def run_scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_owner_resolution_accepts_the_local_player_and_rejects_each_broken_link(self):
        self.run_scenario('owner')

    def test_malformed_lists_are_unreadable_not_empty(self):
        self.run_scenario('lists')

    def test_gameplay_and_system_routes_follow_the_ui_rules(self):
        self.run_scenario('routes')

    def test_full_tick_publishes_a_key_bound_action_and_observes_its_receipts(self):
        self.run_scenario('tick')

    def test_route_handler_recognition_publishes_only_the_tuple(self):
        self.run_scenario('route')

    def test_mouse_look_clears_stick_rates_and_turns_the_yaw_request_once(self):
        self.run_scenario('mouse')

    def test_aim_assist_is_denied_after_kbm_until_a_clean_cleanup(self):
        self.run_scenario('assist')

    def test_handler_lifecycle_tombstones_and_proven_registration(self):
        self.run_scenario('lifecycle')

    def test_foreign_threads_and_events_outside_a_tick_are_ignored_and_errors_cancel(self):
        self.run_scenario('threads')


if __name__ == '__main__':
    unittest.main()
