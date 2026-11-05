"""The host frame clock presents each executed guest flip once (src/frame_present_policy.h)."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler')
class FramePresentPolicyTests(unittest.TestCase):
    def test_taken_receipt_is_not_presented_twice(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'policy'
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                     str(ROOT / 'tests/frame_present_policy.cpp'), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[frame_present_policy] PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
