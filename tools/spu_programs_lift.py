#!/usr/bin/env python3
"""Lift every embedded SPU program of the engine ELF and gate the build on coverage.

Runs the SDK's generic discovery pipeline (build_spu_workloads --input) over the
pinned engine ELF with this title's reviewed decisions from
profiles/retail-v1.0-spu-programs.json: a pinned image id for every program the
pipeline registers, and a reason for every program another path already
provides. The discovered set must equal the audited coverage inventory
(profiles/retail-v1.0-spu-coverage.json) byte for byte, and the pipeline must
report complete coverage, so a new or changed SPU program cannot go
unregistered silently.

  generate: tools/spu_programs_lift.py --sdk <sdk> --sdk-revision <rev> --output local/<dir>
  check:    tools/spu_programs_lift.py --check local/<dir> [--sources-out <file>]   (the build runs this)

Generated code and original bytes stay in the (ignored) output directory.
"""
import argparse, hashlib, json, pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.prepare_inputs import data_path
MANIFEST = ROOT / 'profiles/retail-v1.0-spu-programs.json'
COVERAGE = ROOT / 'profiles/retail-v1.0-spu-coverage.json'
RECEIPT = 'mag-spu-programs.json'
REGISTRY = 'spu_programs.c'
REGISTER_FN = 'mag_spu_programs_register'
SOURCES = 'sources.cmake'
SOURCE_PART = re.compile(r'[A-Za-z0-9._-]+')


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def expected_programs(manifest):
    coverage = json.loads(COVERAGE.read_text())
    carrier = manifest['carrier']
    images = [i for i in coverage['images'] if i['carrier'] == carrier]
    return {i['sha256']: i for i in images}


def verify_inventory(output, manifest):
    inv = json.loads((output / 'inventory.json').read_text())
    if not inv.get('complete') or inv.get('errors'):
        raise ValueError('SPU program coverage is incomplete: ' + json.dumps(
            [dict(sha256=p.get('sha256'), status=p['status'], reason=p.get('reason'))
             for p in inv['programs'] if p['status'] not in ('registered', 'handled')] + inv.get('errors', [])))
    expected = expected_programs(manifest)
    found = {p['sha256']: p for p in inv['programs']}
    if set(found) != set(expected):
        raise ValueError('discovered SPU programs differ from the coverage inventory: missing %s, new %s' % (
            sorted(set(expected) - set(found)), sorted(set(found) - set(expected))))
    for sha, p in found.items():
        e = expected[sha]
        offsets = sorted(o['offset'] for o in p['occurrences'])
        if (p['size'] != e['size'] or offsets != [int(e['file_offset'], 16)] or
                int(p['fingerprint'], 16) != int(e['runtime_fingerprint'], 16)):
            raise ValueError('SPU program identity differs from the coverage inventory: ' + sha)
        if sha in manifest['handled']:
            if p['status'] != 'handled':
                raise ValueError('handled program was not reported handled: ' + sha)
        elif p['status'] != 'registered' or p['image_id'] != manifest['image_ids'][sha]['image_id']:
            raise ValueError('program not registered under its pinned image id: ' + sha)
    for name, h in inv['outputs'].items():
        if digest(output / name) != h:
            raise ValueError('generated output drift: ' + name)
    for p in inv['programs']:
        for name, h in p.get('outputs', {}).items():
            if digest(output / name) != h:
                raise ValueError('generated output drift: ' + name)
    return inv


def check(output):
    output = output.resolve()
    receipt = json.loads((output / RECEIPT).read_text())
    if receipt['manifest_sha256'] != digest(MANIFEST) or receipt['coverage_sha256'] != digest(COVERAGE):
        raise ValueError('SPU program manifest or coverage inventory changed since generation; regenerate')
    if receipt['inventory_sha256'] != digest(output / 'inventory.json'):
        raise ValueError('inventory.json changed since generation')
    inv = verify_inventory(output, json.loads(MANIFEST.read_text()))
    return receipt, checked_sources(output, inv)


