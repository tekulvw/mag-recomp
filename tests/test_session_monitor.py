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


STUB_BUILD_STATS_HEADER = r'''
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS3RECOMP_RSX_BUILD_STATS_VERSION 1
#define RSX_BUILD_FLAG_SPIRV_DISK_CACHE 0x1u
#define RSX_BUILD_FLAG_VK_PIPELINE_CACHE 0x2u
struct ps3recomp_rsx_build_stats {
    uint32_t version, flags;
    uint64_t programs, program_ns, shader_translations, shader_translation_ns, shader_memory_hits, shader_disk_hits;
    uint64_t pipelines, pipeline_ns, pipeline_worst_ns;
    uint64_t texture_uploads, texture_upload_bytes, texture_upload_ns, slow_texture_uploads;
    uint64_t pipeline_cache_loaded_bytes, pipeline_cache_saves, pipeline_cache_save_ns;
    char backend[8];
};
int ps3recomp_rsx_build_stats(struct ps3recomp_rsx_build_stats *out);
#ifdef __cplusplus
}
#endif
'''
STUB_BUILD_STATS_GETTER = r'''
#include <string.h>
#include "ps3emu/rsx_build_stats.h"
static uint64_t calls;
int ps3recomp_rsx_build_stats(struct ps3recomp_rsx_build_stats *out) {
    memset(out, 0, sizeof *out);
    out->version = 1; out->flags = RSX_BUILD_FLAG_SPIRV_DISK_CACHE;
    out->programs = ++calls; out->program_ns = calls * 1000000;
    out->pipeline_cache_loaded_bytes = 4096;
    memcpy(out->backend, "stub", 5);
    return 0;
}
'''


STUB_FS_OBSERVER_HEADER = r'''
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS3RECOMP_FS_OBSERVER_VERSION 1
enum ps3recomp_fs_event_kind { PS3RECOMP_FS_OPEN = 1 };
struct ps3recomp_fs_event { uint32_t version; uint32_t kind; int32_t fd; int32_t error; const char *path; };
typedef void (*ps3recomp_fs_observer_fn)(const struct ps3recomp_fs_event *event);
ps3recomp_fs_observer_fn ps3recomp_fs_set_observer(ps3recomp_fs_observer_fn fn);
#ifdef __cplusplus
}
#endif
'''
STUB_FS_OBSERVER = r'''
#include "ps3emu/fs_host_observer.h"
static ps3recomp_fs_observer_fn installed;
ps3recomp_fs_observer_fn ps3recomp_fs_set_observer(ps3recomp_fs_observer_fn fn) {
    ps3recomp_fs_observer_fn previous = installed; installed = fn; return previous;
}
/* The test's stand-in for a guest open: what the runtime reports after cellFsOpen. */
void stub_fs_open(const char *path, int fd) {
    struct ps3recomp_fs_event e = {PS3RECOMP_FS_OBSERVER_VERSION, PS3RECOMP_FS_OPEN, fd, fd < 0 ? -1 : 0, path};
    if (installed) installed(&e);
}
'''


@unittest.skipUnless(COMPILER, 'C++ compiler')
class SessionMonitorTests(unittest.TestCase):
    def test_counters_summary_and_overhead(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'session_monitor'
            build = subprocess.run([COMPILER, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                    str(ROOT / 'tests/session_monitor.cpp'), str(ROOT / 'src/mag_session_monitor.cpp'),
                                    str(ROOT / 'src/mag_session_network.cpp'), str(ROOT / 'src/mag_session_host.cpp'),
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

            # First-use builds (tests/session_monitor.cpp, builds()): per hitch, per minute and in total.
            builds = json.loads((scratch / 'builds.json').read_text())
            section = builds['builds']
            self.assertEqual((section['available'], section['backend'], section['vk_pipeline_cache'], section['spirv_disk_cache']),
                             (True, 'vulkan', True, False))
            self.assertEqual((section['pipelines'], section['uploads']), (3, 3))
            self.assertEqual(section['slow_frames']['over_250ms'], {'frames': 1, 'frame_ms': 300, 'with_builds': 1, 'build_ms': 280,
                                                                    'with_uploads': 0, 'upload_ms': 0})
            worst = builds['hitches'][0]['builds']
            self.assertEqual((worst['build_ms'], worst['programs'], worst['shaders'], worst['pipelines']), (280.0, 2, 4, 2))
            self.assertEqual(builds['hitches'][1]['builds']['upload_mb'], 6.0)
            self.assertEqual(builds['minutes'][0]['builds'], {'build_ms': 285, 'shaders': 4, 'pipelines': 3, 'upload_ms': 90, 'uploads': 3,
                                                       'slow_with_builds': 1})
            self.assertEqual(builds['timeline'][0]['build_ms'], 280)
            self.assertFalse(model['builds']['available'])

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
            self.assertEqual(live['maps'], {'available': False})
            self.assertFalse(list(scratch.glob('*.tmp')))

    def test_runtime_build_counters_reach_the_summary(self):
        # A stand-in for the runtime's ps3emu/rsx_build_stats.h (same struct and getter): the
        # process-wide monitor reads it at each present and in each summary.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / 'ps3emu').mkdir()
            (tmp / 'ps3emu/rsx_build_stats.h').write_text(STUB_BUILD_STATS_HEADER)
            (tmp / 'ps3emu/fs_host_observer.h').write_text(STUB_FS_OBSERVER_HEADER)
            (tmp / 'stub.c').write_text(STUB_BUILD_STATS_GETTER + STUB_FS_OBSERVER.replace('#include "ps3emu/fs_host_observer.h"', ''))
            cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
            obj = tmp / 'stub.o'
            build = subprocess.run([cc, '-c', '-I' + str(tmp), '-include', str(tmp / 'ps3emu/fs_host_observer.h'), str(tmp / 'stub.c'),
                                    '-o', str(obj)], capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stderr)
            binary = tmp / 'session_monitor'
            build = subprocess.run([COMPILER, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-DMAG_TEST_FS_STUB=1',
                                    '-I' + str(ROOT / 'src'), '-I' + str(tmp),
                                    str(ROOT / 'tests/session_monitor.cpp'), str(ROOT / 'src/mag_session_monitor.cpp'),
                                    str(ROOT / 'src/mag_session_network.cpp'), str(ROOT / 'src/mag_session_host.cpp'), str(obj), '-o', str(binary), '-pthread'],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            scratch = tmp / 'scratch'; scratch.mkdir()
            result = subprocess.run([str(binary), str(scratch)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            live = json.loads((scratch / 'session-engine.json').read_text())['builds']
            self.assertEqual((live['available'], live['backend'], live['spirv_disk_cache'], live['vk_pipeline_cache']),
                             (True, 'stub', True, False))
            self.assertGreater(live['programs'], 0)
            self.assertEqual(live['pipeline_cache_loaded_bytes'], 4096)
            # The file observer: a map's archive opened in the menu is the session's map load.
            maps = json.loads((scratch / 'session-engine.json').read_text())['maps']
            self.assertEqual(maps['available'], True)
            self.assertEqual([load['map'] for load in maps['loads']], ['mag_test_map'])

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
