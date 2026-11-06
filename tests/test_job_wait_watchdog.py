"""The opt-in job-wait watchdog reports a stalled wait once and is inert by default (src/mag_job_wait_watchdog.h)."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler')
class JobWaitWatchdogTests(unittest.TestCase):
    def test_synthetic_job(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'watchdog'
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                     str(ROOT / 'tests/job_wait_watchdog.cpp'), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[job_wait_watchdog] PASS', result.stdout)
            self.assertEqual(result.stdout.count('[mag job-wait watchdog] '), 1, result.stdout)


class TraceCallSiteTests(unittest.TestCase):
    """The trace scope only compiles inside an engine build, so check the call sites in source."""
    def test_watchdog_is_gated_by_the_cached_threshold(self):
        source = (ROOT / 'src/mag_graphics_trace.cpp').read_text()
        self.assertIn('getenv("MAG_JOB_WAIT_WATCHDOG")', source)
        ctor = source[source.index('MagGraphicsTraceScope::MagGraphicsTraceScope('):source.index('MagGraphicsTraceScope::~MagGraphicsTraceScope(')]
        dtor = source[source.index('MagGraphicsTraceScope::~MagGraphicsTraceScope('):]
        self.assertIn('if(job_wait_threshold_ms){', ctor)
        self.assertIn('if(job_wait_threshold_ms){', dtor)
        # The watchdog runs before the capture early return, so it works without critical tracing.
        self.assertLess(dtor.index('job_wait_leave('), dtor.index('if(!capture)return;'))


if __name__ == '__main__':
    unittest.main()
