"""Actual-SDK neutral checks for the MAG-only recorder observation adapter."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.jobio import parse

ROOT = Path(__file__).resolve().parents[1]
SDK = os.environ.get("MAG_TEST_SDK")

@unittest.skipUnless(SDK, "set MAG_TEST_SDK for actual-SDK recorder checks")
class CaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.tmp.name)
        cls.exe = cls.base / "capture"
        subprocess.run(["cc", "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        "-I" + str(Path(SDK) / "runtime"), "-I" + str(Path(SDK) / "include"),
                        str(ROOT / "src/mag_jobio_capture.c"),
                        str(ROOT / "tests/fixtures/jobio_transfer_capture.c"),
                        "-pthread", "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_mode(self, mode):
        directory = self.base / mode
        directory.mkdir()
        env = dict(os.environ)
        env.pop("MAG_JOB_IO_TRANSFERS_ONLY", None)
        if mode == "transfers":
            env["MAG_JOB_IO_TRANSFERS_ONLY"] = "1"
        subprocess.run([str(self.exe), str(directory), mode], env=env, check=True, timeout=15)
        return directory, parse((directory / "job-0000.jobio").read_bytes())

    def test_default_keeps_complete_version_one_and_atomic_status(self):
        directory, rec = self.run_mode("full")
        self.assertTrue(rec["full_job_replay"])
        self.assertEqual(rec["format_version"], 1)
        self.assertEqual([e["kind"] for e in rec["events"]], [3, 3, 3, 4, 5, 1, 1, 2, 6])
        self.assertEqual(rec["events"][3]["extra"], 1)
        self.assertFalse((directory / "job-0000.mode.json").exists())

    def test_poll_flood_does_not_truncate_transfers_or_contaminate_counters(self):
        directory, rec = self.run_mode("transfers")
        self.assertEqual(rec["format_version"], 2)
        self.assertFalse(rec["full_job_replay"])
        self.assertFalse(rec["truncated"])
        self.assertEqual([e["kind"] for e in rec["events"]], [1, 1, 2, 6])
        self.assertEqual([e["data"] for e in rec["events"]], [bytes([1, 2, 3, 4]), bytes([1, 2, 3, 4]), bytes([5, 6]), b""])
        self.assertEqual((rec["entry_ls"][7], rec["exit_ls"][7]), (12, 23))
        mode = json.loads((directory / "job-0000.mode.json").read_text())
        self.assertFalse(mode["full_job_replay"])
        self.assertEqual(sum(mode["raw_by_kind"]), 800006)
        self.assertEqual(sum(mode["recorded_by_kind"]), 4)
        self.assertEqual(sum(mode["skipped_by_kind"]), 800002)
        self.assertLess((directory / "job-0000.jobio").stat().st_size, 600000)

if __name__ == "__main__":
    unittest.main()
