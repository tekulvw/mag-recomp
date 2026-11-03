"""Fail-closed receipts for the source-bound, CPU-only SDK runner."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VulkanSdkCpuRunnerTests(unittest.TestCase):
    def invoke(self, root, *extra):
        report = root / "report.json"
        report.write_text('{"passed": true, "gpu_executed": true}\n')
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/test_render_frontend_vulkan_sdk_cpu.py"),
             "--sdk-root", str(root / "sdk"), "--vulkan-include", str(root),
             "--sdl-include", str(root), "--output-dir", str(root), *extra],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        failed = json.loads(report.read_text())
        self.assertFalse(failed["passed"])
        for key in ("runtime_executed", "device_created", "queue_executed", "gpu_executed"):
            self.assertFalse(failed[key])
        self.assertFalse(failed["cpu_fixtures_executed"])
        self.assertTrue(failed["error"])
        return failed

    def test_missing_sdk_replaces_stale_success_without_starting_fixture(self):
        with tempfile.TemporaryDirectory(prefix="Vulkan SDK missing ") as temporary:
            failed = self.invoke(Path(temporary))
            self.assertIn("sdk-head failed", failed["error"])
            self.assertEqual(len(failed["commands"]), 1)

    def test_mutable_revision_is_refused_before_any_subprocess(self):
        with tempfile.TemporaryDirectory(prefix="Vulkan SDK ref ") as temporary:
            failed = self.invoke(Path(temporary), "--sdk-revision", "main")
            self.assertIn("immutable", failed["error"])
            self.assertEqual(failed["commands"], [])

    def test_wrong_exact_sdk_cannot_silently_test_another_checkout(self):
        with tempfile.TemporaryDirectory(prefix="Vulkan SDK identity ") as temporary:
            root = Path(temporary)
            sdk = root / "sdk"
            sdk.mkdir()
            subprocess.run(["git", "init", "-q", str(sdk)], check=True,
                           capture_output=True, timeout=10)
            subprocess.run(["git", "-C", str(sdk), "-c", "user.name=CPU fixture",
                            "-c", "user.email=fixture@example.invalid", "commit", "-q",
                            "--allow-empty", "-m", "synthetic SDK identity"], check=True,
                           capture_output=True, timeout=10)
            failed = self.invoke(root)
            self.assertIn("explicit SDK revision mismatch", failed["error"])
            self.assertEqual(len(failed["commands"]), 1)


if __name__ == "__main__":
    unittest.main()
