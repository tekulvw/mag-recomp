"""The runtime extension registry: empty by default, refuses double registration, carries scratch."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'clang++ required')
class ExtensionRegistryTests(unittest.TestCase):
    def test_registry_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'registry'
            subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                            str(ROOT / 'src/mag_extension.cpp'), str(ROOT / 'tests/fixtures/extension_registry.cpp'),
                            '-o', str(binary)], check=True, capture_output=True, text=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('PASS extension registry', run.stdout)


if __name__ == '__main__':
    unittest.main()
