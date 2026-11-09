"""Game controls settings catalog with the portable C++ engine; synthetic profiles only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GameControlsCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-game-controls-catalog-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'catalog'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                        '-I' + str(ROOT / 'src/game_controls'), str(ROOT / 'tests/game_controls_catalog.cpp'),
                        str(ROOT / 'src/game_controls/mag_game_controls_catalog.cpp'),
                        str(ROOT / 'src/player_controls.cpp'), '-o', str(cls.binary)], check=True)

    def test_descriptors_follow_the_catalog(self):
        subprocess.run([str(self.binary), 'descriptors'], check=True)

    def test_defaults_validate_and_unsupported_bindings_are_refused(self):
        subprocess.run([str(self.binary), 'defaults'], check=True)

    def test_upgrade_adds_system_buttons_once_without_displacing_custom_bindings(self):
        subprocess.run([str(self.binary), 'upgrade'], check=True)


if __name__ == '__main__':
    unittest.main()
