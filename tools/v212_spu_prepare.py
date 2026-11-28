#!/usr/bin/env python3
"""Prepare exact retail 2.12 original SPU programs for the native integration.

Registration identity is derived from verified bytes and their PPU load mappings.
Role/ABI unknowns remain visible. Unsupported generated sites terminate if reached;
preparation is not execution acceptance. No firmware policy bytes are substituted.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import target_spu_audit as audit
from tools.prepare_inputs import ROOT, data_path, load_framework, sha256_file, verify_hash
from tools.targets import get_target, validate_target_profile
from tools.verified_spu_jobs import sdk_identity
from tools.wws_job_lift import elf_segments, fnv64, LOAD_BASE, HEADER

TARGET = 'retail-v2.12'
MANIFEST = ROOT / 'profiles/retail-v2.12-spu-programs.json'
IDENTITY_FIELDS = ('kind', 'sha256', 'size', 'entry', 'occurrences')
CALLBACK_HEADER = '''#ifndef MAG_V212_SPU_REGISTRATION_H
#define MAG_V212_SPU_REGISTRATION_H
#ifdef __cplusplus
extern "C" {
#endif
void mag_v212_spu_register_all(void);
/* Caller supplies the independently reviewed firmware taskset policy image ID. */
void mag_v212_spu_register_tasksets(int reviewed_firmware_policy_id);
#ifdef __cplusplus
}
#endif
#endif
'''


def validate_inventory(manifest, rows):
    expected = {(r['kind'], r['sha256']): r for r in manifest['programs']}
    actual = {(r['kind'], r.get('sha256')): r for r in rows}
    if len(expected) != len(manifest['programs']) or len(actual) != len(rows) or set(expected) != set(actual):
        raise ValueError('SPU program inventory differs from reviewed 2.12 identities')
    for key, row in actual.items():
        for field in IDENTITY_FIELDS:
            if row.get(field) != expected[key].get(field):
                raise ValueError('SPU identity or occurrence changed: ' + str(key) + ': ' + field)
        if row.get('status') in ('invalid', 'unsupported_layout'):
            raise ValueError('SPU layout is not supported for preparation')


def assign_image_ids(rows):
    assigned, owners = {}, {}
    for row in rows:
        sha = row['sha256']
        if not re.fullmatch('[0-9a-f]{64}', sha):
            raise ValueError('Invalid program SHA-256')
        image_id = 0x10000000 | int(sha[:7], 16)
        if image_id in owners and owners[image_id] != sha:
            raise ValueError('Content-derived SPU image ID collision')
        owners[image_id] = sha
        assigned[sha] = image_id
    return assigned


def guard_source(source):
    pattern = r'/\* TODO spu:.*?\*/\s*(?:spu_unsupported\([^;]*\);|;)?|\bspu_unsupported\([^;{}]*\);'
    halt = 'do { fprintf(stderr, "[mag] unsupported 2.12 SPU in %s\\n", __func__); _Exit(86); } while (0);'
    guarded, count = re.subn(pattern, lambda _: halt, source, flags=re.S)
    return '#include <stdio.h>\n#include <stdlib.h>\n' + guarded, count


def checked_source_files(output, outputs):
    output = output.resolve()
    for relative, expected in outputs.items():
        parts = Path(relative).parts
        if (Path(relative).is_absolute() or not parts or
                any(not re.fullmatch(r'[A-Za-z0-9._-]+', p) or p in ('.', '..') for p in parts)):
            raise ValueError('Unsafe generated source path')
        path = output / relative
        if path.is_symlink() or not path.resolve().is_relative_to(output):
            raise ValueError('Generated source escaped its owned directory')
        verify_hash(path, expected)
    actual = {p.relative_to(output).as_posix() for p in output.rglob('*')
              if p.is_file() and p.suffix in ('.c', '.h', '.cpp')}
    if actual != set(outputs):
        raise ValueError('Generated translation unit/header set changed')
    return [output / p for p in outputs if Path(p).suffix in ('.c', '.cpp')]


def write_sources(path, output, sources):
    values = dict(MAG_V212_SPU_SOURCES=sources, MAG_V212_SPU_INCLUDE_DIRS=[output],
                  MAG_V212_SPU_REGISTRATION_HEADER=[output / 'mag_v212_spu_registration.h'])
    text = ''
    for name, paths in values.items():
        if any(']==]' in Path(p).as_posix() for p in paths):
            raise ValueError('Unsupported CMake path delimiter')
        text += 'set(' + name + '\n' + ''.join('  [==[' + Path(p).as_posix() + ']==]\n' for p in paths) + ')\n'
    Path(path).write_text(text)


def bind_occurrences(rows, inputs):
    segments = {image: elf_segments(path.read_bytes()) for image, path in inputs.items()}
    for row in rows:
        for occurrence in row['occurrences']:
            image = occurrence['image']
            if 'file_offset' in occurrence:
                offset = occurrence['file_offset']
                matches = [va + offset - off for va, off, size in segments[image]
                           if off <= offset and offset + row['size'] <= off + size]
                if len(matches) != 1:
                    raise ValueError('SPU file occurrence lacks a unique complete PPU load mapping')
                occurrence['guest_ea'] = matches[0]
            else:
                ea = occurrence['guest_ea']
                matches = [off + ea - va for va, off, size in segments[image]
                           if va <= ea and ea + row['size'] <= va + size]
                if len(matches) != 1:
                    raise ValueError('WWS occurrence lacks a unique complete PPU load mapping')
                occurrence['file_offset'] = matches[0]


def registration_plan(rows, image_ids):
    workloads, jobs, tasks = [], [], []
    fingerprints = {}
    for row in rows:
        sha = row['sha256']
        row['image_id'] = image_ids[sha]
        row['prefix'] = 'audit_' + sha[:16] + '_'
        if row['kind'] == 'raw_wws':
            code_end = LOAD_BASE + row['size'] - row['unexamined_tail_bytes']
            jobs.append(dict(image_id=row['image_id'], prefix=row['prefix'], load_base=LOAD_BASE,
                             span=code_end - LOAD_BASE, code=row['_raw'][HEADER:code_end - LOAD_BASE],
                             code_base=LOAD_BASE + HEADER,
                             guest_eas=sorted({x['guest_ea'] for x in row['occurrences']})))
        else:
            keys = {fnv64(row['_raw'])}
            if row['kind'] == 'embedded_elf':
                executable = row['_layout']['executable']
                if len(executable) != 1:
                    raise ValueError('Text fingerprint needs one executable PT_LOAD')
                ph = executable[0]
                keys.add(fnv64(row['_raw'][ph['off']:ph['off'] + ph['filesz']]))
                for occurrence in row['occurrences']:
                    tasks.append(dict(guest_ea=occurrence['guest_ea'], image_id=row['image_id']))
            for key in keys:
                if key in fingerprints and fingerprints[key] != sha:
                    raise ValueError('Ambiguous full-image/text runtime fingerprint')
                fingerprints[key] = sha
            workloads.append(dict(image_id=row['image_id'], prefix=row['prefix'], entry=row['entry'],
                                  fingerprints=sorted(keys), label='retail-v2.12 ' + row['kind'] + ' ' + sha))
    if len(tasks) > 16:
        raise ValueError('Task ELF occurrence bindings exceed pinned runtime capacity')
    return workloads, jobs, tasks


def byte_array(name, data):
    return ['static const uint8_t ' + name + '[] = {'] + [
        '  ' + ','.join('0x%02x' % x for x in data[n:n + 24]) + ',' for n in range(0, len(data), 24)] + ['};']


def render_registration(rows, image_ids):
    workloads, jobs, tasks = registration_plan(rows, image_ids)
    lines = ['/* PRIVATE exact original-code registration; do not commit. */',
             '#include "spu_workload.h"', '#include "spu_overlay_residency.h"',
             '#include "mag_v212_spu_registration.h"', '#include <stdio.h>', '#include <stdlib.h>',
             'extern void spu_begin_image(int);',
             'extern void spu_taskset_register_task_elf(uint32_t,int,int);']
    for row in rows:
        lines += [f"extern void {row['prefix']}spu_recomp_register(void);",
                  f"extern void {row['prefix']}spu_func_{row['entry']:08X}(spu_context*);"]
    for job in jobs:
        lines += byte_array('job_%d' % job['image_id'], job['code'])
    lines += ['void mag_v212_spu_register_all(void) {', '  static int registered;',
              '  if (registered) return;', '  registered = 1;']
    for row in rows:
        lines.append(f"  spu_begin_image({row['image_id']}); {row['prefix']}spu_recomp_register();")
    for item in workloads:
        for fp in item['fingerprints']:
            lines.append(f"  spu_workload_register_img(0x{fp:016X}ULL, {item['prefix']}spu_func_{item['entry']:08X}, {item['image_id']}, \"{item['label']}\");")
    for job in jobs:
        ident = job['image_id']
        lines += [f"  {{ const spu_overlay_code_segment code = {{0x{job['code_base']:X}u, {len(job['code'])}u, job_{ident}}};"]
        for ea in job['guest_eas']:
            lines += [f"    if (!spu_overlay_register_verified_region(0x{ea:X}u, 0x{job['load_base']:X}u, 0x{job['span']:X}u, {ident}, &code, 1)) {{",
                      '      fputs("2.12 verified WWS registration failed\\n", stderr); abort(); }']
        lines.append('  }')
    lines += ['  spu_begin_image(0);', '}',
              'void mag_v212_spu_register_tasksets(int reviewed_firmware_policy_id) {',
              '  static int registered_policy;',
              '  if (reviewed_firmware_policy_id <= 0) abort();',
              '  if (registered_policy == reviewed_firmware_policy_id) return;',
              '  if (registered_policy) abort();',
              '  mag_v212_spu_register_all();',
              '  registered_policy = reviewed_firmware_policy_id;']
    for item in tasks:
        lines.append(f"  spu_taskset_register_task_elf(0x{item['guest_ea']:X}u, {item['image_id']}, reviewed_firmware_policy_id);")
    lines += ['}', '__attribute__((constructor)) static void mag_v212_spu_ctor(void) {',
              '  mag_v212_spu_register_all();', '}', '']
    return '\n'.join(lines)


def check(output):
    output = output.resolve()
    manifest = json.loads(MANIFEST.read_text())
    receipt = json.loads((output / 'provenance.json').read_text())
    if (receipt.get('status') != 'complete' or receipt.get('target') != TARGET or
            receipt.get('executed_todo_policy') != 'halt' or
            receipt.get('guarded_todo_count') != receipt.get('unsupported_count') or
            receipt.get('manifest_sha256') != sha256_file(MANIFEST)):
        raise ValueError('2.12 SPU preparation is incomplete or identities changed')
    expected_runtime = load_framework()[0]['ps3recomp']['runtime']['commit']
    if receipt.get('sdk_revision') != expected_runtime:
        raise ValueError('Prepared SPU source uses another SDK revision')
    validate_inventory(manifest, receipt['programs'])
    sources = checked_source_files(output, receipt['outputs'])
    return receipt, sources


def generate(sdk, output):
    definition = get_target(TARGET)
    profile_path = data_path(definition.profile)
    profile = json.loads(profile_path.read_text())
    validate_target_profile(profile, TARGET)
    manifest_hash = sha256_file(MANIFEST)
    manifest = json.loads(MANIFEST.read_text())
    framework, framework_path = load_framework()
    revision = framework['ps3recomp']['runtime']['commit']
    pin = sdk_identity(sdk, revision)
    inputs = {name: Path(item['elf']) for name, item in profile['images'].items()}
    for name, path in inputs.items():
        verify_hash(path, manifest['carriers'][name])
    rows = audit.discover_programs(inputs, sdk)
    rows += audit.policy_program(inputs, ROOT / 'profiles/retail-v1.0-engine-spu.json')
    bind_occurrences(rows, inputs)
    validate_inventory(manifest, rows)
    ids = assign_image_ids(rows)
    output = audit.prepare_output(output, TARGET)
    receipt = dict(schema=1, target=TARGET, status='incomplete', not_runtime_verified=True,
                   executed_todo_policy='halt', errors=[], sdk_revision=revision, sdk_pin=pin,
                   manifest_sha256=manifest_hash, profile_sha256=sha256_file(profile_path),
                   framework_sha256=sha256_file(framework_path), generator_sha256=sha256_file(Path(__file__)),
                   audit_tool_sha256=sha256_file(Path(audit.__file__)),
                   sdk_tools={str(p.relative_to(sdk)): sha256_file(p) for p in sorted((sdk / 'tools').glob('*.py'))},
                   input_hashes=manifest['carriers'], programs=[], unsupported_count=0, guarded_todo_count=0)
    try:
        modules = audit.sdk_modules(sdk)
        deadline = time.monotonic() + 600
        expected = {(p['kind'], p['sha256']): p for p in manifest['programs']}
        for row in rows:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ValueError('Bounded 2.12 SPU generation deadline exhausted')
            audit.lift_program(row, sdk, output, modules, min(120, remaining))
            if (row['status'] not in ('generated', 'generated_with_hazards') or
                    not row.get('entry_generated')):
                raise ValueError('SPU generation did not emit its exact entry: ' + row['sha256'])
            if row['census']['unsupported_count'] != expected[(row['kind'], row['sha256'])]['expected_unsupported_count']:
                raise ValueError('SPU unsupported instruction census changed: ' + row['sha256'])
            source_path = output / row['generated_directory'] / 'spu_recomp.c'
            guarded, count = guard_source(source_path.read_text())
            if count != row['census']['todo_count']:
                raise ValueError('SPU unsupported/TODO guarding census mismatch')
            source_path.write_text(guarded)
            row['guarded_todo_count'] = count
            receipt['guarded_todo_count'] += count
            receipt['unsupported_count'] += row['census']['unsupported_count']
        if receipt['unsupported_count'] != receipt['guarded_todo_count']:
            raise ValueError('Not every unsupported generated instruction has an executed halt')
        (output / 'mag_v212_spu_registration.h').write_text(CALLBACK_HEADER)
        (output / 'mag_v212_spu_registration.c').write_text(render_registration(rows, ids))
        for path in output.rglob('*'):
            if path.is_file() and path.suffix in ('.c', '.h'):
                text = re.sub(r'#include "(?:\.\./)+runtime/spu/([^\"]+)"', r'#include "\1"', path.read_text())
                path.write_text(text)
        verify_hash(MANIFEST, manifest_hash)
        for name, path in inputs.items():
            verify_hash(path, manifest['carriers'][name])
        sdk_identity(sdk, revision)
        for relative, expected_hash in receipt['sdk_tools'].items():
            verify_hash(sdk / relative, expected_hash)
        receipt['outputs'] = {p.relative_to(output).as_posix(): sha256_file(p) for p in sorted(output.rglob('*'))
                              if p.is_file() and p.suffix in ('.c', '.h', '.cpp')}
        receipt['status'] = 'complete'
    except (OSError, ValueError, KeyError, SystemExit) as error:
        receipt['errors'].append(str(error))
    finally:
        receipt['programs'] = [audit.serializable(row) for row in rows]
        (output / 'provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if receipt['status'] != 'complete':
        raise ValueError('2.12 SPU preparation failed; see ' + str(output / 'provenance.json'))
    checked, sources = check(output)
    write_sources(output / 'sources.cmake', output, sources)
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--output', type=Path, help='fresh ignored output directory')
    mode.add_argument('--check', type=Path, help='verify an existing prepared source set')
    parser.add_argument('--sdk', type=Path, help='clean framework-pinned runtime checkout')
    parser.add_argument('--sources-out', type=Path, help='write checked CMake paths relative to current package location')
    args = parser.parse_args()
    if args.check:
        receipt, sources = check(args.check)
        if args.sources_out:
            write_sources(args.sources_out, args.check.resolve(), sources)
    else:
        sdk = args.sdk or data_path(load_framework()[0]['ps3recomp']['runtime']['path'])
        receipt = generate(sdk.resolve(), args.output.resolve())
    print(json.dumps(dict(status=receipt['status'], target=TARGET, programs=len(receipt['programs']),
                          unsupported_count=receipt['unsupported_count'], executed_todo_policy='halt',
                          not_runtime_verified=True), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
