"""The standalone CPU runner must be passive when unittest discovers tools."""
from pathlib import Path
import subprocess
import sys
import unittest

RUNNER = Path(__file__).resolve().parents[1] / "tools/test_render_scene_resource_index.py"


class SceneResourceRunnerTests(unittest.TestCase):
    def test_discovery_import_ignores_caller_arguments_and_starts_no_fixture(self):
        code = """
import runpy
import sys
from unittest.mock import patch
source = sys.argv[1]
sys.argv = ['unittest', 'discover', '-s', 'tools', '-p', 'test_*.py', '-t', '.', '-v']
with patch('subprocess.run', side_effect=AssertionError('fixture started during import')):
    runpy.run_path(source, run_name='discovered_scene_resource_runner')
print('passive import')
"""
        result = subprocess.run([sys.executable, "-c", code, str(RUNNER)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "passive import")

    def test_help_keeps_the_explicit_component_benchmark_option(self):
        result = subprocess.run([sys.executable, str(RUNNER), "--help"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--bench", result.stdout)


if __name__ == "__main__":
    unittest.main()
