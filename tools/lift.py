"""Generate one image into a fresh directory and retain the audit evidence."""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, sha256_file, verify_hash, data_path, load_framework
from tools.inventory import check_sdk, SDK_REVISION
from tools.movie_picture_hooks import instrument_source, RETAIL_ELF
from tools.graphics_trace_hooks import instrument_source as instrument_graphics


def guard_generated_source(source: str) -> str:
    source = '#include <stdio.h>\n#include <stdlib.h>\n' + source
    source = re.sub(r'(/\* (?:TODO:.*?|unresolved target.*?) \*/);?',
                    lambda m: m[1] + ' do { fprintf(stderr, "[mag] unsupported lifted code in %s\\n", __func__); _Exit(86); } while (0);', source)
    entry = 'void func_00010230(ppu_context* ctx) {'
    source = source.replace(entry, entry + '\n        fprintf(stderr, "[mag] retail_entry 0x00010230\\n");', 1)
    return source


def run_generator(command: list, output_dir: Path) -> dict:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise FileExistsError(f'Use a fresh output directory: {output_dir}')
    output_dir.mkdir(parents=True, exist_ok=True)
    with (output_dir/'generation.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode or not (output_dir/'ppu_recomp.h').is_file() or not list(output_dir.glob('*.cpp')) + list(output_dir.glob('*.c')):
        raise RuntimeError(f'Generation incomplete (exit {result.returncode}); see {output_dir}/generation.log')
    return dict(command=command, exit_code=result.returncode)


def validate_nonlocal_profile(profile_path: Path, sdk_root: Path, elf_path: Path, image_name: str) -> tuple[Path, dict]:
    """Keep optional control-flow configuration tied to one reviewed image/tool."""
    profile=json.loads(profile_path.read_text())
    if profile.get('image_name') != image_name:
        raise ValueError('Nonlocal profile belongs to a different image')
    verify_hash(elf_path,profile['elf_sha256'])
    name=profile['configuration']
    if Path(name).name != name or not name:
        raise ValueError('Nonlocal configuration must be a local basename')
    config=profile_path.parent/name
    if config.is_symlink() or not config.is_file():
        raise ValueError('Nonlocal configuration must be a regular local file')
    verify_hash(config,profile['configuration_sha256'])
    framework=load_framework()[0]
    revision=subprocess.check_output(['git','-C',str(sdk_root),'rev-parse','HEAD'],text=True).strip()
    dirty=subprocess.check_output(['git','-C',str(sdk_root),'status','--porcelain','--untracked-files=no'],text=True)
    if revision != profile['sdk_revision'] or revision not in framework.get('allowed_lifter_revisions',[]) or dirty:
        raise ValueError('Nonlocal generation requires its clean reviewed SDK revision')
    return config,profile


def generate_lift(elf_path: Path, functions_path: Path, imports_path: Path, sdk_root: Path, output_dir: Path, nonlocal_profile: Path | None = None) -> dict:
    config=None; control_profile=None; generator_revision=SDK_REVISION
    if nonlocal_profile is None:
        check_sdk(sdk_root)
    else:
        config,control_profile=validate_nonlocal_profile(nonlocal_profile,sdk_root,elf_path,elf_path.stem)
        generator_revision=control_profile['sdk_revision']
    manifest = json.loads((data_path('profiles/retail-v1.0.json')).read_text())
    image = manifest['images'][elf_path.stem]
    verify_hash(elf_path, image['elf_sha256'])
    summary = json.loads((functions_path.parent/'summary.json').read_text())
    if summary['elf_sha256'] != image['elf_sha256'] or summary['sdk_revision'] != SDK_REVISION:
        raise ValueError('Inventory identity does not match current image/framework')
    manifests=summary.get('manifests',{})
    if not manifests.get('functions_sha256') or not manifests.get('imports_sha256'):
        raise ValueError('Inventory is missing function/import manifest hashes; regenerate it')
    verify_hash(functions_path, manifests['functions_sha256'])
    verify_hash(imports_path, manifests['imports_sha256'])
    result = run_generator([sys.executable, str(sdk_root/'tools/ppu_lifter.py'), str(elf_path),
                            '--functions', str(functions_path), '--hle-stubs', str(imports_path),
                            '--code-end', summary['code_end'], '--jobs', '2', '--output', str(output_dir)] +
                           (['--nonlocal-control-flow',str(config)] if config else []), output_dir)
    result.update(elf_sha256=sha256_file(elf_path), functions_sha256=sha256_file(functions_path),
                  imports_sha256=sha256_file(imports_path), sdk_revision=generator_revision,
                  analysis_sdk_revision=SDK_REVISION)
    if control_profile is not None:
        result['nonlocal_control_flow']=control_profile
        result['nonlocal_profile_sha256']=sha256_file(nonlocal_profile)
    for path in output_dir.glob('ppu_recomp_*.cpp'):
        source=guard_generated_source(path.read_text())
        path.write_text(instrument_graphics(instrument_source(source)) if image['elf_sha256']==RETAIL_ELF else source)
    result['postprocessing'] = 'Fail at executed TODO/unresolved code; log verified retail entry function; tools/lift.py'
    if image['elf_sha256']==RETAIL_ELF:
        result['movie_picture_hooks']=dict(tool_sha256=sha256_file(ROOT/'tools/movie_picture_hooks.py'),elf_sha256=RETAIL_ELF)
    result['generator_sha256'] = sha256_file(Path(__file__))
    result['outputs'] = {p.name:sha256_file(p) for pattern in ('*.c','*.cpp','*.h')
                         for p in sorted(output_dir.glob(pattern))}
    audit_cmd = [sys.executable, str(sdk_root/'tools/lift_audit.py'), '--elf', str(elf_path),
                 '--functions', str(functions_path), '--recomp-dir', str(output_dir),
                 '--baseline', str(output_dir/'audit-baseline.txt'), '--update-baseline']
    with (output_dir/'audit.log').open('w') as log:
        audit = subprocess.run(audit_cmd, stdout=log, stderr=subprocess.STDOUT)
    result.update(audit_command=audit_cmd, audit_exit=audit.returncode,
                  status='complete' if audit.returncode == 0 else 'failed',
                  audit_meaning='Census baseline only, not an assertion of correctness or zero hazards.')
    (output_dir/'provenance.json').write_text(json.dumps(result, indent=2)+'\n')
    if audit.returncode:
        raise RuntimeError('Lift audit failed; inspect audit.log')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', choices=['EBOOT','mage_g'], default='EBOOT')
    parser.add_argument('--sdk', type=Path, default=os.environ.get('PS3RECOMP_DIR'), required='PS3RECOMP_DIR' not in os.environ,
                        help='ps3recomp checkout (default: $PS3RECOMP_DIR)')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--nonlocal-profile', type=Path, help='Exact-image configuration for reviewed nonlocal control flow')
    a=parser.parse_args()
    image=json.loads((data_path('profiles/retail-v1.0.json')).read_text())['images'][a.image]
    analysis=data_path('analysis/retail-v1.0')/a.image
    out=a.output or data_path('generated/retail-v1.0')/a.image
    generate_lift(Path(image['elf']), analysis/'reviewed-functions.json', analysis/f'{a.image}.imports.json', a.sdk, out, a.nonlocal_profile)
    print('Generated and audited', out)
