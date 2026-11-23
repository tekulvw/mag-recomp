"""The session performance monitor (src/mag_session_monitor.h): its counters, percentiles,
per-minute series, the summary JSON, and the overhead of its hot path. Synthetic frame times only."""
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


@unittest.skipUnless(COMPILER, 'C++ compiler')
class SessionMonitorTests(unittest.TestCase):
    def test_counters_summary_and_overhead(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'session_monitor'
            build = subprocess.run([COMPILER, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                    str(ROOT / 'tests/session_monitor.cpp'), str(ROOT / 'src/mag_session_monitor.cpp'),
                                    '-o', str(binary), '-pthread'], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            scratch = pathlib.Path(tmp) / 'scratch'; scratch.mkdir()
            result = subprocess.run([str(binary), str(scratch)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[session_monitor] PASS', result.stdout)
            bench = re.search(r'frame_ns=([\d.]+) presented_ns=([\d.]+)', result.stdout)
            self.assertIsNotNone(bench)
            print(f'\nsession monitor hot path: {bench[1]} ns per frame, {bench[2]} ns with the clock read')

            model = json.loads((scratch / 'model.json').read_text())
            self.assertEqual(model['schema_version'], 1)
            self.assertEqual(model['state'], 'ended')
            frames = model['frames']
            self.assertEqual(frames['count'], 1033)
            self.assertEqual(frames['slow'], {'over_33ms': 33, 'over_50ms': 32, 'over_100ms': 2, 'over_250ms': 2})
            self.assertEqual(sum(count for _, count in frames['histogram_ms']), frames['count'])
            self.assertLessEqual(frames['p50_ms'], frames['p95_ms'])
            self.assertLessEqual(frames['p95_ms'], frames['p99_ms'])
            self.assertLessEqual(frames['p99_ms'], frames['max_ms'])
            self.assertEqual(len(model['hitches']), 16)
            self.assertEqual([h['ms'] for h in model['hitches'][:3]], [300.0, 300.0, 60.0])
            self.assertEqual(set(model['phase_seconds']), {'starting', 'frontend', 'menu', 'match'})
            self.assertTrue(model['audio']['available'])
            self.assertEqual(model['audio']['drift_ms'], 2.0)
            self.assertLess(len(json.dumps(model)), 16 * 1024)

            live = json.loads((scratch / 'session-engine.json').read_text())
            self.assertEqual((live['role'], live['state'], live['ending']), ('engine', 'ended', 'window_closed'))
            self.assertEqual(live['frames']['count'], 4)
            self.assertFalse(list(scratch.glob('*.tmp')))

    def test_frame_clock_feeds_the_monitor(self):
        main = (ROOT / 'src/main.cpp').read_text()
        self.assertIn('mag_session_monitor_start(', main)
        present = main[main.index('static void present_guest_frame'):main.index('static DWORD WINAPI frame_clock')]
        self.assertIn('mag_session_frame_presented();', present)
        # The window-close path ends with _Exit, which skips atexit: the final summary is written first.
        close = main[main.index('if (pump_result != 0)'):]
        self.assertLess(close.index('mag_session_monitor_finish("window_closed")'), close.index('_Exit(0)'))
        self.assertIn('src/mag_session_monitor.cpp', (ROOT / 'CMakeLists.txt').read_text())


if __name__ == '__main__':
    unittest.main()
