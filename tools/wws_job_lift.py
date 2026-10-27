#!/usr/bin/env python3
"""Lift every WWS SPU job module embedded in the retail engine ELF (private output).

The job manager streams each module from its main-memory image into the job
code buffer at local store 0x4000, header included. This tool finds the module
images, bounds each module's code by control-flow reachability from its header
entry (plus optional measured interpreter entries), lifts that interval with
the SDK lifter, and emits a registration file that registers each module as a
verified streamed region (spu_overlay_register_verified_region) when
MAG_COMPILED_JOBS=1. Original bytes and generated code stay in the private
output directory; the tool itself holds no title bytes.
"""
import argparse, json, pathlib, re, struct, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from verified_spu_jobs import digest, sdk_identity, fresh_output, copy_baseline, executable_sections

LOAD_BASE = 0x4000
HEADER = 0x30
MAGIC = b'\xC0\xDE\xC0\xDE'
FIRST_IMAGE_ID = 200
PREFIX = 'mag_wwsjob_{ea:08x}_'
REGISTRY = 'jobmods_residency_register.c'
LS_SIZE = 0x40000


def elf_segments(elf):
    if elf[:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 2:
        raise ValueError('expected a big-endian ELF64 engine image')
    phoff = struct.unpack_from('>Q', elf, 0x20)[0]
    phes, phn = struct.unpack_from('>HH', elf, 0x36)
    out = []
    for i in range(phn):
        typ, _, off, va, _, filesz, _, _ = struct.unpack_from('>IIQQQQQQ', elf, phoff + i * phes)
        if typ == 1 and filesz:
            out.append((va, off, filesz))
    return out


def find_modules(elf):
    """Module images: 16-byte aligned header, magic at +0x20, load base at +0x28."""
    modules = []
    for va, off, size in elf_segments(elf):
        blob = elf[off:off + size]
        at = blob.find(MAGIC)
        while at != -1:
            h = at - 0x20
            if h >= 0 and h % 16 == 0 and h + HEADER <= len(blob) and \
                    struct.unpack_from('>I', blob, h + 0x28)[0] == LOAD_BASE:
                entry, image = struct.unpack_from('>II', blob, h + 0x10)
                if HEADER <= entry < image <= LS_SIZE - LOAD_BASE and h + image <= len(blob):
                    modules.append(dict(ea=va + h, entry=LOAD_BASE + entry, size=image,
                                        raw=bytes(blob[h:h + image])))
            at = blob.find(MAGIC, at + 4)
    modules.sort(key=lambda m: m['ea'])
    return modules


def fnv64(data):
    """The hash SPU_INTERP_ATTRIBUTION reports for ls4000_hash (its seed is not
    the standard FNV-1a offset basis; keep it identical to the report)."""
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# SPU branch encodings (Cell BE SPU ISA): RI16 branches by 9-bit opcode,
# indirect branches and stops by 11-bit opcode.
RELATIVE = {0x064: 'jump', 0x066: 'call', 0x040: 'cond', 0x042: 'cond', 0x044: 'cond', 0x046: 'cond'}
ABSOLUTE = {0x060: 'jump', 0x062: 'call'}
INDIRECT_END = {0x1A8, 0x1AA}            # bi, iret: no fallthrough
INDIRECT_CONT = {0x1A9, 0x1AB, 0x128, 0x129, 0x12A, 0x12B}   # bisl/bisled, conditional bi*


def reachable_end(raw, entries, lo, hi):
    """Highest reachable instruction + 4, following direct branches only.
    Indirect targets are unknown; code reached only through them stays interpreted."""
    seen, work = set(), [e for e in entries if lo <= e < hi]
    while work:
        pc = work.pop()
        while lo <= pc < hi and pc not in seen and not pc & 3:
            seen.add(pc)
            w = struct.unpack_from('>I', raw, pc - LOAD_BASE)[0]
            op9, op11 = w >> 23, w >> 21
            i16 = (w >> 7) & 0xFFFF
            rel = (pc + ((i16 - 0x10000 if i16 & 0x8000 else i16) << 2)) & (LS_SIZE - 1)
            if op9 in RELATIVE or op9 in ABSOLUTE:
                kind = RELATIVE.get(op9) or ABSOLUTE[op9]
                work.append(rel if op9 in RELATIVE else (i16 << 2) & (LS_SIZE - 1))
                if kind == 'jump':
                    break
            elif op11 in INDIRECT_END:
                break
            pc += 4
    if not seen:
        raise ValueError('no reachable code')
    return max(seen) + 4


def wrapper_elf(raw, code_end, entry):
    """Minimal SPU ELF32: one PT_LOAD of the whole image at 0x4000 and an
    executable .text section covering [0x4030, code_end)."""
    shstr = b'\0.text\0.shstrtab\0'
    ehsize, phsize, shsize = 52, 32, 40
    image_off = 0x100
    shstr_off = image_off + len(raw)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    eh = bytearray(b'\x7fELF\x01\x02\x01' + b'\0' * 9)
    eh += struct.pack('>HHIIIIIHHHHHH', 2, 23, 1, entry, ehsize, sh_off, 0,
                      ehsize, phsize, 1, shsize, 3, 2)
    ph = struct.pack('>IIIIIIII', 1, image_off, LOAD_BASE, LOAD_BASE, len(raw), len(raw), 7, 16)
    text = struct.pack('>IIIIIIIIII', 1, 1, 6, LOAD_BASE + HEADER, image_off + HEADER,
                       code_end - LOAD_BASE - HEADER, 0, 0, 16, 0)
    strtab = struct.pack('>IIIIIIIIII', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    out = bytearray(eh + ph)
    out += b'\0' * (image_off - len(out)) + raw + shstr
    out += b'\0' * (sh_off - len(out)) + b'\0' * shsize + text + strtab
    return bytes(out)


def attribution_entries(paths):
    """{ls4000_hash: {entry,...}} from SPU_INTERP_ATTRIBUTION reports."""
    found = {}
    for path in paths:
        lines = pathlib.Path(path).read_text().splitlines()
        cols = next((l.split('\t') for l in lines if l.startswith('wall_ns\t')), None)
        if not cols:
            raise ValueError('not an interpreter attribution report: ' + str(path))
        for line in lines[lines.index('\t'.join(cols)) + 1:]:
            row = dict(zip(cols, line.split('\t')))
            if len(row) == len(cols):
                found.setdefault(row['ls4000_hash'], set()).add(int(row['entry'], 16))
    return found


def emit_registry(modules):
    lines = ['/* PRIVATE generated expected guest bytes; do not commit. */',
             '#include "spu_overlay_residency.h"', '#include <stdio.h>', '#include <stdlib.h>',
             '#include <string.h>', 'extern void spu_begin_image(int);']
    for m in modules:
        lines.append(f"extern void {m['prefix']}spu_recomp_register(void);")
        code = m['raw'][HEADER:m['code_end'] - LOAD_BASE]
        lines.append(f"static const uint8_t job_{m['image_id']}_code[] = {{")
        lines += ['  ' + ','.join(f'0x{x:02x}' for x in code[k:k + 24]) + ',' for k in range(0, len(code), 24)]
        lines.append('};')
    lines += ['__attribute__((constructor)) static void mag_register_streamed_jobs(void) {',
              '  const char* enabled = getenv("MAG_COMPILED_JOBS");',
              '  if (!enabled || strcmp(enabled, "1")) return;']
    for m in modules:
        n = m['code_end'] - LOAD_BASE - HEADER
        lines.append(f"  {{ const spu_overlay_code_segment seg = {{0x{LOAD_BASE + HEADER:X}u, {n}u, job_{m['image_id']}_code}};")
        lines.append(f"    spu_begin_image({m['image_id']}); {m['prefix']}spu_recomp_register();")
        # The span is the lifted code alone. Jobs DMA their own data into the
        # rest of the module image, and any transfer into the span from another
        # source drops the resident owner -- after which every indirect branch
        # of the running job falls to the interpreter.
        span = m['code_end'] - LOAD_BASE
        lines.append(f"    if (!spu_overlay_register_verified_region(0x{m['ea']:08X}u, 0x{LOAD_BASE:X}u, 0x{span:X}u, {m['image_id']}, &seg, 1)) {{")
        lines.append('      fputs("MAG streamed job registration failed\\n", stderr); abort(); } }')
    lines += ['  spu_begin_image(0);', '}']
    return '\n'.join(lines) + '\n'


def generated_census(source, prefix):
    functions = len(re.findall(r'^void ' + prefix + r'spu_func_', source, re.M))
    unsupported = bool(re.search(r'unsupported|UNSUPPORTED|\.word', source))
    return functions, unsupported


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--engine-elf', type=pathlib.Path, required=True)
    ap.add_argument('--engine-elf-sha256', required=True)
    ap.add_argument('--sdk', type=pathlib.Path, required=True)
    ap.add_argument('--sdk-revision', required=True)
    ap.add_argument('--baseline', type=pathlib.Path, required=True, help='existing audited SPU directory')
    ap.add_argument('--output', type=pathlib.Path, required=True)
    ap.add_argument('--attribution', type=pathlib.Path, action='append', default=[],
                    help='SPU_INTERP_ATTRIBUTION report(s) whose entries become extra lift entries')
    ap.add_argument('--merge-chunks', action='store_true',
                    help='lift each contiguous run between call targets as one host function, so '
                         'branches inside it are gotos instead of drain-loop transfers')
    a = ap.parse_args()
    if digest(a.engine_elf) != a.engine_elf_sha256:
        raise ValueError('engine ELF identity')
    pin = sdk_identity(a.sdk, a.sdk_revision)
    lifter = a.sdk / 'tools/spu_lifter.py'
    if 'spu_overlay_register_verified_region' not in (a.sdk / 'runtime/spu/spu_overlay_residency.h').read_text():
        raise ValueError('SDK lacks verified streamed regions')
    base = json.loads((a.baseline / 'provenance.json').read_text())
    base_digest = digest(a.baseline / 'provenance.json')
    if base.get('status') != 'complete':
        raise ValueError('baseline must be complete')
    for rel, h in base['outputs'].items():
        if digest(a.baseline / rel) != h:
            raise ValueError('baseline drift ' + rel)
    if (a.baseline / REGISTRY).exists():
        raise ValueError('baseline already has streamed job registration')
    extras = attribution_entries(a.attribution)
    modules = find_modules(a.engine_elf.read_bytes())
    if not modules:
        raise ValueError('no job modules found')
    output = fresh_output(a.output)
    copy_baseline(a.baseline, output)
    (output / 'provenance.json').write_text(json.dumps(dict(base, status='preparing'), indent=2) + '\n')
    work = output / 'wws-job-inputs'
    work.mkdir(mode=0o700)
    lifted, records = [], []
    for index, m in enumerate(modules):
        m['image_id'] = FIRST_IMAGE_ID + index
        m['prefix'] = PREFIX.format(ea=m['ea'])
        lo, hi = LOAD_BASE + HEADER, LOAD_BASE + m['size']
        measured = sorted(e for e in extras.get('%016x' % fnv64(m['raw'][:64]), ()) if lo <= e < hi)
        record = dict(ea=hex(m['ea']), size=hex(m['size']), entry=hex(m['entry']),
                      image_id=m['image_id'], raw_sha256=digest_bytes(m['raw']))
        try:
            m['code_end'] = reachable_end(m['raw'], [m['entry'], *measured], lo, hi)
            extra = [e for e in measured if e < m['code_end']]
            wrapper = wrapper_elf(m['raw'], m['code_end'], m['entry'])
            if executable_sections(wrapper) != [(lo, m['raw'][HEADER:m['code_end'] - LOAD_BASE])]:
                raise ValueError('wrapper executable coverage')
            elf_path = work / f"job_{m['ea']:08x}.elf"
            elf_path.write_bytes(wrapper)
            dest = output / f"mag_wwsjob_{m['image_id']}"
            cmd = [sys.executable, str(lifter), '--auto-functions', str(elf_path), '--code-end', hex(m['code_end']),
                   '--symbol-prefix', m['prefix'], '--output', str(dest)]
            if extra:
                cmd += ['--extra-funcs', ','.join(hex(e) for e in extra)]
            if a.merge_chunks:
                cmd += ['--merge-chunks']
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            (work / f"lift_{m['ea']:08x}.log").write_text(result.stdout + result.stderr)
            if result.returncode:
                raise ValueError('lifter failed')
            for f in [dest / 'spu_recomp.c', dest / 'spu_recomp.h']:
                f.write_text(f.read_text().replace('../../runtime/spu/', ''))
            count, unsupported = generated_census((dest / 'spu_recomp.c').read_text(), m['prefix'])
            if unsupported or not count:
                for f in dest.iterdir():
                    f.unlink()
                dest.rmdir()
                raise ValueError('unsupported instructions' if unsupported else 'no functions')
            record.update(status='lifted', code_end=hex(m['code_end']), functions=count,
                          extra_entries=[hex(e) for e in extra], command=cmd[1:])
            lifted.append(m)
        except ValueError as e:
            record.update(status='interpreted', reason=str(e))
        records.append(record)
    (output / REGISTRY).write_text(emit_registry(lifted))
    if sdk_identity(a.sdk, a.sdk_revision) != pin or digest(a.baseline / 'provenance.json') != base_digest:
        raise ValueError('input drift')
    base['wws_job_extension'] = {'engine_elf_sha256': a.engine_elf_sha256, 'SDK_pin': pin,
                                 'lifter_sha256': digest(lifter), 'runtime_toggle': 'MAG_COMPILED_JOBS=1',
                                 'attribution_sha256': [digest(p) for p in a.attribution], 'modules': records}
    base['outputs'] = {str(f.relative_to(output)): digest(f) for f in sorted(output.rglob('*'))
                       if f.suffix in ('.c', '.h', '.cpp')}
    (output / 'provenance.json').write_text(json.dumps(base, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'modules': len(modules), 'lifted': len(lifted),
                      'interpreted': [r for r in records if r['status'] != 'lifted']}, indent=2))


def digest_bytes(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


if __name__ == '__main__':
    main()
