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
                                    str(ROOT / 'src/mag_session_network.cpp'),
                                    '-o', str(binary), '-pthread'], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            scratch = pathlib.Path(tmp) / 'scratch'; scratch.mkdir()
            result = subprocess.run([str(binary), str(scratch)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[session_monitor] PASS', result.stdout)
            bench = re.search(r'frame_ns=([\d.]+) presented_ns=([\d.]+) socket_ns=([\d.]+)', result.stdout)
            self.assertIsNotNone(bench)
            print(f'\nsession monitor hot path: {bench[1]} ns per frame, {bench[2]} ns with the clock read, '
                  f'{bench[3]} ns per socket call')

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

            # Network health: a synthetic socket stream with injected silences beside a frame stream
            # with injected hitches, classified (tests/session_monitor.cpp, network()).
            network = json.loads((scratch / 'network.json').read_text())
            self.assertTrue(network['network']['available'])
            stream = next(e for e in network['network']['endpoints'] if e['address'] == '192.0.2.10:10473')
            self.assertEqual((stream['transport'], stream['open'], stream['streamed'], stream['gap_events']), ('tcp', True, True, 2))
            self.assertEqual(sum(count for _, count in stream['gaps_ms']), stream['in']['packets'] - 1)
            self.assertEqual(stream['rtt_ms']['samples'], 2)
            self.assertEqual(network['network']['other_endpoints']['count'], 1)
            self.assertNotIn('198.51.100.7', json.dumps(network))
            self.assertEqual([(e['kind'], e['verdict']) for e in network['timeline']],
                             [('frames', 'frames'), ('network', 'network'), ('network', 'both'), ('frames', 'both'),
                              ('network', 'network')])
            self.assertTrue(network['timeline'][-1]['open'])
            self.assertTrue(all(e['phase'] == 'match' for e in network['timeline']))
            self.assertEqual([m['verdict'] for m in network['minutes']], ['ok', 'frames', 'network', 'both'])
            self.assertTrue(all(m['net']['stream'] for m in network['minutes']))
            self.assertLess(len(json.dumps(network)), 16 * 1024)
            self.assertFalse(model['network']['available'])

            # A sustained frame-rate drop (tests/session_monitor.cpp, drops()): 98 fps, then 34 fps
            # for 41 s in a match, with its capture, as the report reads it.
            summary = json.loads((scratch / 'drops.json').read_text())
            drops = summary['fps_drops']
            self.assertEqual((drops['count'], drops['ratio'], drops['floor_fps'], drops['min_s']), (1, 0.6, 60, 3.0))
            [drop] = drops['episodes']
            self.assertEqual((drop['at_s'], drop['s'], drop['phase']), (180.0, 41.0, 'match'))
            self.assertEqual((drop['baseline_fps'], drop['mean_fps'], drop['min_fps'], drop['frames']), (98.0, 34.0, 34.0, 41 * 34))
            self.assertEqual(drop['lost_frames'], (98 - 34) * 41)
            self.assertEqual((drop['capture']['from_s'], drop['capture']['to_s']), (175.0, 226.0))
            self.assertEqual(drop['capture']['frames'], 5 * 98 + 41 * 34 + 5 * 98)
            self.assertAlmostEqual(drop['capture']['p50_ms'], 1000 / 34, delta=0.6)
            self.assertNotIn('open', drop)
            self.assertNotIn('network', drop)  # no socket observer in this run
            self.assertIn('loading', summary['frames'])
            self.assertLess(len(json.dumps(summary)), 16 * 1024)
            self.assertIn('fps_drops', model)
            self.assertNotIn('frozen', model)

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
