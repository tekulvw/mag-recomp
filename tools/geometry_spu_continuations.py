"""Add private exact image105 resume entries without regenerating existing code.

The SDK lifts overlapping suffix bounds. Only its two complete definitions are
selected verbatim; all inherited generated files remain byte-for-byte unchanged.
Original bytes and generated title code must stay outside Git.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from verified_spu_jobs import copy_baseline, digest, fresh_output, sdk_identity

PREFIX = 'mag_edgegeom_perf_'
RESUMES = ((0x7A44, 0x7BBC), (0x7BE0, 0x7ED0))
RAW_SHA = '0ba87cc07fc961ca98231735183699f1b71ecec792dae70095c1b57f430fefe8'
GUARDS = r'spu_verified_code_enter_span\(ctx, 105, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\)'


def continuation_bounds(source):
    bounds = [(int(a, 16), int(b, 16)) for a, b in re.findall(GUARDS, source)]
    if bounds.count((0x7A40, 0x7BBC)) != 1 or bounds.count((0x7BBC, 0x7ED0)) != 1:
        raise ValueError('unreviewed original source spans')
    if any(a in dict(bounds) for a, _ in RESUMES):
        raise ValueError('resume already registered')
    return list(RESUMES)


def registration_source():
    return '''
/* Additive entries use the existing complete image105 identity. */
__attribute__((constructor)) static void mag_register_edgegeom_continuations(void) {
    const char* enabled=getenv("MAG_COMPILED_GEOMETRY");
    if (!enabled || strcmp(enabled,"1")) return;
    spu_begin_image(105);
    spu_register_function(0x7A44u,mag_edgegeom_perf_spu_func_00007A44);
    spu_register_function(0x7BE0u,mag_edgegeom_perf_spu_func_00007BE0);
    spu_begin_image(0);
}
'''


def definition(source, entry):
    name = PREFIX + f'spu_func_{entry:08X}'
    matches = re.findall(r'void ' + name + r'\(spu_context\* ctx\) \{\n.*?\n\}', source, re.S)
    if len(matches) != 1:
        raise ValueError('missing/ambiguous SDK-generated definition')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('sdk', 'baseline', 'edge-raw', 'output'):
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--sdk-revision', required=True)
    args = parser.parse_args()
    pin = sdk_identity(args.sdk, args.sdk_revision)
    if digest(args.edge_raw) != RAW_SHA:
        raise ValueError('unreviewed original geometry module')
    original = args.baseline / 'mag_verified_105/spu_recomp.c'
    source = original.read_text()
    resumes = continuation_bounds(source)
    bounds = [(int(a, 16), int(b, 16)) for a, b in re.findall(GUARDS, source)]
    provenance = args.baseline / 'provenance.json'
    receipt = json.loads(provenance.read_text())
    if receipt.get('status') != 'complete' or receipt.get('geometry_continuation_extension'):
        raise ValueError('unqualified baseline')
    for name, expected in receipt['outputs'].items():
        if digest(args.baseline / name) != expected:
            raise ValueError('inherited source drift: ' + name)
    baseline_sha = digest(provenance)
    output = fresh_output(args.output)
    copy_baseline(args.baseline, output)
    dest = output / 'mag_edgegeom_resume_105'
    dest.mkdir()
    bound_data = json.dumps([dict(start=a, end=b) for a, b in sorted(bounds + resumes)], indent=2) + '\n'
    (dest / 'bounds.private.json').write_text(bound_data)
    with tempfile.TemporaryDirectory(prefix='mag-edge-resume-lift-') as tmp:
        generated = pathlib.Path(tmp)
        manifest = generated / 'bounds.json'
        manifest.write_text(bound_data)
        command = [sys.executable, str(args.sdk / 'tools/spu_lifter.py'), str(args.edge_raw),
                   '--base', '0x4000', '--functions', str(manifest), '--code-end', '0x15B50',
                   '--symbol-prefix', PREFIX, '--verified-code-image', '105',
                   '--verified-code-spans', '--output', str(generated)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        (dest / 'lift.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            raise ValueError('SDK lift failed')
        lifted = (generated / 'spu_recomp.c').read_text()
        functions = [definition(lifted, a) for a, _ in resumes]
        if any(re.search(r'TODO|unsupported|UNSUPPORTED|\.word', body) for body in functions):
            raise ValueError('unsupported resume instruction')
        for body, (start, end) in zip(functions, resumes):
            if re.findall(GUARDS, body) != [(f'{start:X}', f'{end:X}')]:
                raise ValueError('incorrect exact continuation span')
        # Preserve the SDK's runtime declarations and helper macros exactly.
        preamble = lifted[:lifted.index('\nvoid ' + PREFIX)]
        header = (generated / 'spu_recomp.h').read_text().replace('../../runtime/spu/', '')
    header += '\nextern void spu_begin_image(int);\n'
    (dest / 'spu_recomp.h').write_text(header)
    supplement = preamble.replace('../../runtime/spu/', '') + '\n#include <stdlib.h>\n#include <string.h>\n'
    supplement += '\n\n'.join(functions) + '\n' + registration_source()
    (dest / 'spu_recomp.c').write_text(supplement)
    for name, expected in receipt['outputs'].items():
        if digest(output / name) != expected:
            raise ValueError('existing generated source changed: ' + name)
    if sdk_identity(args.sdk, args.sdk_revision) != pin or digest(provenance) != baseline_sha:
        raise ValueError('input drift')
    receipt['geometry_continuation_extension'] = {
        'preparer_sha256': digest(pathlib.Path(__file__)), 'SDK_pin': pin,
        'baseline_provenance_sha256': baseline_sha, 'image_id': 105,
        'resume_bounds': resumes, 'raw_sha256': RAW_SHA,
        'bound_manifest_sha256': hashlib.sha256(bound_data.encode()).hexdigest(),
        'command': command, 'inherited_output_count': len(receipt['outputs']),
        'runtime_toggle': 'MAG_COMPILED_GEOMETRY=1; otherwise no entries registered',
        'scope': 'Two SDK-generated suffix definitions; original800 bodies and all inherited files unchanged; complete existing105 identity and exact suffix spans retained.'}
    receipt['outputs'].update({str(f.relative_to(output)): digest(f) for f in dest.iterdir() if f.suffix in ('.c', '.h')})
    (output / 'provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'resume_bounds': resumes, 'provenance_sha256': digest(output / 'provenance.json')}, indent=2))


if __name__ == '__main__':
    main()