def checked_sources(output, inv):
    """The generated sources, located inside the checked directory itself.

    sources.cmake records absolute paths where the lift ran. A directory copied
    to another workspace or host must compile its own verified files, so each
    recorded entry is matched to exactly one inventory source by its path
    within the directory, keeping the recorded order."""
    names = [p['name'] for p in inv['programs'] if p['status'] == 'registered']
    expected = [name + '/spu_recomp.c' for name in names] + [REGISTRY]
    for path in expected:
        if not all(SOURCE_PART.fullmatch(part) and part not in ('.', '..') for part in path.split('/')):
            raise ValueError('unexpected generated source name: ' + path)
    recorded = re.findall(r'\[==\[(.*?)\]==\]', (output / SOURCES).read_text())
    ordered = []
    for old in recorded:
        parts = pathlib.PurePosixPath(old).parts
        match = [new for new in expected if parts[-len(new.split('/')):] == tuple(new.split('/'))]
        if len(match) != 1:
            raise ValueError(SOURCES + ' does not list the inventory sources: ' + old)
        ordered.append(match[0])
    if sorted(ordered) != sorted(expected):
        raise ValueError(SOURCES + ' does not list the inventory sources')
    for path in ordered:
        if not (output / path).is_file():
            raise ValueError('generated source missing: ' + path)
    return [output / path for path in ordered]


def write_sources(path, sources):
    # Bracket arguments preserve spaces and avoid expansion of $ in paths.
    path.write_text('set(SPU_GENERATED_SOURCES\n' + ''.join(f'  [==[{p.as_posix()}]==]\n' for p in sources) + ')\n')


def generate(sdk, revision, output):
    manifest = json.loads(MANIFEST.read_text())
    image = json.loads((data_path('profiles/retail-v1.0.json')).read_text())['images'][manifest['carrier']]
    elf = pathlib.Path(image['elf'])
    if digest(elf) != manifest['elf_sha256'] or image['elf_sha256'] != manifest['elf_sha256']:
        raise ValueError('engine ELF identity differs from the SPU program manifest')
    head = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(sdk), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if head != revision or dirty:
        raise ValueError('SDK revision or tracked cleanliness differs')
    if output.exists():
        raise ValueError('output must be fresh: ' + str(output))
    inputs = output.with_name(output.name + '.inputs')
    inputs.mkdir(parents=True, exist_ok=False)
    (inputs / 'handled.json').write_text(json.dumps({k: v['reason'] for k, v in manifest['handled'].items()}, indent=2))
    (inputs / 'image-ids.json').write_text(json.dumps({k: v['image_id'] for k, v in manifest['image_ids'].items()}, indent=2))
    command = [sys.executable, str(sdk / 'tools/build_spu_workloads.py'), '--input', str(elf),
               '--lifted', str(output), '--out', str(output / REGISTRY), '--register-fn', REGISTER_FN,
               '--constructor', '--handled', str(inputs / 'handled.json'),
               '--image-ids', str(inputs / 'image-ids.json')]
    result = subprocess.run(command, capture_output=True, text=True)
    (inputs / 'generation.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError('SPU program generation failed; see ' + str(inputs / 'generation.log'))
    inv = verify_inventory(output, manifest)
    receipt = dict(sdk_revision=head, lifter_sha256=digest(sdk / 'tools/spu_lifter.py'),
                   elf_sha256=manifest['elf_sha256'], manifest_sha256=digest(MANIFEST),
                   coverage_sha256=digest(COVERAGE), inventory_sha256=digest(output / 'inventory.json'),
                   command=command[1:],
                   programs=[dict(role=(manifest['image_ids'].get(p['sha256']) or manifest['handled'][p['sha256']])['role'],
                                  sha256=p['sha256'], status=p['status'], image_id=p.get('image_id'),
                                  fingerprint=p['fingerprint'],
                                  functions=len(p.get('diagnostics', {}).get('functions', [])),
                                  unsupported=len(p.get('diagnostics', {}).get('unsupported', [])),
                                  reason=p.get('reason'))
                             for p in inv['programs']])
    (output / RECEIPT).write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', type=pathlib.Path)
    ap.add_argument('--sources-out', type=pathlib.Path,
                    help='with --check: write the checked directory\'s source list as CMake')
    ap.add_argument('--sdk', type=pathlib.Path)
    ap.add_argument('--sdk-revision')
    ap.add_argument('--output', type=pathlib.Path)
    a = ap.parse_args()
    try:
        if a.check:
            receipt, sources = check(a.check)
            if a.sources_out:
                write_sources(a.sources_out, sources)
            print(f"SPU program coverage complete: {sum(p['status'] == 'registered' for p in receipt['programs'])} "
                  f"registered, {sum(p['status'] == 'handled' for p in receipt['programs'])} handled")
        else:
            if not (a.sdk and a.sdk_revision and a.output):
                ap.error('--sdk, --sdk-revision and --output are required to generate')
            receipt = generate(a.sdk.resolve(), a.sdk_revision, a.output.resolve())
            for p in receipt['programs']:
                print(f"{p['status']:10} {str(p['image_id'] or '-'):>4} {p['functions']:5} fn "
                      f"{p['unsupported']} unsupported  {p['role']}")
    except (ValueError, OSError, KeyError) as error:
        print('[spu_programs_lift] ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
