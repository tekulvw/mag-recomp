#!/usr/bin/env python3
"""Source-bound offline SPU census and generation, with no runtime registration.

Original inputs, extracted images, generated code and diagnostic logs are private
outputs. A completed audit only means the bounded tool run finished; role, ABI,
indirect coverage and runtime acceptance remain independent unknowns.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, WORKSPACE, data_path, load_framework, sha256_file, verify_hash
from tools.verified_spu_jobs import fresh_output, sdk_identity
from tools import wws_job_lift as wws

# File offsets are identities, never native registration addresses. Metadata only.
POLICY_REGIONS = {
    '0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c': 0x19D4C80,
    '705a6ec6a25458b2401b869e75e2904597304e2c06d9534a171a1dcd54aca2b3': 0x14C1680,
}
PHASE_SECONDS = 600


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sdk_modules(sdk):
    """Load the selected pinned tool sources, not a globally installed SDK."""
    names = ('spu_elf', 'spu_program_pipeline', 'spu_disasm', 'find_spu_functions', 'wrap_spu_elf')
    previous = {name: sys.modules.get(name) for name in names}
    old_path = sys.path[:]
    modules = {}
    try:
        sys.path.insert(0, str(sdk / 'tools'))
        for name in names:
            spec = importlib.util.spec_from_file_location(name, sdk / 'tools' / (name + '.py'))
            module = importlib.util.module_from_spec(spec)
            sys.modules[name] = module
            spec.loader.exec_module(module)
            modules[name] = module
    finally:
        sys.path[:] = old_path
        for name, old in previous.items():
            if old is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old
    return modules


def discover_programs(inputs, sdk):
    """Deduplicate exact ELF bytes; retain all invalid candidates and WWS jobs."""
    modules = sdk_modules(sdk)
    rows, unique = [], {}
    for image, path in sorted(inputs.items()):
        buf = Path(path).read_bytes()
        for offset, info, error in modules['spu_elf'].scan_spu_candidates(buf):
            occurrence = dict(image=image, file_offset=offset)
            if error:
                rows.append(dict(kind='embedded_elf', status='invalid', reason=error,
                                 occurrences=[occurrence], role=None, abi_verified=False))
                continue
            raw = buf[offset:offset + info['size']]
            identity = digest(raw)
            if identity in unique:
                unique[identity]['occurrences'].append(occurrence)
                continue
            row = dict(kind='embedded_elf', sha256=identity, size=len(raw), entry=info['entry'],
                       occurrences=[occurrence], role=None, abi_verified=False,
                       status='discovered', _raw=raw, _layout=info)
            try:
                row['executable_ranges'] = modules['spu_program_pipeline'].code_ranges(info)
            except ValueError as error:
                row.update(status='unsupported_layout', reason=str(error))
            unique[identity] = row
            rows.append(row)
        # The title's header scanner takes ELF64 PT_LOADs; a scanner rejection
        # must be visible, while arbitrary standalone synthetic SPU ELFs needn't
        # be accepted as PPU containers.
        if buf[:7] == b'\x7fELF\x02\x02\x01':
            for module in wws.find_modules(buf):
                raw = module['raw']
                rows.append(dict(kind='raw_wws', sha256=digest(raw), size=len(raw),
                                 entry=module['entry'], status='discovered', role=None,
                                 abi_verified=False, _raw=raw,
                                 occurrences=[dict(image=image, guest_ea=module['ea'])],
                                 coverage='Header entry plus direct-flow extent; indirect entry coverage unknown.'))
    return rows


def prepare_output(output, target, workspace=WORKSPACE):
    from tools.targets import validate_target_output
    validate_target_output(Path(output), target, workspace=workspace)
    return fresh_output(output)


def run_tool(command, logfile, timeout):
    """Retain unsuccessful stdout and partial generated files, including timeout."""
    env = dict(os.environ, OMP_NUM_THREADS='2', OPENBLAS_NUM_THREADS='2', VECLIB_MAXIMUM_THREADS='2')
    record = dict(command=[str(x) for x in command], timed_out=False, returncode=None, completed=False)
    start = time.monotonic()
    with Path(logfile).open('wb') as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=timeout, env=env)
            record.update(returncode=result.returncode, completed=result.returncode == 0)
        except subprocess.TimeoutExpired:
            record['timed_out'] = True
        except OSError as error:
            record['error'] = str(error)
    record.update(elapsed_seconds=round(time.monotonic() - start, 3), log=Path(logfile).name)
    return record


def generation_census(diagnostics, source):
    starts = {s for s, _ in diagnostics['functions']}
    calls = set(diagnostics['call_targets'])
    branches = set(diagnostics['branch_targets'])
    unsupported = diagnostics['unsupported']
    return dict(functions=len(starts), unsupported=unsupported,
                unsupported_count=sum(unsupported.values()),
                unresolved_direct_targets=sorted((calls | branches) - starts),
                todo_count=source.count('/* TODO spu:'),
                runtime_verified=False,
                indirect_dispatch_count=len(re.findall(r'\bspu_indirect_branch\b', source)),
                fallthrough_annotations=len(re.findall('fall.?through', source, re.I)),
                branch_coverage_verified=False,
                note='Lexical dispatch/fallthrough counts are diagnostics, not proof of branch coverage.')


def lift_program(row, sdk, output, modules, timeout):
    dest = output / (row['kind'] + '_' + row['sha256'])
    dest.mkdir()
    row['generated_directory'] = str(dest.relative_to(output))
    raw = row['_raw']
    (dest / 'input.bin').write_bytes(raw)
    prefix = 'audit_' + row['sha256'][:16] + '_'
    command = [sys.executable, str(sdk / 'tools/spu_lifter.py'),
               '--output', str(dest), '--report', str(dest / 'lift.json'), '--symbol-prefix', prefix]
    if row['kind'] == 'embedded_elf':
        functions, _ = modules['find_spu_functions'].detect_functions(raw, verbose=False)
        bounds = sorted({(max(s, a), min(e, b)) for s, e in functions
                         for a, b in row['executable_ranges'] if max(s, a) < min(e, b)})
        code, base = modules['find_spu_functions'].text_image(
            raw, modules['find_spu_functions'].parse_elf(raw)['phs'])
    elif row['kind'] == 'raw_wws':
        base = wws.LOAD_BASE + wws.HEADER
        end = wws.reachable_end(raw, [row['entry']], base, wws.LOAD_BASE + len(raw))
        row['executable_ranges'] = [(base, end)]
        wrapper = wws.wrapper_elf(raw, end, row['entry'])
        (dest / 'wrapper.elf').write_bytes(wrapper)
        functions, _ = modules['find_spu_functions'].detect_functions(wrapper, verbose=False)
        bounds = [(max(s, base), min(e, end)) for s, e in functions if max(s, base) < min(e, end)]
        code = raw[wws.HEADER:end - wws.LOAD_BASE]
        row['unexamined_tail_bytes'] = len(raw) - (end - wws.LOAD_BASE)
    else:
        base = row['base']
        wrapped = modules['wrap_spu_elf'].wrap(raw, base=base, entry=row['entry'])
        (dest / 'policy.elf').write_bytes(wrapped)
        functions, _ = modules['find_spu_functions'].detect_functions(wrapped, verbose=False)
        bounds = [(s, min(e, row['code_end'])) for s, e in functions if s < row['code_end']]
        code = raw[:row['code_end'] - base]
        if row.get('extra_functions'):
            command += ['--extra-funcs', ','.join(hex(x) for x in row['extra_functions'])]
    (dest / 'code.bin').write_bytes(code)
    (dest / 'functions.json').write_text(json.dumps([dict(start=s, end=e) for s, e in bounds]))
    command += [str(dest / 'code.bin'), '--base', str(base), '--functions', str(dest / 'functions.json')]
    row['execution'] = run_tool(command, dest / 'lift.log', timeout)
    row['generated_directory'] = str(dest.relative_to(output))
    if not row['execution']['completed']:
        row.update(status='generation_failed', reason='lifter subprocess failed or timed out')
    else:
        try:
            diagnostics = json.loads((dest / 'lift.json').read_text())
            source = (dest / 'spu_recomp.c').read_text()
            row['census'] = generation_census(diagnostics, source)
            header = (dest / 'spu_recomp.h').read_text()
            row['entry_generated'] = (row['entry'] in {s for s, _ in diagnostics['functions']} and
                                      f"{prefix}spu_func_{row['entry']:08X}(" in header)
            hazards = (not row['entry_generated'] or row['census']['unsupported_count'] or row['census']['todo_count'] or
                       row['census']['unresolved_direct_targets'])
            row['status'] = 'generated_with_hazards' if hazards else 'generated'
        except (OSError, ValueError, KeyError, TypeError) as error:
            row.update(status='generation_failed', reason='incomplete diagnostics: ' + str(error))
    row['outputs'] = {str(p.relative_to(output)): sha256_file(p) for p in sorted(dest.iterdir()) if p.is_file()}


def policy_program(inputs, engine_profile):
    metadata = json.loads(engine_profile.read_text())['policy']
    policy = []
    for image, path in sorted(inputs.items()):
        if image != 'mage_g':
            continue
        parent_hash = sha256_file(path)
        if parent_hash not in POLICY_REGIONS:
            raise ValueError('No reviewed policy region for engine identity')
        offset = POLICY_REGIONS[parent_hash]
        with path.open('rb') as stream:
            stream.seek(offset)
            raw = stream.read(metadata['size'])
        if digest(raw) != metadata['sha256']:
            raise ValueError('Known policy region hash mismatch')
        policy.append(dict(kind='known_policy', status='discovered', sha256=digest(raw), size=len(raw),
                           role='Exact-byte identity with reviewed v1 policy; target ABI unknown', abi_verified=False,
                           base=metadata['base'], entry=metadata['entry'], code_end=metadata['code_end'],
                           extra_functions=metadata.get('extra_functions', []), _raw=raw,
                           occurrences=[dict(image=image, file_offset=offset)],
                           coverage='Identity observation only; target registration and dispatch map unverified.'))
    return policy


def serializable(value):
    return {key: item for key, item in value.items() if not key.startswith('_')}


def audit_target(target, sdk, output=None):
    from tools.targets import get_target, validate_target_profile
    definition = get_target(target)
    profile_path = data_path(definition.profile)
    profile_hash = sha256_file(profile_path)
    profile = json.loads(profile_path.read_text())
    validate_target_profile(profile, target)
    framework, framework_path = load_framework()
    runtime = framework['ps3recomp']['runtime']['commit']
    pin = sdk_identity(sdk, runtime)
    tool_hashes = {str(p.relative_to(sdk)): sha256_file(p) for p in sorted((sdk / 'tools').glob('*.py'))}
    inputs = {name: Path(record['elf']) for name, record in profile['images'].items()}
    for name, path in inputs.items():
        verify_hash(path, profile['images'][name]['elf_sha256'])
    output = prepare_output(output or data_path(definition.analysis) / 'spu-lift-audit', target)
    policy_path = ROOT / 'profiles/retail-v1.0-engine-spu.json'
    report = dict(schema=1, generator='target_spu_audit', target=target, app_version=definition.app_version,
                  title_id=definition.title_id, not_runtime_verified=True, production_registration_emitted=False,
                  audit_completed=False, errors=[], programs=[],
                  provenance=dict(profile_sha256=profile_hash, framework_sha256=sha256_file(framework_path),
                                  sdk=pin, sdk_tools=tool_hashes, python=sys.version,
                                  policy_metadata_sha256=sha256_file(policy_path),
                                  audit_tool_sha256=sha256_file(Path(__file__)),
                                  wws_tool_sha256=sha256_file(Path(wws.__file__)),
                                  inputs={name: dict(path=str(path), sha256=sha256_file(path)) for name, path in inputs.items()}))
    rows = []
    try:
        rows = discover_programs(inputs, sdk)
        rows += policy_program(inputs, policy_path)
        modules = sdk_modules(sdk)
        for kind in ('embedded_elf', 'raw_wws', 'known_policy'):
            deadline = time.monotonic() + PHASE_SECONDS
            for row in [r for r in rows if r['kind'] == kind and r['status'] == 'discovered']:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    row.update(status='not_generated', reason='bounded phase deadline exhausted')
                    continue
                try:
                    lift_program(row, sdk, output, modules, min(120, remaining))
                except (OSError, ValueError, KeyError, SystemExit) as error:
                    row.update(status='generation_failed', reason=str(error))
        verify_hash(profile_path, profile_hash)
        sdk_identity(sdk, runtime)
        for name, path in inputs.items():
            verify_hash(path, profile['images'][name]['elf_sha256'])
        for relative, expected in tool_hashes.items():
            verify_hash(sdk / relative, expected)
        report['audit_completed'] = True
    except (OSError, ValueError, KeyError, SystemExit) as error:
        report['errors'].append(str(error))
    finally:
        report['programs'] = [serializable(row) for row in rows]
        report['counts'] = dict(embedded_elf=sum(r['kind'] == 'embedded_elf' and r['status'] != 'invalid' for r in rows),
                              raw_wws=sum(r['kind'] == 'raw_wws' for r in rows),
                              known_policy=sum(r['kind'] == 'known_policy' for r in rows),
                              unknown_roles=sum(r.get('role') is None for r in rows),
                              statuses={status: sum(r['status'] == status for r in rows) for status in sorted({r['status'] for r in rows})},
                              unsupported_instructions=sum(r.get('census', {}).get('unsupported_count', 0) for r in rows))
        (output / 'audit.json').write_text(json.dumps(report, indent=2) + '\n')
    return report, output


def main():
    from tools.targets import DEFAULT_TARGET, TARGET_NAMES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=TARGET_NAMES, default=DEFAULT_TARGET)
    parser.add_argument('--sdk', type=Path, default=None, help='clean pinned runtime SDK (default: framework runtime path)')
    parser.add_argument('--output', type=Path, help='fresh private directory; never reused')
    args = parser.parse_args()
    sdk = args.sdk or data_path(load_framework()[0]['ps3recomp']['runtime']['path'])
    report, output = audit_target(args.target, sdk.resolve(), args.output)
    print(json.dumps(dict(target=args.target, audit_completed=report['audit_completed'], counts=report['counts'],
                          not_runtime_verified=True, report=str(output / 'audit.json')), indent=2))
    # A census with hazards is useful evidence but never a successful coverage gate.
    return 0 if report['audit_completed'] and all(r['status'] == 'generated' for r in report['programs']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
