"""Portable editor/store service and synthetic native controls panel."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

@unittest.skipIf(os.name == 'nt', 'C++ linker fixture; Rust editor tests run on Windows')
class ControlsSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('clang++') or shutil.which('g++')
        if not cls.compiler or not shutil.which('cargo'):
            raise unittest.SkipTest('C++20 and pinned Rust toolchain required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-controls-settings-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        env = {**os.environ, 'CARGO_TARGET_DIR': str(cls.root/'cargo')}
        subprocess.run(['cargo', 'build', '--manifest-path', str(ROOT/'crates/mag-input/Cargo.toml'), '--release', '--locked', '--offline'], cwd=ROOT, env=env, check=True)
        cls.common = [str(ROOT/p) for p in ['src/player_controls_settings.cpp', 'src/player_controls_store.cpp', 'src/player_controls_rust.cpp', 'src/player_preferences_file.cpp']]
        cls.library = cls.root/'cargo/release/libmag_input.a'
        cls.link = ['-framework', 'Security', '-framework', 'SystemConfiguration', '-liconv', '-lresolv'] if sys.platform == 'darwin' else ['-ldl', '-lm']

    def test_conflict_replace_clear_reset_save_and_failed_validation(self):
        binary = self.root/'service'
        subprocess.run([self.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(ROOT/'src'), str(ROOT/'tests/player_controls_settings.cpp'), *self.common, str(self.library), *self.link, '-o', str(binary)], check=True)
        with tempfile.TemporaryDirectory() as folder:
            subprocess.run([str(binary), folder], check=True, timeout=10)

    @unittest.skipUnless(sys.platform == 'darwin', 'native AppKit panel')
    def test_controls_apply_cancel_and_explicit_unbind_preserve_general_settings(self):
        objects = []
        for source in ['player_settings_macos', 'player_settings_store']:
            obj = self.root/(source+'.o')
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-I'+str(ROOT/'src'), '-c', str(ROOT/'src'/f'{source}.m'), '-o', str(obj)], check=True)
            objects.append(str(obj))
        binary = self.root/'panel'
        subprocess.run([self.compiler, '-std=c++20', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-pthread', '-I'+str(ROOT/'src'), str(ROOT/'tests/player_controls_panel.mm'), str(ROOT/'src/player_controls_macos.mm'), *self.common, *objects, str(self.library), *self.link, '-framework', 'AppKit', '-framework', 'QuartzCore', '-framework', 'GameController', '-o', str(binary)], check=True)
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run([str(binary), folder], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('preserve general settings passed', result.stdout)
