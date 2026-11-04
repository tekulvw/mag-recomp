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


def brz(pc, target, rt=0):
    return ri16(0x040, (target - pc) >> 2, rt)


def binz(ra, rt=0):
    return (0x129 << 21) | (ra << 7) | rt


# Encoders for the instructions the static discovery tests use (SPU ISA formats).
def ila(rt, imm):
    return (0x21 << 25) | ((imm & 0x3FFFF) << 7) | rt


def il(rt, imm):
    return ri16(0x081, imm, rt)


def rr(op11, rt, ra, rb=0):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


def ri10(op8, rt, ra, imm):
    return (op8 << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt


def bi(ra):
    return rr(0x1A8, 0, ra)


def bisl(rt, ra):
    return rr(0x1A9, rt, ra)


def a_(rt, ra, rb):
    return rr(0x0C0, rt, ra, rb)


def sf(rt, ra, rb):
    return rr(0x040, rt, ra, rb)


def lqd(rt, ra, offset):
    return ri10(0x34, rt, ra, offset >> 4)


def lqx(rt, ra, rb):
    return rr(0x1C4, rt, ra, rb)


def rotqby(rt, ra, rb):
    return rr(0x1DC, rt, ra, rb)


def shli(rt, ra, n):
    return rr(0x07B, rt, ra, n)


def clgti(rt, ra, n):
    return ri10(0x5C, rt, ra, n)


def brnz(pc, rt, target):
    return ri16(0x042, (target - pc) >> 2, rt)


def image(code, size, entry):
    """Module image with `code` ({local-store address: word}) and zeros elsewhere."""
    raw = bytearray(module([], size, entry))
    for addr, word in code.items():
        struct.pack_into('>I', raw, addr - w.LOAD_BASE, word)
    return bytes(raw)


UNDEFINED = 0x9ABCDEF0        # top nibble 9: no SPU instruction encodes this


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

    # ---- static indirect-target discovery ---------------------------------
    def discover(self, code, size=0x1000, entry=0x4030, **kw):
        raw = image(code, size, entry)
        return w.discover_code(raw, [entry], w.LOAD_BASE + w.HEADER, w.LOAD_BASE + size, **kw)

    def test_computed_jump_to_an_ila_constant_is_followed(self):
        code = {0x4030: ila(2, 0x4800), 0x4034: bi(2),
                0x4800: NOP, 0x4804: BI_LR}
        end, targets = self.discover(code)
        self.assertEqual((end, targets), (0x4808, [0x4800]))
        raw = image(code, 0x1000, 0x4030)
        self.assertEqual(w.reachable_end(raw, [0x4030], 0x4030, 0x5000, indirect=False), 0x4038)
        self.assertEqual(w.reachable_end(raw, [0x4030], 0x4030, 0x5000), 0x4808)

    def test_ilhu_iohl_and_ai_build_targets(self):
        code = {0x4030: ri16(0x082, 0, 3), 0x4034: ri16(0x0C1, 0x4900, 3), 0x4038: ri10(0x1C, 3, 3, 8),
                0x403C: bi(3), 0x4908: BI_LR}
        self.assertEqual(self.discover(code), (0x490C, [0x4908]))

    def test_explicit_return_address_in_link_register(self):
        # Position-independent base (brsl to the next word), then a call made as
        # a jump with $r0 = return point + base: the callee returns there.
        code = {0x4030: ila(2, 0x4038), 0x4034: brsl(0x4034, 0x4038) | 126,
                0x4038: sf(126, 2, 126),
                0x403C: brsl(0x403C, 0x4600),            # ABI: r126 survives
                0x4040: ila(0, 0x4A00), 0x4044: a_(0, 0, 126), 0x4048: br(0x4048, 0x4600),
                0x4600: BI_LR,
                0x4A00: NOP, 0x4A04: BI_LR}
        end, targets = self.discover(code)
        self.assertEqual((end, targets), (0x4A08, [0x4A00]))

    def test_volatile_registers_do_not_survive_a_call(self):
        code = {0x4030: ila(5, 0x4A00), 0x4034: ila(85, 0x4B00), 0x4038: brsl(0x4038, 0x4600),
                0x403C: binz(85), 0x4040: bi(5),
                0x4600: il(5, 0), 0x4604: BI_LR,
                0x4A00: BI_LR, 0x4B00: BI_LR}
        self.assertEqual(self.discover(code), (0x4B04, [0x4B00]))

    def test_constant_address_pointer_load_feeds_indirect_call(self):
        # Constructor-table shape: lqd/rotqby of a word at a known address.
        code = {0x4030: ila(80, 0x4F04), 0x4034: lqd(7, 80, 0), 0x4038: rotqby(3, 7, 80),
                0x403C: bisl(0, 3), 0x4040: BI_LR,
                0x4C00: NOP, 0x4C04: BI_LR,
                0x4F04: 0x4C00}
        self.assertEqual(self.discover(code), (0x4C08, [0x4C00]))

    def jump_table(self, entries, bound, relative=False):
        """0x4030 jumps to a bounded jump table at 0x4040 whose table is at 0x4E00."""
        base = 0x4E00
        code = {0x4030: br(0x4030, 0x4040),
                0x4040: ila(10, base), 0x4044: shli(8, 6, 2), 0x4048: clgti(5, 6, bound),
                0x404C: a_(3, 8, 10), 0x4050: lqx(7, 10, 8), 0x4054: rotqby(2, 7, 3),
                0x4058: a_(2, 2, 10) if relative else NOP,
                0x405C: brnz(0x405C, 5, 0x4100), 0x4060: bi(2),
                0x4100: BI_LR}
        for i, target in enumerate(entries):
            code[base + 4 * i] = (target - base) & 0xFFFFFFFF if relative else target
            code.setdefault(target, BI_LR)
        return code

    def test_bounded_jump_table_takes_every_entry(self):
        cases = [0x4400, 0x4500, 0x4600]
        for relative in (False, True):
            with self.subTest(relative=relative):
                self.assertEqual(self.discover(self.jump_table(cases, 2, relative)), (0x4604, cases))

    def test_jump_table_bound_limits_entries(self):
        # A further valid address past the bound is not an entry.
        code = self.jump_table([0x4400, 0x4500], 1)
        code.update({0x4E08: 0x4700, 0x4700: BI_LR})
        self.assertEqual(self.discover(code), (0x4504, [0x4400, 0x4500]))

    def test_jump_table_with_an_invalid_entry_is_rejected_whole(self):
        code = self.jump_table([0x4400, 0x4500], 1)
        code[0x4500] = UNDEFINED
        self.assertEqual(self.discover(code), (0x4104, []))

    def test_jump_table_without_bound_check_is_not_taken(self):
        code = self.jump_table([0x4400, 0x4500], 1)
        code[0x405C] = NOP
        self.assertEqual(self.discover(code), (0x4064, []))

    def test_jump_table_entered_mid_sequence_is_not_taken(self):
        # A second path joins at the lqx, skipping the shift and the bound check.
        code = self.jump_table([0x4400, 0x4500], 1)
        code.update({0x4030: brz(0x4030, 0x4800, 9), 0x4034: br(0x4034, 0x4040),
                     0x4800: ila(10, 0x4E00), 0x4804: br(0x4804, 0x4050)})
        end, targets = self.discover(code)
        self.assertEqual((end, targets), (0x4808, []))

    def test_target_into_data_is_rejected(self):
        for bad in ({0x4800: UNDEFINED}, {0x4800: NOP}, {0x4800: NOP, 0x4804: brsl(0x4804, 0x4900), 0x4808: BI_LR,
                                                      0x4900: UNDEFINED}):
            with self.subTest(bad=bad):
                code = {0x4030: ila(2, 0x4800), 0x4034: bi(2), **bad}
                # zeros after the NOP, an undefined word, or an invalid callee
                self.assertEqual(self.discover(code), (0x4038, []))

    def test_target_over_loaded_data_is_rejected(self):
        # The code loads the quadword at 0x4800 as data; a jump there is refused.
        code = {0x4030: ila(4, 0x4800), 0x4034: lqd(5, 4, 0), 0x4038: ila(2, 0x4804), 0x403C: bi(2),
                0x4800: 0x12345678, 0x4804: NOP, 0x4808: BI_LR}
        self.assertEqual(self.discover(code), (0x4040, []))

    def test_constants_not_used_as_branch_targets_are_ignored(self):
        code = {0x4030: ila(3, 0x4800), 0x4034: ila(0, 0x4900), 0x4038: lqd(0, 1, 0x10), 0x403C: br(0x403C, 0x4600),
                0x4600: BI_LR, 0x4800: NOP, 0x4804: BI_LR, 0x4900: NOP, 0x4904: BI_LR}
        self.assertEqual(self.discover(code), (0x4604, []))

    def test_target_out_of_module_or_misaligned_is_ignored(self):
        for target in (0x30000, 0x4802, 0x4010):
            with self.subTest(target=target):
                code = {0x4030: ila(2, target), 0x4034: bi(2), 0x4800: NOP, 0x4804: BI_LR}
                self.assertEqual(self.discover(code), (0x4038, []))

    def test_static_and_measured_entries_combine(self):
        code = {0x4030: ila(2, 0x4800), 0x4034: bi(2), 0x4800: BI_LR, 0x4C00: NOP, 0x4C04: BI_LR}
        raw = image(code, 0x1000, 0x4030)
        plans = w.module_plans(raw, [0x4030, 0x4C00], 0x4030, 0x5000)
        self.assertEqual(plans, [('static', 0x4C08, [0x4800]), ('direct', 0x4C08, [])])
        self.assertEqual(w.module_plans(raw, [0x4030], 0x4030, 0x5000, direct_only=True),
                         [('direct', 0x4038, [])])
        # No static gain: only the direct bound is offered.
        plain = image({0x4030: NOP, 0x4034: BI_LR}, 0x1000, 0x4030)
        self.assertEqual(w.module_plans(plain, [0x4030], 0x4030, 0x5000), [('direct', 0x4038, [])])

    def test_data_code_pointers_are_opt_in(self):
        # 0x4400 starts a function (after `bi $0` and lnop padding) inside the
        # lifted interval that only a data word names; its callee lies past the
        # bound. A word naming 0x4404 (mid-function) or 0x4A00 (past the bound)
        # is not a candidate.
        code = {0x4030: brsl(0x4030, 0x4600), 0x4034: BI_LR, 0x4600: BI_LR,
                0x43F8: BI_LR, 0x43FC: 0x00200000,
                0x4400: il(3, 1), 0x4404: brsl(0x4404, 0x4A00), 0x4408: BI_LR, 0x4A00: BI_LR,
                0x4F00: 0x4400, 0x4F04: 0x4404, 0x4F08: 0x4A00}
        raw = image(code, 0x1000, 0x4030)
        self.assertEqual(w.data_code_pointers(raw, 0x4030, 0x4604), [0x4400])
        self.assertEqual(w.module_plans(raw, [0x4030], 0x4030, 0x5000), [('direct', 0x4604, [])])
        self.assertEqual(w.module_plans(raw, [0x4030], 0x4030, 0x5000, data_pointers=True),
                         [('static+data', 0x4A04, [0x4400]), ('direct', 0x4604, [])])
        # A pointed-to function whose code is not valid is refused.
        code[0x4408] = UNDEFINED
        raw = image(code, 0x1000, 0x4030)
        self.assertEqual(w.module_plans(raw, [0x4030], 0x4030, 0x5000, data_pointers=True),
                         [('direct', 0x4604, [])])

    def test_is_defined_rejects_unencoded_words(self):
        for word in (NOP, BI_LR, ila(1, 2), lqx(1, 2, 3), 0x80000000, 0):
            self.assertTrue(w.is_defined(word), hex(word))
        for word in (UNDEFINED, 0xA0000000, 0x3D800000, 0x40000000):
            self.assertFalse(w.is_defined(word), hex(word))


if __name__ == '__main__':
    unittest.main()
