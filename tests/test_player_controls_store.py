"""Synthetic portable controls persistence; does not open a window or game."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ControlsStoreTests(unittest.TestCase):
    def test_profile_store_preserves_preferences_and_rejects_invalid_data(self):
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler: self.skipTest('C++20 compiler required')
        with tempfile.TemporaryDirectory(prefix='mag-controls-store-') as scratch:
            root=Path(scratch); binary=root/'store'
            sources=['tests/player_controls_store.cpp','src/player_controls.cpp','src/player_controls_store.cpp','src/player_preferences_file.cpp']
            subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),*[str(ROOT/s) for s in sources],'-o',str(binary)],check=True)
            preferences=root/'preferences'; preferences.mkdir()
            subprocess.run([str(binary),str(preferences)],check=True)
if __name__=='__main__': unittest.main()
