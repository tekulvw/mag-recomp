"""Finding MAG's RSX programs in its archives (src/mag_shader_programs.h) and warming the shader
cache from them (src/mag_shader_warm.cpp, docs/shader-warm.md).

Synthetic: the archives, resource blocks, effect records and programs are written here from the
layout the header describes. No game files and no GPU. The warm-up test compiles the tool against
the pinned runtime (MAG_TEST_SDK, or local/ps3recomp-runtime) without glslang, so it checks the
program and variant choice, the names, the workers and pruning, not translation itself."""
import hashlib
import os
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
CC = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
SDK = pathlib.Path(os.environ.get('MAG_TEST_SDK', ROOT / 'local/ps3recomp-runtime'))
POSIX = os.name == 'posix'

MARKER = 0x600DF00D
EFFECT = 0xB66091F2


def fp_word(host):
    """A fragment program word as stored: big-endian with its 16-bit halves swapped."""
    return struct.pack('>I', ((host << 16) | (host >> 16)) & 0xFFFFFFFF)


EXEC_ALWAYS = (7 << 18) | (0 << 21) | (1 << 23) | (2 << 25) | (3 << 27)
SWZ_IDENT_FP = (0 << 9) | (1 << 11) | (2 << 13) | (3 << 15)


def fp_tex(unit, end):
    """TEX r0, TC0 on `unit`."""
    w0 = (0x17 << 24) | (0xF << 9) | (4 << 13) | (unit << 17) | (1 if end else 0)
    return fp_word(w0) + fp_word(1 | SWZ_IDENT_FP | EXEC_ALWAYS) + fp_word(0) + fp_word(0)


def fp_mov_const(end):
    """MOV r0, an inline constant (a type-2 source), followed by the constant."""
    w0 = (0x01 << 24) | (0xF << 9) | (1 if end else 0)
    return (fp_word(w0) + fp_word(2 | SWZ_IDENT_FP | EXEC_ALWAYS) + fp_word(0) + fp_word(0) +
            struct.pack('>4f', 1.0, 0.5, 0.25, 1.0))


SWZ_IDENT_VP = (3 << 8) | (2 << 10) | (1 << 12) | (0 << 14)


def vp_mov(inp, out, end):
    """MOV o[out], v[inp], as stored: four big-endian words."""
    src0 = 2 | SWZ_IDENT_VP
    src1 = 1 | SWZ_IDENT_VP
    d0 = (0x3F << 15) | (1 << 30)
    d1 = (src0 >> 9) | ((inp & 0xF) << 8) | (0x01 << 22)
    d2 = (src1 >> 11) | ((src1 & 0x1FFFF) << 6) | ((src0 & 0x1FF) << 23)
    d3 = (1 if end else 0) | ((out & 0x1F) << 2) | (0x3F << 7) | (0xF << 13) | ((src1 & 0x7FF) << 21)
    return struct.pack('>4I', d0, d1, d2, d3)


VP = vp_mov(0, 0, False) + vp_mov(8, 7, True)          # position and TC0
FP_TWO_UNITS = fp_tex(0, False) + fp_tex(14, True)      # samples units 0 and 14
FP_CONST = fp_mov_const(True)                           # an inline constant, no texture


def vp_header(count):
    return struct.pack('>HHIIHH', count, 0, 0x11, 0x22, 0x0333, 0xFFFF)


def fp_header(size, control, rel):
    return struct.pack('>IIIII', size, 0, control, 0, rel)


