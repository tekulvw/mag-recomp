"""The match stall reporter (src/mag_stall_reporter.h): when to report, what a report says, and the
real thread against a synthetic clock and probe. No game files."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


@unittest.skipUnless(COMPILER, 'C++ compiler')
class StallReporterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.tmp.name) / 'stall_reporter'
        build = subprocess.run([COMPILER, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                str(ROOT / 'tests/stall_reporter.cpp'), str(ROOT / 'src/mag_stall_reporter.cpp'),
                                '-o', str(cls.binary), '-pthread'], capture_output=True, text=True, timeout=120)
        assert build.returncode == 0, build.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_decisions_and_text(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('[stall_reporter] PASS', result.stdout)

    def test_live_thread_reports_a_frozen_match(self):
        env = dict(os.environ, MAG_STALL_REPORT_SECONDS='1,2')
        result = subprocess.run([str(self.binary), 'live'], capture_output=True, text=True, timeout=60, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        err = result.stderr
        first = err.index('[mag-stall] no guest frame for ')
        self.assertIn('(report 1)', err[first:first + 120])
        self.assertIn('(report 2)', err)
        self.assertIn('[mag-stall] guest-tid 3 inside firmware call sys_lwcond_wait', err)
        self.assertIn('[mag-stall] audio periods 1', err)
        self.assertEqual(err.count('[WATCHDOG]   guest-tid 3 lr=0x00010000'), 2)
        self.assertIn('[mag-stall] guest frames resumed after ', err)
        self.assertLess(err.index('(report 2)'), err.index('resumed after'))

    def test_off_writes_nothing(self):
        env = dict(os.environ, MAG_STALL_REPORT_SECONDS='off')
        result = subprocess.run([str(self.binary), 'live'], capture_output=True, text=True, timeout=60, env=env)
        self.assertNotIn('[mag-stall]', result.stderr)
        self.assertIn('listings=0', result.stdout)

    def test_built_into_the_engine(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('src/mag_stall_reporter.cpp', cmake)
        self.assertIn('src/mag_stall_probe.cpp', cmake)
        main = (ROOT / 'src/main.cpp').read_text()
        self.assertIn('mag_stall_reporter_start()', main)


if __name__ == '__main__':
    unittest.main()
