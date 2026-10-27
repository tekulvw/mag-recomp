import importlib.util
import unittest
import sys
import time


class EdgeQueueProbeTests(unittest.TestCase):
    def module(self):
        self.assertIsNotNone(importlib.util.find_spec('tools.probe_edge_queue'),
                             'The bounded real EDGE queue runner is required')
        from tools import probe_edge_queue
        return probe_edge_queue

    def test_success_requires_all_jobs_and_real_oracles(self):
        mod = self.module()
        good = 'EDGE_QUEUE_PASS jobs=96 producers=2 bytes=393216 counters=0 masks=1,2 wraps=8\n'
        self.assertTrue(mod.completed(good, 0))
        for text, code in [(good, 1), ('', 0), (good.replace('96', '95'), 0),
                           (good.replace('counters=0', 'counters=1'), 0),
                           (good.replace('masks=1,2', 'masks=1,1'), 0)]:
            self.assertFalse(mod.completed(text, code))

    def test_environment_drops_runtime_overrides(self):
        mod = self.module()
        env = mod.clean_environment({'PATH':'/bin', 'LBP_WS_DRAIN':'1',
            'SPU_SERIAL':'1', 'YDKJ_MINIMAL_CTX':'1', 'MAG_TRACE_QUEUE':'1',
            'PS3RECOMP_STRICT_HLE':'0', 'DYLD_INSERT_LIBRARIES':'bad'})
        self.assertEqual(env, {'PATH':'/bin', 'PS3RECOMP_STRICT_HLE':'1'})

    def test_real_subprocess_deadline_and_output_limit(self):
        mod=self.module()
        with self.assertRaises(TimeoutError):
            mod.bounded_run([sys.executable,'-c','import time; time.sleep(10)'],
                            time.monotonic()+.1,1024)
        with self.assertRaisesRegex(RuntimeError,'log bound'):
            mod.bounded_run([sys.executable,'-c','print("x"*2048)'],
                            time.monotonic()+2,1024)
