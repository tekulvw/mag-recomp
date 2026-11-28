"""Run pinned static analysis tools; never launch the game or emulator."""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, sha256_file, validate_ppu_elf, verify_hash, data_path, load_framework
from tools import prepare_inputs
from tools.targets import (DEFAULT_TARGET, TARGET_NAMES, get_target,
                           validate_target_profile, validate_target_output)

SDK_REVISION = '5244db92669b54831c6d3ebb7a96d58c4dbfa6df'


def check_sdk(sdk_root: Path, expected_revision: str = SDK_REVISION):
    revision = subprocess.check_output(['git', '-C', str(sdk_root), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(sdk_root), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if expected_revision != SDK_REVISION:
        framework=load_framework()[0]
        if expected_revision not in framework.get('allowed_lifter_revisions',[]):
            raise ValueError('Generator revision is not reviewed')
    if revision != expected_revision or dirty:
        raise ValueError('Analysis requires the clean pinned ps3recomp revision')


def summarize_functions(image: str, rows: list) -> dict:
    tocs = sorted({int(r['toc'], 0) for r in rows if r.get('toc') and int(r['toc'], 0)})
    return dict(image=image, function_count=len(rows), tocs=[hex(t) for t in tocs],
                functions_without_toc=sum(not r.get('toc') or int(r['toc'], 0) == 0 for r in rows))


def restrict_to_code(rows: list, ranges: list) -> list:
    result = []
    for row in rows:
        start, end = int(row['start'], 0), int(row['end'], 0)
        for lo, hi in ranges:
            if lo <= start < hi and start < end:
                result.append(dict(row, end=hex(min(end, hi))))
                break
    return result


def inventory_image(elf_path: Path, sdk_root: Path, output_dir: Path,
                    target: str = DEFAULT_TARGET) -> dict:
    definition = get_target(target)
    validate_target_output(output_dir, target, workspace=prepare_inputs.WORKSPACE)
    if target != DEFAULT_TARGET:
        manifest = json.loads(data_path(definition.profile).read_text())
        validate_target_profile(manifest, target)
        image = manifest['images'].get(elf_path.stem)
        if image is None:
            raise ValueError('ELF name does not belong to the selected target')
        verify_hash(elf_path, image['elf_sha256'])
        if output_dir.exists() and any(output_dir.iterdir()):
            raise ValueError('Target inventory requires a fresh output directory')
    check_sdk(sdk_root)
    header = validate_ppu_elf(elf_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    name = elf_path.stem
    commands = [
        ('elf_info.txt', 'elf_parser.py', str(elf_path), '--all'),
        ('loader.log', 'ppu_loader.py', str(elf_path), '-o', str(output_dir)),
        ('find-functions.log', 'find_functions.py', str(elf_path), '--seed-json', str(output_dir/f'{name}.functions.json'), '-o', str(output_dir/'functions.json')),
        ('imports.log', 'gen_imports.py', str(elf_path), '-o', str(output_dir/'named-imports.json')),
        ('spu.log', 'extract_spu_images.py', str(elf_path), '-o', str(output_dir/'spu')),
    ]
    for logfile, tool, *args in commands:
        command = [sys.executable, str(sdk_root/'tools'/tool), *args]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output_dir/logfile).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'{tool} failed; see {output_dir/logfile}')
    seeds = json.loads((output_dir/f'{name}.functions.json').read_text())
    functions = json.loads((output_dir/'functions.json').read_text())
    imports = json.loads((output_dir/'named-imports.json').read_text())
    elf_info = json.loads((output_dir/'elf_info.txt').read_text())
    ranges = [(int(s['addr'], 0), int(s['addr'], 0)+int(s['size'], 0))
              for s in elf_info['section_headers'] if int(s['flags'], 0) & 4 and int(s['size'], 0)]
    if not ranges:
        raise ValueError('No executable section ranges; manual analysis required')
    reviewed = restrict_to_code(functions, ranges)
    (output_dir/'reviewed-functions.json').write_text(json.dumps(reviewed, indent=1)+'\n')
    summary = dict(summarize_functions(name, seeds), elf_sha256=sha256_file(elf_path),
                   manifests=dict(functions_sha256=sha256_file(output_dir/'reviewed-functions.json'),
                                  imports_sha256=sha256_file(output_dir/f'{name}.imports.json')),
                   sdk_revision=SDK_REVISION, header=header, discovered_function_count=len(functions),
                   reviewed_function_count=len(reviewed), code_ranges=ranges, code_end=hex(max(hi for lo, hi in ranges)),
                   imports_by_library={lib: sum(x['lib']==lib for x in imports) for lib in sorted({x['lib'] for x in imports})},
                   spu_elfs=[dict(path=str(p.relative_to(output_dir)), bytes=p.stat().st_size, sha256=sha256_file(p)) for p in sorted((output_dir/'spu').glob('*.elf'))],
                   coverage='Descriptor TOCs; heuristic function discovery; embedded ELF scan excludes raw or compressed SPU jobs.')
    if target != DEFAULT_TARGET:
        summary['target'] = target
    (output_dir/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=os.environ.get('PS3RECOMP_DIR'), required='PS3RECOMP_DIR' not in os.environ,
                        help='ps3recomp checkout (default: $PS3RECOMP_DIR)')
    parser.add_argument('--target', choices=TARGET_NAMES, default=DEFAULT_TARGET)
    args = parser.parse_args()
    definition = get_target(args.target)
    config = json.loads(data_path(definition.profile).read_text())
    validate_target_profile(config, args.target)
    inventory_path = data_path(Path(definition.profile).with_name(f'{args.target}-inventory.json'))
    validate_target_output(inventory_path, args.target, workspace=prepare_inputs.WORKSPACE)
    summaries = {}
    for name, record in config['images'].items():
        path = Path(record['elf']); verify_hash(path, record['elf_sha256'])
        summaries[name] = inventory_image(path, args.sdk, data_path(definition.analysis)/name, target=args.target)
        print(name, summaries[name]['discovered_function_count'], 'functions,', len(summaries[name]['tocs']), 'TOCs,', len(summaries[name]['spu_elfs']), 'SPU ELFs', flush=True)
    inventory_path.write_text(json.dumps(summaries, indent=2)+'\n')
