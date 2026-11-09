"""The opt-in payload-override seam: guards only the original payload call, composes with the trace hooks, records itself."""
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import tools.payload_override_hooks as hooks
from tools.graphics_payload_hooks import instrument_source as trace_instrument

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ("void mag_wws_policy_spu_func_00003190(spu_context* ctx) {\n"
            " ctx->host_depth++; spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260); ctx->host_depth--;\n}\n")
PIN = hashlib.sha256(ORIGINAL.rstrip('\n').encode()).hexdigest()


class PayloadOverrideHooks(unittest.TestCase):
    def test_guards_the_original_call_only(self):
        new = hooks.instrument_source(ORIGINAL, PIN)
        self.assertIn(hooks.GUARD_OPEN + hooks.CALL + hooks.GUARD_CLOSE, new)
        self.assertEqual(new.count('spu_indirect_branch'), 1)
        self.assertTrue(new.startswith(hooks.DECLARATION))
        self.assertEqual(new, hooks.instrument_source(new, PIN))

    def test_after_trace_hooks(self):
        traced = trace_instrument(ORIGINAL, PIN)
        new = hooks.instrument_source(traced, PIN)
        self.assertLess(new.index('mag_graphics_payload_begin'), new.index(hooks.GUARD_OPEN))
        self.assertGreater(new.index('mag_graphics_payload_end'), new.index(hooks.GUARD_CLOSE + ' '))
        self.assertEqual(hooks.original_body(new[new.index('void '):].rstrip('\n')), ORIGINAL.rstrip('\n'))

    def test_mismatch_closed(self):
        with self.assertRaises(ValueError):
            hooks.instrument_source(ORIGINAL.replace('0x3260', '0x3264'), PIN)
        with self.assertRaises(ValueError):
            hooks.instrument_source('void mag_wws_policy_spu_func_00003190(spu_context* ctx) {\n return;\n}', None)

    def test_apply_records_receipt_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); (directory / 'mag_wws_policy').mkdir()
            path = directory / 'mag_wws_policy/spu_recomp.c'; path.write_text(ORIGINAL)
            record = dict(status='complete', policy_sha256=hooks.POLICY_SHA,
                          outputs={'mag_wws_policy/spu_recomp.c': hashlib.sha256(path.read_bytes()).hexdigest()})
            (directory / 'provenance.json').write_text(json.dumps(record))
            with mock.patch.object(hooks, 'FUNCTION_SHA', PIN):
                first = hooks.apply_hooks(directory)
                self.assertEqual(first['payload_override_hooks']['call_pc'], '0x325C')
                self.assertEqual(first['outputs']['mag_wws_policy/spu_recomp.c'],
                                 hashlib.sha256(path.read_bytes()).hexdigest())
                text = path.read_text()
                self.assertEqual(hooks.apply_hooks(directory), first)
                self.assertEqual(path.read_text(), text)

    @unittest.skipUnless(shutil.which('clang++') and shutil.which('clang'), 'clang required')
    def test_seam_runs_or_skips_the_lifted_job(self):
        stub = ('typedef struct spu_context { unsigned pc; int host_depth; } spu_context;\n'
                'extern int lifted_runs;\n'
                'static void spu_indirect_branch(spu_context* ctx) { (void)ctx; lifted_runs++; }\n'
                'static void spu_drain_call(spu_context* ctx, unsigned pc) { ctx->pc = pc; }\n')
        policy = hooks.instrument_source(ORIGINAL, PIN).replace(hooks.DECLARATION, hooks.DECLARATION + stub, 1)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'policy.c').write_text(policy)
            binary = tmp / 'seam'
            subprocess.run(['clang', '-c', '-std=c11', '-Wall', '-Werror', str(tmp / 'policy.c'), '-o', str(tmp / 'policy.o')],
                           check=True, capture_output=True, text=True)
            subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                            str(ROOT / 'src/mag_extension.cpp'), str(ROOT / 'tests/fixtures/payload_override_seam.cpp'),
                            str(tmp / 'policy.o'), '-o', str(binary)], check=True, capture_output=True, text=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('PASS payload override seam', run.stdout)


if __name__ == '__main__':
    unittest.main()
