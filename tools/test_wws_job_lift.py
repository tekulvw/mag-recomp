#!/usr/bin/env python3
"""Synthetic checks for tools/wws_job_lift.py; no title bytes."""
import os, pathlib, shutil, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wws_job_lift as w
from verified_spu_jobs import executable_sections

NOP = 0x40200000          # nop (RR)
BI_LR = 0x35000000        # bi $0


def ri16(op9, i16, rt=0):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def br(pc, target):
    return ri16(0x064, (target - pc) >> 2)


def brsl(pc, target):
    return ri16(0x066, (target - pc) >> 2)


def brz(pc, target):
    return ri16(0x040, (target - pc) >> 2)


def module(words, size, entry):
    """Header + code words at 0x4030 + zero-filled data tail."""
    header = bytearray(0x30)
    struct.pack_into('>II', header, 0x10, entry - w.LOAD_BASE, size)
    header[0x20:0x24] = w.MAGIC
    struct.pack_into('>II', header, 0x28, w.LOAD_BASE, 5)
    code = b''.join(struct.pack('>I', x) for x in words)
    return bytes(header) + code + bytes(size - len(header) - len(code))


def fake_elf64(payload_va, payload):
    """Big-endian ELF64 with one PT_LOAD holding `payload` at `payload_va`."""
    eh = bytearray(64)
    eh[:6] = b'\x7fELF\x02\x02'
    struct.pack_into('>QHH', eh, 0x20, 64, 0, 0)
    struct.pack_into('>HH', eh, 0x36, 56, 1)
    ph = struct.pack('>IIQQQQQQ', 1, 5, 0x100, payload_va, payload_va, len(payload), len(payload), 16)
    out = bytearray(eh + ph)
    out += bytes(0x100 - len(out)) + payload
    return bytes(out)


