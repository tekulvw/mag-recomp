"""Keyboard and mouse button prompts (docs/native-controls.md, section 12); synthetic inputs only."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GameControlsPromptsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-game-prompts-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'prompts'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                        '-I' + str(ROOT / 'src/game_controls'), str(ROOT / 'tests/game_controls_prompts.cpp'),
                        str(ROOT / 'src/game_controls/mag_game_controls_prompts.cpp'),
                        str(ROOT / 'src/game_controls/mag_game_controls_catalog.cpp'),
                        str(ROOT / 'src/player_prompt_labels.cpp'),
                        str(ROOT / 'src/player_controls.cpp'), '-o', str(cls.binary)], check=True)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_device_follows_the_last_press_with_mode_and_controller_fallbacks(self):
        self.run_case('device')

    def test_key_labels_follow_the_host_layout_with_short_fallbacks(self):
        self.run_case('labels')

    def test_placeholders_resolve_to_the_live_bindings(self):
        self.run_case('resolve')

    def test_text_rewrite_replaces_only_resolvable_placeholders(self):
        self.run_case('rewrite')

    def test_seam_swaps_the_text_into_scratch_and_restores_the_game_pointer(self):
        self.run_case('seam')

    def test_host_backends_compile_where_their_headers_exist(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        source = ROOT / 'src/player_prompt_labels_none.cpp'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                        '-I' + str(ROOT / 'src'), str(source)], check=True)
        sdl = shutil.which('sdl2-config')
        if sdl:
            flags = subprocess.run([sdl, '--cflags'], capture_output=True, text=True, check=True).stdout.split()
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                            '-I' + str(ROOT / 'src'), *flags, str(ROOT / 'src/player_prompt_labels_sdl.cpp')], check=True)
        if sys.platform == 'darwin':
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', '-fobjc-arc',
                            '-x', 'objective-c++', '-I' + str(ROOT / 'src'),
                            str(ROOT / 'src/player_prompt_labels_macos.mm')], check=True)
        mingw = shutil.which('x86_64-w64-mingw32-g++')
        if mingw:
            subprocess.run([mingw, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                            '-I' + str(ROOT / 'src'), str(ROOT / 'src/player_prompt_labels_win32.cpp')], check=True)


if __name__ == '__main__':
    unittest.main()
