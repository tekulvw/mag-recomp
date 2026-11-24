"""Key cap images and menu picture prompts (docs/native-controls.md, section 12.5); synthetic only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlayerPromptKeycapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-keycap-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'keycap'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                        str(ROOT / 'tests/player_prompt_keycap.cpp'), str(ROOT / 'src/player_prompt_keycap.cpp'),
                        '-o', str(cls.binary)], check=True)
        cls.icons = Path(cls.tmp.name) / 'icons'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                        '-I' + str(ROOT / 'src/game_controls'), str(ROOT / 'tests/game_controls_icons.cpp'),
                        str(ROOT / 'src/game_controls/mag_game_controls_icons.cpp'),
                        str(ROOT / 'src/game_controls/mag_game_controls_catalog.cpp'),
                        str(ROOT / 'src/player_controls.cpp'), str(ROOT / 'src/player_prompt_keycap.cpp'),
                        str(ROOT / 'src/player_sha256.cpp'), '-o', str(cls.icons)], check=True)

    def run_case(self, name, binary=None):
        result = subprocess.run([str(binary or self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_key_caps_draw_an_outline_and_the_label(self):
        self.run_case('render')

    def test_encodings_round_trip_through_reference_decoders(self):
        self.run_case('encode')

    def test_icons_are_recognised_by_layout_and_identity_only(self):
        self.run_case('identity', self.icons)

    def test_key_caps_show_for_keyboard_in_menus_and_switch_back(self):
        self.run_case('pictures', self.icons)


if __name__ == '__main__':
    unittest.main()
