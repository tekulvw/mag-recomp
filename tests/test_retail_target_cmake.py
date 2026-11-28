from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cmake'), 'CMake required')
class RetailTargetCmakeTests(unittest.TestCase):
    def configure_guard(self, target=None):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 'guard.cmake'
            script.write_text(f'include("{(ROOT / "cmake/MagRetailTarget.cmake").as_posix()}")\n'
                              'message(STATUS "selected=${MAG_RETAIL_TARGET}")\n')
            args = ['cmake'] + ([f'-DMAG_RETAIL_TARGET={target}'] if target else [])
            return subprocess.run(args + ['-P', str(script)], capture_output=True, text=True)

    def test_default_and_explicit_v1_build_contract(self):
        for target in (None, 'retail-v1.0'):
            with self.subTest(target=target):
                result = self.configure_guard(target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('selected=retail-v1.0', result.stdout)

    def test_v212_native_build_is_visible_audit_only_failure(self):
        result = self.configure_guard('retail-v2.12')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('audit-only', result.stderr)
        self.assertIn('tools/lift.py', result.stderr)

    def test_unknown_target_fails_before_runtime_configuration(self):
        result = self.configure_guard('guess')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('MAG_RETAIL_TARGET', result.stderr)


if __name__ == '__main__':
    unittest.main()