class WwsJobLift(unittest.TestCase):
    def test_find_modules_reads_header_fields(self):
        raw = module([NOP, BI_LR], 0x200, 0x4030)
        pad = bytes(0x40)
        mods = w.find_modules(fake_elf64(0x10000, pad + raw + bytes(0x80)))
        self.assertEqual(len(mods), 1)
        m = mods[0]
        self.assertEqual((m['ea'], m['entry'], m['size']), (0x10040, 0x4030, 0x200))
        self.assertEqual(m['raw'], raw)

    def test_find_modules_rejects_misaligned_or_truncated(self):
        raw = module([NOP], 0x200, 0x4030)
        self.assertEqual(w.find_modules(fake_elf64(0x10000, bytes(8) + raw)), [])  # misaligned
        self.assertEqual(w.find_modules(fake_elf64(0x10000, raw[:0x100])), [])     # image past segment

    def test_reachability_follows_calls_jumps_and_conditionals(self):
        base = 0x4030
        words = [0] * 32
        pc = lambda i: base + 4 * i
        words[0] = brsl(pc(0), pc(10))      # call -> 10, continue at 1
        words[1] = brz(pc(1), pc(20))       # conditional -> 20, continue at 2
        words[2] = BI_LR                     # return: stop
        words[10] = br(pc(10), pc(12))      # jump over 11
        words[11] = 0xDEADBEEF               # data, unreachable
        words[12] = BI_LR
        words[20] = NOP
        words[21] = BI_LR
        raw = module(words, 0x400, base)
        end = w.reachable_end(raw, [base], base, w.LOAD_BASE + 0x400)
        self.assertEqual(end, pc(21) + 4)
        # Measured indirect entries extend coverage beyond direct reachability.
        words[30] = BI_LR
        raw = module(words, 0x400, base)
        self.assertEqual(w.reachable_end(raw, [base, pc(30)], base, w.LOAD_BASE + 0x400), pc(30) + 4)

    def test_reachability_ignores_out_of_range_targets(self):
        base = 0x4030
        raw = module([br(base, 0x30000)], 0x100, base)
        self.assertEqual(w.reachable_end(raw, [base], base, w.LOAD_BASE + 0x100), base + 4)

    def test_wrapper_exposes_exactly_the_code_interval(self):
        raw = module([NOP, NOP, BI_LR], 0x300, 0x4038)
        elf = w.wrapper_elf(raw, 0x403C, 0x4038)
        self.assertEqual(executable_sections(elf), [(0x4030, raw[0x30:0x3C])])
        self.assertEqual(struct.unpack_from('>I', elf, 24)[0], 0x4038)

    def test_registry_registers_each_module_behind_toggle(self):
        raw = module([NOP, BI_LR], 0x200, 0x4030)
        mods = [dict(ea=0x1234500, size=0x200, raw=raw, code_end=0x4038, image_id=200, prefix='p_')]
        src = w.emit_registry(mods)
        self.assertIn('getenv("MAG_COMPILED_JOBS")', src)
        self.assertIn('spu_overlay_register_verified_region(0x01234500u, 0x4000u, 0x38u, 200, &seg, 1)', src)
        self.assertIn('{0x4030u, 8u, job_200_code}', src)
        self.assertIn('abort()', src)
        self.assertLess(src.index('return;'), src.index('p_spu_recomp_register()'))

    def test_generated_constructor_defaults_on_and_honors_explicit_disable(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler unavailable')
        raw = module([NOP, BI_LR], 0x200, 0x4030)
        mods = [dict(ea=0x1234500, size=0x200, raw=raw, code_end=0x4038,
                     image_id=200, prefix='p_')]
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root/'spu_overlay_residency.h').write_text('''#include <stdint.h>
typedef struct { uint32_t lsa,size; const uint8_t* bytes; } spu_overlay_code_segment;
int spu_overlay_register_verified_region(uint32_t,uint32_t,uint32_t,int,const spu_overlay_code_segment*,int);
''')
            (root/'registry.c').write_text(w.emit_registry(mods))
            (root/'main.c').write_text('''#include "spu_overlay_residency.h"
#include <stdio.h>
#include <stdlib.h>
static int compiled, regions;
void spu_begin_image(int image) { if(image!=0 && image!=200) abort(); }
void p_spu_recomp_register(void) { ++compiled; }
int spu_overlay_register_verified_region(uint32_t ea,uint32_t base,uint32_t bytes,int image,const spu_overlay_code_segment* seg,int count) {
 if(ea!=0x1234500 || base!=0x4000 || bytes!=0x38 || image!=200 || count!=1 || seg->lsa!=0x4030 || seg->size!=8 || seg->bytes[0]!=0x40) abort();
 ++regions; return 1;
}
int main(void) { printf("%d %d\\n",compiled,regions); return 0; }
''')
            exe = root/'registry-test'
            result = subprocess.run([compiler, '-I'+str(root), str(root/'registry.c'),
                                     str(root/'main.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for setting, expected in [(None,'1 1'),('1','1 1'),('0','0 0'),('invalid','0 0')]:
                env = dict(os.environ); env.pop('MAG_COMPILED_JOBS', None)
                if setting is not None: env['MAG_COMPILED_JOBS'] = setting
                with self.subTest(setting=setting):
                    p = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertEqual(p.stdout.strip(), expected)

    def test_required_coverage_rejects_any_interpreted_module(self):
        w.require_complete([dict(image_id=200,status='lifted')])
        with self.assertRaisesRegex(ValueError, '201'):
            w.require_complete([dict(image_id=200,status='lifted'),
                                dict(image_id=201,status='interpreted',reason='lifter failed')])

    def test_attribution_entries_group_by_module_hash(self):
        cols = 'wall_ns\tsteps\tentries\treason\timage\tpolicy\tovl\ttask\tcode_image\tcode_src_ea\tcode_size\tentry\tls1330\tentry_hash\tls4000_hash\tfirst16'
        rows = ['1\t1\t1\tunlifted\t101\t1\t0\t0\t0\t0x0\t0x0\t0x04100\t0x0\taa\t00000000000000ff\t00',
                '1\t1\t1\tunlifted\t101\t1\t0\t0\t0\t0x0\t0x0\t0x04200\t0x0\tbb\t00000000000000ff\t00']
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / 'a.tsv'
            p.write_text('# spu-interp-attribution v1\nelapsed_ns=1\n' + cols + '\n' + '\n'.join(rows) + '\n')
            self.assertEqual(w.attribution_entries([p]), {'00000000000000ff': {0x4100, 0x4200}})

    def test_fnv64_matches_sdk_attribution_hash(self):
        # The SDK interpreter attribution report seeds with 1469598103934665603.
        self.assertEqual(w.fnv64(b''), 1469598103934665603)
        self.assertEqual(w.fnv64(b'a'), ((1469598103934665603 ^ 0x61) * 1099511628211) & (2**64 - 1))


if __name__ == '__main__':
    unittest.main()