def effect_record(techniques, declared=None, extra=0):
    """One shader effect: per technique a VP header and program, then (past `extra` bytes) an FP
    header pointing at its program, which is placed after all techniques."""
    head = bytearray(struct.pack('>I', MARKER) + bytes(0x18) + struct.pack('>I', EFFECT) + bytes(8) +
                     struct.pack('>I', len(techniques) if declared is None else declared) + bytes(4))
    body = bytearray()
    fixups = []
    for vp, fp, control in techniques:
        body += vp_header(len(vp) // 16) + vp + bytes(extra)
        fixups.append((len(head) + len(body), fp, control))
        body += bytes(0x20)                        # the FP header, filled in below
    record = head + body
    for at, fp, control in fixups:
        while len(record) % 16:
            record += b'\0'
        record[at:at + 20] = fp_header(len(fp), control, len(record) - at)
        record += fp
    return bytes(record)


def resource_block(records, types=None):
    types = types if types is not None else [(EFFECT, len(records))]
    table = b''.join(struct.pack('>II', t, n) for t, n in types)
    header = bytearray(0x30)
    header[0:4] = struct.pack('>I', 6)
    header[4:8] = struct.pack('>I', 0x30)
    header[0x2c:0x30] = struct.pack('>I', len(types))
    data = bytes(header) + table
    for r in records:
        data += bytes(6) + r                      # records need not be word-aligned
    return data


def psarc(members, block_size=256, compress=True):
    """A zlib PSARC: member 0 lists the names; each block is stored raw when compression does not
    shrink it (or `compress` is off)."""
    names = '\n'.join(n for n, _ in members).encode()
    contents = [names] + [d for _, d in members]
    width = 1
    while (1 << (8 * width)) < block_size:
        width += 1
    sizes, blobs, entries = [], [], []
    for data in contents:
        first = len(sizes)
        for at in range(0, max(len(data), 1), block_size):
            chunk = data[at:at + block_size]
            z = zlib.compress(chunk, 9)
            stored = z if compress and len(z) < len(chunk) else chunk
            sizes.append(len(stored) % (1 << (8 * width)))   # a full raw block is written as 0
            blobs.append(stored)
        entries.append((first, len(data)))
    toc = 32 + 30 * len(contents) + width * len(sizes)
    offsets, at = [], toc
    for i, (first, _) in enumerate(entries):
        offsets.append(at)
        end = entries[i + 1][0] if i + 1 < len(entries) else len(blobs)
        at += sum(len(b) for b in blobs[first:end])
    out = bytearray(b'PSAR' + struct.pack('>HH', 1, 4) + b'zlib' +
                    struct.pack('>IIIII', toc, 30, len(contents), block_size, 0))
    for i, (first, length) in enumerate(entries):
        out += hashlib.md5(str(i).encode()).digest() + struct.pack('>I', first) + \
            length.to_bytes(5, 'big') + offsets[i].to_bytes(5, 'big')
    for s in sizes:
        out += s.to_bytes(width, 'big')
    for b in blobs:
        out += b
    return bytes(out)


def le_words(be):
    return b''.join(be[i:i + 4][::-1] for i in range(0, len(be), 4))


def compile_scanner(tmp):
    binary = pathlib.Path(tmp) / 'shader_programs'
    build = subprocess.run([COMPILER, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                            str(ROOT / 'tests/shader_programs.cpp'), str(ROOT / 'src/mag_shader_programs.cpp'),
                            str(ROOT / 'src/player_inflate.cpp'), '-o', str(binary)],
                           capture_output=True, text=True, timeout=180)
    if build.returncode:
        raise AssertionError(build.stderr)
    return binary


def game_archive():
    """A game.psarc-like archive: an effect block with two techniques, a block of other records,
    a loose fragment program file, and an effect whose technique count is wrong."""
    good = effect_record([(VP, FP_TWO_UNITS, 0x40), (VP, FP_CONST, 0x02)], extra=16)
    wrong = effect_record([(VP, FP_CONST, 0x40)], declared=2)
    loose = bytes(16) + fp_header(len(FP_TWO_UNITS), 0x00, 0x20) + bytes(12) + FP_TWO_UNITS
    return psarc([
        ('/run/ps3/global_resources/global_shaders.zrb', resource_block([good, wrong])),
        ('/run/ps3/global_resources/textures.zrb', resource_block([], types=[(0x85CE0660, 3)]) + bytes(600)),
        ('/run/ps3/media/shaders/copy.fbin', loose),
        ('/run/ps3/media/readme.txt', b'not a program'),
    ])


class ProgramFinderTests(unittest.TestCase):
    @unittest.skipUnless(COMPILER, 'C++ compiler')
    def test_checks_without_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([str(compile_scanner(tmp))], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[shader_programs] PASS', result.stdout)

    @unittest.skipUnless(COMPILER, 'C++ compiler')
    def test_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            scanner = compile_scanner(tmp)
            archive = pathlib.Path(tmp) / 'game.psarc'
            archive.write_bytes(game_archive())
            for mode in ([str(archive)], ['--memory', str(archive)]):
                result = subprocess.run([str(scanner), *mode], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                lines = result.stdout.splitlines()
                found = [l.split() for l in lines if l.startswith(('vp ', 'fp '))]
                self.assertEqual(found, [
                    ['vp', '00000000', le_words(VP).hex()],
                    ['fp', '00000040', FP_TWO_UNITS.hex()],
                    ['vp', '00000000', le_words(VP).hex()],
                    ['fp', '00000002', FP_CONST.hex()],
                    ['fp', '00000000', FP_TWO_UNITS.hex()],
                ])
                # The textures block is not read whole; the wrong technique count skips that effect.
                self.assertIn('members 2 blocks 1 effects 1 skipped 1 techniques 2 loose 1', lines[-1])

    @unittest.skipUnless(COMPILER, 'C++ compiler')
    def test_damaged_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            scanner = compile_scanner(tmp)
            data = game_archive()
            cases = {'short': data[:20], 'magic': b'XSAR' + data[4:], 'cut': data[:len(data) - 40]}
            for name, blob in cases.items():
                path = pathlib.Path(tmp) / (name + '.psarc')
                path.write_bytes(blob)
                result = subprocess.run([str(scanner), str(path)], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 1, name + ': ' + result.stdout)
                self.assertTrue(result.stdout.startswith('error '), name)


def sdk_has_prewarm():
    return (SDK / 'libs/video/rsx_shader_prewarm.h').is_file()


@unittest.skipUnless(COMPILER and CC and POSIX, 'C and C++ compilers on a POSIX host')
@unittest.skipUnless(sdk_has_prewarm(), 'a runtime with rsx_shader_prewarm.h')
class ShaderWarmTests(unittest.TestCase):
    """MagShaderWarm against the runtime's stub translator (no glslang): the texts and names are
    the runtime's own."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        out = pathlib.Path(cls.tmp.name)
        video = SDK / 'libs/video'
        objects = []
        for name in ('rsx_shader_prewarm.c', 'rsx_vp_decompiler.c', 'rsx_fp_decompiler.c', 'rsx_fifo_busy.c'):
            obj = out / (name + '.o')
            build = subprocess.run([CC, '-std=gnu17', '-w', '-I' + str(SDK / 'include'), '-I' + str(video), '-c',
                                    str(video / name), '-o', str(obj)], capture_output=True, text=True, timeout=180)
            if build.returncode:
                raise AssertionError(build.stderr)
            objects.append(str(obj))
        cls.tool = out / 'MagShaderWarm'
        build = subprocess.run([COMPILER, '-std=c++17', '-Wall', '-Wextra', '-I' + str(ROOT / 'src'),
                                '-I' + str(SDK / 'include'), '-I' + str(video), '-I' + str(SDK / 'runtime/ppu'),
                                str(ROOT / 'src/mag_shader_warm.cpp'), str(ROOT / 'src/mag_shader_programs.cpp'),
                                str(ROOT / 'src/player_inflate.cpp'), str(video / 'rsx_shader_spirv.cpp'), *objects,
                                '-o', str(cls.tool)], capture_output=True, text=True, timeout=300)
        if build.returncode:
            raise AssertionError(build.stderr)
        game = out / 'PS3_GAME/USRDIR/run/mag/ps3'
        game.mkdir(parents=True)
        # An already-decrypted game archive is read as it is.
        (game / 'game.psarc.sdat').write_bytes(game_archive())
        main = effect_record([(VP, FP_TWO_UNITS, 0x40)])     # the same programs again: one each
        (game / 'main.psarc').write_bytes(psarc([('/run/mag/ps3/global_resources/global_common.zrb',
                                                  resource_block([main]))], compress=False))
        cls.game = out / 'PS3_GAME'

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_tool(self, *args, env=None):
        return subprocess.run([str(self.tool), '--game', str(self.game), *args], capture_output=True, text=True,
                              timeout=120, env=env)

    def test_dry_run_variants(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_tool('--cache', cache, '--dry-run')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # One vertex program. Fragment programs: units 0 and 14 at 32-bit exports (ctrl 0x40)
            # give cube@14 x gamma@0 = 4 texts; at 16-bit exports (0x00) sRGB export doubles that
            # to 8; the constant-only one (0x02) gives neutral and sRGB export = 2.
            self.assertIn('4 programs (1 vertex, 3 fragment)', result.stdout)
            self.assertIn('15 texts, 0 already cached, 15 to translate', result.stdout)
            names = [l for l in result.stdout.splitlines() if re.fullmatch(r'[0-9a-f]{16}\.spv', l)]
            self.assertEqual(len(names), 15)
            self.assertEqual(os.listdir(cache), [], 'a dry run writes nothing')

    def test_run_without_translator_prunes_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as cache:
            cache = pathlib.Path(cache)
            dry = self.run_tool('--cache', str(cache), '--dry-run').stdout
            current = sorted(l[:16] for l in dry.splitlines() if re.fullmatch(r'[0-9a-f]{16}\.spv', l))
            stale = 'ffffffffffffffff'
            (cache / (stale + '.spv')).write_bytes(b'from an older warm-up')
            (cache / (current[0] + '.spv')).write_bytes(b'kept: this warm-up names it')
            (cache / '0123456789abcdef.spv').write_bytes(b'kept: the engine wrote it, no warm-up named it')
            (cache / 'shader-warm.txt').write_text(stale + '\n' + current[0] + '\n')
            result = self.run_tool('--cache', str(cache), '--jobs', '3')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertRegex(result.stdout, r'\[shader-warm\] 4 programs .* 15 texts: 0 translated, 0 already cached, '
                                            r'15 failed; 1 stale files removed; .* with 1 processes')
            self.assertIn('first failure:', result.stdout)
            self.assertFalse((cache / (stale + '.spv')).exists())
            self.assertTrue((cache / (current[0] + '.spv')).exists())
            self.assertTrue((cache / '0123456789abcdef.spv').exists())
            self.assertEqual((cache / 'shader-warm.txt').read_text().split(), current)
            self.assertFalse([p for p in os.listdir(cache) if 'work' in p], 'the work files are removed')

    def test_switch_and_missing_game(self):
        env = {k: v for k, v in os.environ.items() if k not in ('PS3RECOMP_SPIRV_CACHE', 'LOCALAPPDATA')}
        result = self.run_tool(env={**env, 'MAG_SHADER_CACHE': '0', 'LOCALAPPDATA': '/nonexistent'})
        self.assertEqual(result.returncode, 0)
        self.assertIn('[shader-warm] skipped: shader cache: off (MAG_SHADER_CACHE=0)', result.stdout)
        result = self.run_tool(env=env)
        self.assertEqual(result.returncode, 0)
        self.assertIn('[shader-warm] skipped: shader cache: off (no LOCALAPPDATA)', result.stdout)
        with tempfile.TemporaryDirectory() as cache:
            result = subprocess.run([str(self.tool), '--game', cache, '--cache', cache], capture_output=True,
                                    text=True, timeout=60)
            self.assertEqual(result.returncode, 1)
            self.assertIn('[shader-warm] failed: cannot read', result.stdout)


if __name__ == '__main__':
    unittest.main()
