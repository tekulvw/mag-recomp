"""The native runner must never leave stale successful GPU evidence on error."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeRunnerFailureTests(unittest.TestCase):
    def test_invalid_explicit_sdk_overwrites_old_pass_report(self):
        with tempfile.TemporaryDirectory(prefix="sdk runner failure ") as directory:
            root = Path(directory)
            report = root / "report.json"
            report.write_text('{"passed": true, "real_hardware": true}\n')
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/check_render_frontend_sdk.py"),
                 "--sdk-source", str(root / "missing sdk"),
                 "--shader-root", str(root / "shaders"), "--report", str(report)],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            failed = json.loads(report.read_text())
            self.assertFalse(failed["passed"])
            self.assertEqual(failed["full_m2"], "pending")
            self.assertTrue(failed["error"])
            self.assertNotIn("real_hardware", failed)


if __name__ == "__main__":
    unittest.main()
