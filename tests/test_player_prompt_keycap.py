"""Key cap images for picture prompts (docs/native-controls.md, section 12); synthetic only."""
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

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_key_caps_draw_an_outline_and_the_label(self):
        self.run_case('render')

    def test_encodings_round_trip_through_reference_decoders(self):
        self.run_case('encode')


if __name__ == '__main__':
    unittest.main()
