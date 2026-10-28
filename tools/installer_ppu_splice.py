"""Reproduce a profile-bound engine dispatcher fix using already pinned SDKs.

No generated body is bundled. The original decoder is shared by both runs;
only the lifter changes. Keep the complete reviewed function inventory because
jump-table discovery uses neighbouring function boundaries. This deliberately
avoids a single-function inventory that could change dispatcher extents.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.partial_ppu_relift import pinned_blob
from tools.ppu_function_splice import functions


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require_hash(path, expected):
    if digest(path) != expected:
        raise ValueError('Splice input identity mismatch: ' + str(path))


def verify_result(output, profile):
    record = json.loads((output / 'provenance.json').read_text())
    splice = record['function_splices'][-1]
    if splice['changed'] != [profile['function']] or splice['added']:
        raise ValueError('Splice changed functions outside the approved profile')
    body = functions(output)[profile['function']][1]
    found = hashlib.sha256(body.encode()).hexdigest()
    if found != profile['body_sha256'] or splice['body_sha256'] != {profile['function']: found}:
        raise ValueError('Regenerated dispatcher does not match its reviewed body identity')
    return record


def prepare(base, output, work, elf, inventory, nonlocal_profile, before_sdk, after_sdk, profile):
    """Fresh generated output; all game-derived intermediates stay in private work."""
    base, output, work, elf, inventory, nonlocal_profile, before_sdk, after_sdk = map(
        Path, (base, output, work, elf, inventory, nonlocal_profile, before_sdk, after_sdk))
    if output.exists() or output.is_symlink() or work.exists() or work.is_symlink():
        raise ValueError('Splice output and work directories must be fresh')
    require_hash(elf, profile['elf_sha256'])
    summary = json.loads((inventory / 'summary.json').read_text())
    if summary['elf_sha256'] != profile['elf_sha256']:
        raise ValueError('Splice inventory belongs to another executable')
    manifest = summary['manifests']
    funcs = inventory / 'reviewed-functions.json'; imports = inventory / 'mage_g.imports.json'
    require_hash(funcs, manifest['functions_sha256']); require_hash(imports, manifest['imports_sha256'])
    control = json.loads(nonlocal_profile.read_text())
    if control['elf_sha256'] != profile['elf_sha256'] or control['sdk_revision'] != profile['before']['revision']:
        raise ValueError('Splice nonlocal profile belongs to another image or base lifter')
    config = nonlocal_profile.parent / control['configuration']
    if Path(control['configuration']).name != control['configuration'] or config.is_symlink():
        raise ValueError('Splice nonlocal configuration must be an ordinary sibling file')
    require_hash(config, control['configuration_sha256'])
    provenance = json.loads((base / 'provenance.json').read_text())
    for key, expected in (('elf_sha256', profile['elf_sha256']),
                          ('functions_sha256', manifest['functions_sha256']),
                          ('imports_sha256', manifest['imports_sha256']),
                          ('sdk_revision', profile['before']['revision'])):
        if provenance.get(key) != expected:
            raise ValueError('Splice base provenance differs from reviewed generation inputs: ' + key)
    decoder = pinned_blob(before_sdk, profile['before']['revision'], 'tools/ppu_disasm.py', profile['decoder_sha256'])
    lifters = {name: pinned_blob(sdk, profile[name]['revision'], 'tools/ppu_lifter.py', profile[name]['lifter_sha256'])
               for name, sdk in (('before', before_sdk), ('after', after_sdk))}
    work.mkdir(parents=True)
    commands = {}
    with tempfile.TemporaryDirectory(prefix='dispatcher-relift-', dir=output.parent) as temporary:
        temporary = Path(temporary)
        (temporary / 'ppu_disasm.py').write_bytes(decoder)
        for name in ('before', 'after'):
            lifter = temporary / ('lifter_' + name + '.py'); lifter.write_bytes(lifters[name])
            commands[name] = [sys.executable, str(lifter), str(elf), '--functions', str(funcs),
                              '--hle-stubs', str(imports), '--code-end', str(summary['code_end']),
                              '--jobs', '2', '--nonlocal-control-flow', str(config), '--output', str(temporary / name)]
            with (work / (name + '.log')).open('w') as log:
                subprocess.run(commands[name], stdout=log, stderr=subprocess.STDOUT, check=True)
        command = [sys.executable, str(Path(__file__).with_name('ppu_function_splice.py')),
                   '--base', str(base), '--before', str(temporary / 'before'), '--after', str(temporary / 'after'),
                   '--output', str(output), '--sdk', str(after_sdk), '--before-revision', profile['before']['revision'],
                   '--after-revision', profile['after']['revision'], '--only', profile['function'], '--reason', profile['reason']]
        with (work / 'splice.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        record = verify_result(output, profile)
        record['installer_dispatcher_splice'] = {'profile': profile, 'commands': commands,
                                                'nonlocal_profile_sha256': digest(nonlocal_profile),
                                                'decoder_source_revision': profile['before']['revision']}
        (output / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('base', 'output', 'work', 'elf', 'inventory', 'nonlocal-profile', 'before-sdk', 'after-sdk', 'profile'):
        parser.add_argument('--' + key, type=Path, required=True)
    args = vars(parser.parse_args())
    args['profile'] = json.loads(args['profile'].read_text())
    prepare(**args)


if __name__ == '__main__':
    main()
