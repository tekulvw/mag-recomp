"""End-to-end shader output checks; no GPU or original program inputs."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "tools/check_render_frontend_shaders.py"
TOOL = Path(os.environ.get("MAG_SHADER_TOOL", ROOT / "crates/mag-render-shaders/target/debug" /
                          ("mag-render-shaders.exe" if os.name == "nt" else "mag-render-shaders")))


class GeneratedValidationTests(unittest.TestCase):
    def invoke(self, directory, *extra):
        return subprocess.run([sys.executable, str(CHECKER), "--shader-tool", str(TOOL),
                               "--output-dir", str(directory), *extra], text=True, capture_output=True)

    def test_real_generated_outputs_report_each_compiler_and_gpu_separately(self):
        self.assertTrue(TOOL.is_file(), "build the actual shader tool before this test")
        with tempfile.TemporaryDirectory() as temporary:
            result = self.invoke(Path(temporary), "--require-spirv-val", "--require-hlsl-compiler")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((Path(temporary) / "report.json").read_text())
            self.assertEqual(report["gpu_execution"], "pending")
            for name in ("copy_transform", "compact"):
                statuses = report["shaders"][name]
                self.assertEqual(statuses["spirv_val"], "passed")
                self.assertEqual(statuses["gpu_execution"], "pending")
                self.assertEqual(statuses["metal_compiler"], "pending")
                self.assertIn("passed", (statuses["hlsl_glslang"], statuses["hlsl_dxc"]))

    def test_missing_required_validator_fails_without_success_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            # A previous successful report must not survive a failed rerun.
            (Path(temporary) / "report.json").write_text('{"gpu_execution":"pending","old_pass":true}')
            result = self.invoke(Path(temporary), "--spirv-val", "", "--require-spirv-val")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("spirv-val is required", result.stderr)
            self.assertFalse((Path(temporary) / "report.json").exists())

    def test_corrupted_spirv_is_rejected_by_real_validator(self):
        validator = shutil.which("spirv-val")
        self.assertIsNotNone(validator, "spirv-val required for generated-output verification")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.invoke(directory, "--require-spirv-val")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            shader = directory / "copy_transform/shader.spv"
            payload = shader.read_bytes()
            shader.write_bytes(b"\0\0\0\0" + payload[4:])
            rejected = subprocess.run([validator, "--target-env", "vulkan1.1", str(shader)],
                                      capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)


if __name__ == "__main__":
    unittest.main()
