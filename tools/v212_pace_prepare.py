"""Prepare the hash-pinned original 2.12 libpace PRX; never launch the game.

The reviewed SDK supplies the PRX parser and PPU lifter. This adapter adds the
ADDR64 relocation used by libpace, with strict segment bounds. Relocation types
follow RPCS3 PPUModule.cpp (R_PPC64_ADDR64 writes the complete resolved value):
https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/PPUModule.cpp
Original images and generated sources stay in the ignored workspace.
"""
import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, WORKSPACE, data_path, load_framework, sha256_file, verify_hash
from tools.inventory import check_sdk
from tools.lift import guard_generated_source
from tools.targets import validate_target_output, validate_target_profile

BASE = 0x21200000
WINDOW = 0x100000
ROLE = 'lifter-ppu-v212'
TARGET = 'retail-v2.12'


def validate_placement(base, image_size):
    if base != BASE or not 0 < image_size <= WINDOW:
        raise ValueError('libpace must fit its reviewed 0x21200000 1 MiB reservation')


def relocate_prx(prx, base):
    """Relocate only reviewed types; refuse invalid sources, targets and widths."""
    loads = prx.loads
    if not loads:
        raise ValueError('PRX has no load segments')
    top = max(v + m for _, v, _, m in loads)
    validate_placement(base, top)
    image = bytearray(top)
    for off, va, filesz, memsz in loads:
        if min(off, va, filesz, memsz) < 0 or filesz > memsz or off + filesz > len(prx.data):
            raise ValueError('Invalid PRX load segment')
        image[va:va+filesz] = prx.data[off:off+filesz]
    if not prx.rela:
        raise ValueError('PRX is missing relocations')
    ro, size = prx.rela
    if size % 24 or ro < 0 or ro + size > len(prx.data):
        raise ValueError('Invalid PRX relocation table')
    counts = Counter()
    for offset in range(ro, ro+size, 24):
        at, _, value_index, address_index, kind, ptr = struct.unpack_from('>QHBBIQ', prx.data, offset)
        width = {1: 4, 4: 2, 5: 2, 6: 2, 38: 8}.get(kind)
        if width is None:
            raise ValueError(f'Unsupported PRX relocation type {kind}')
        if address_index >= len(loads) or (value_index != 255 and value_index >= len(loads)):
            raise ValueError('PRX relocation references an invalid segment')
        _, target_va, _, target_size = loads[address_index]
        if at + width > target_size or at % min(width, 4):
            raise ValueError('PRX relocation target is outside its segment or misaligned')
        target = target_va + at
        value = ptr + (0 if value_index == 255 else base + loads[value_index][1])
        if value >= 1 << (64 if kind == 38 else 32):
            raise ValueError('PRX relocation value exceeds its address width')
        if kind == 1:
            struct.pack_into('>I', image, target, value)
        elif kind == 38:
            struct.pack_into('>Q', image, target, value)
        else:
            half = value if kind == 4 else (value >> 16 if kind == 5 else (value+0x8000) >> 16)
            struct.pack_into('>H', image, target, half & 0xffff)
        counts[kind] += 1
    return bytes(image), dict(counts)


def import_stubs(image, imports, base):
    """Keep the original code-stub EAs supplied by the reviewed PRX parser."""
    result = []
    for library, entries in imports.items():
        for nid, stub in entries.items():
            code = int(stub, 0)
            if not base <= code < base + len(image) or code % 4:
                raise ValueError('Import code is outside the module image')
            result.append(dict(library=library, nid=nid, stub=hex(code)))
    return result


REFERENCE_LIMIT = 4096


def import_references(image, base, module_info_offset):
    """Read original linkage chains independently of function-stub extraction.

    The pinned primary module uses only ADDR32 records with zero addends.
    Function linkage values remain OPD addresses, rather than their code words.
    Unknown layouts, TLS, relocation types and addends fail explicitly.
    """
    def region(ea, size):
        offset=ea-base
        if not isinstance(ea,int) or offset<0 or size<0 or offset>len(image) or size>len(image)-offset:
            raise ValueError('Import linkage range is outside the original module image')
        return offset
    def word(ea):
        return struct.unpack_from('>I',image,region(ea,4))[0]
    def string(ea):
        begin=region(ea,1)
        end=image.find(b'\0',begin,min(len(image),begin+128))
        if end<=begin:
            raise ValueError('Import library name is empty or unterminated')
        try:return image[begin:end].decode('ascii')
        except UnicodeDecodeError as exc:raise ValueError('Unsupported import library name') from exc
    mi=region(base+module_info_offset,0x34)
    start,end=struct.unpack_from('>II',image,mi+0x2c)
    begin=region(start,0);finish=region(end,0)
    if finish<begin:
        raise ValueError('Import descriptor range is reversed')
    refs=[];seen_targets=set();seen_libraries=set()
    summary=dict(descriptor_count=0,function_imports=0,variable_imports=0,tls_imports=0,
                 function_references=0,variable_references=0)
    def chain(library,nid,kind,pointer,descriptor):
        if not pointer:return
        for index in range(REFERENCE_LIMIT+1):
            at=pointer+index*12
            kind_word=word(at)
            if kind_word==0:return
            region(at,12)
            target,addend=word(at+4),word(at+8)
            if kind_word!=1 or addend:
                raise ValueError('Unsupported import reference type or addend')
            if len(refs)==REFERENCE_LIMIT or target%4:
                raise ValueError('Import reference count or alignment invalid')
            expected=word(target)
            if target in seen_targets:
                raise ValueError('Duplicate import reference target')
            seen_targets.add(target)
            refs.append(dict(library=library,nid=f'0x{nid:08X}',kind=kind,type=kind_word,
                             target=f'0x{target:08X}',addend=addend,expected=f'0x{expected:08X}',
                             descriptor=f'0x{descriptor:08X}',chain=f'0x{pointer:08X}'))
            summary[kind+'_references']+=1
        raise ValueError('Import reference chain has no bounded terminator')
    offset=begin
    while offset<finish:
        if summary['descriptor_count']==128 or offset+44>finish or (image[offset] or 44)!=44:
            raise ValueError('Unsupported or truncated import descriptor layout')
        version,attributes,functions,variables,tls=struct.unpack_from('>HHHHH',image,offset+2)
        if tls or attributes & ~0x2009:
            raise ValueError('Unsupported import attributes or TLS imports')
        name,nids,stubs,vnids,vrefs,extra1,extra2=struct.unpack_from('>IIIIIII',image,offset+16)
        if extra1 or extra2:
            raise ValueError('Unsupported extra import tables')
        library=string(name)
        if library in seen_libraries:
            raise ValueError('Duplicate import library descriptor')
        seen_libraries.add(library)
        summary['descriptor_count']+=1;summary['function_imports']+=functions
        summary['variable_imports']+=variables;summary['tls_imports']+=tls
        if summary['function_imports']+summary['variable_imports']>65536:
            raise ValueError('Import symbol count exceeds bound')
        if functions:
            region(nids,functions*(8 if attributes&0x2000 else 4));region(stubs,functions*4)
        used=set()
        for i in range(functions):
            nid=word(nids+i*4)
            if not nid or nid in used:
                raise ValueError('Invalid or duplicate function import NID')
            used.add(nid)
            if attributes&0x2000:chain(library,nid,'function',word(nids+(functions+i)*4),base+offset)
        if variables:region(vnids,variables*4);region(vrefs,variables*4)
        used=set()
        for i in range(variables):
            nid=word(vnids+i*4);pointer=word(vrefs+i*4)
            if not nid or nid in used or not pointer:
                raise ValueError('Invalid variable import or missing reference chain')
            used.add(nid);chain(library,nid,'variable',pointer,base+offset)
        offset+=44
    return refs,summary


def _json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def _profile():
    path = ROOT/'profiles/retail-v2.12-pace.json'
    profile = json.loads(path.read_text())
    if (profile.get('target') != TARGET or profile.get('base') != BASE or
            profile.get('reserved_bytes') != WINDOW or profile.get('lifter_role') != ROLE):
        raise ValueError('libpace profile does not describe the reviewed target reservation and SDK role')
    return profile, path


def _validate_title_placement():
    machine = json.loads(data_path('profiles/retail-v2.12.json').read_text())
    validate_target_profile(machine, TARGET)
    for record in machine['images'].values():
        for segment in record['load_segments']:
            if segment['vaddr'] < BASE+WINDOW and segment['vaddr']+segment['memsz'] > BASE:
                raise ValueError('libpace reservation overlaps a title load segment')


def _artifact_set(output, suffixes):
    result = {}
    for path in output.rglob('*'):
        if path.is_symlink():
            raise ValueError('Prepared libpace artifacts cannot contain symlinks')
        if path.is_file() and path.suffix in suffixes:
            result[path.relative_to(output).as_posix()] = path
    return result


def check(output: Path, sdk: Path | None = None):
    """Verify a complete primary-module preparation without executing guest code."""
    output = output.resolve()
    validate_target_output(output, TARGET, workspace=WORKSPACE)
    profile, profile_path = _profile()
    selected = profile['variants']['libpace_g']
    receipt_path = output/'provenance.json'
    if receipt_path.is_symlink():
        raise ValueError('Prepared libpace provenance must be an ordinary file')
    receipt = json.loads(receipt_path.read_text())
    framework, framework_path = load_framework()
    pin = framework['ps3recomp'][ROLE]
    expected = dict(status='complete', target=TARGET, variant='libpace_g',
                    elf_sha256=selected['elf_sha256'], self_sha256=selected['self_sha256'],
                    profile_sha256=sha256_file(profile_path), generator_sha256=sha256_file(Path(__file__)),
                    framework_sha256=sha256_file(framework_path), sdk_revision=pin['commit'],
                    lifter_role=ROLE, sdk_tools=profile['sdk_tools'], executed_unknown_policy='halt',
                    runtime_verified=False)
    for key, value in expected.items():
        if key not in receipt or receipt[key] != value or (key == 'runtime_verified' and receipt[key] is not False):
            raise ValueError('Prepared libpace identity/policy mismatch: '+key)
    validate_placement(receipt.get('base'), receipt.get('image_size', 0))
    if receipt['image_size'] != selected['image_size']:
        raise ValueError('Prepared libpace image size does not match the reviewed primary module')
    _validate_title_placement()
    sdk = sdk or data_path(pin['path'])
    check_sdk(sdk, expected_revision=pin['commit'], role=ROLE)
    for name, digest in profile['sdk_tools'].items():
        verify_hash(sdk/name, digest)
    outputs = _artifact_set(output, ('.c', '.cpp', '.h'))
    names = set(outputs)
    fixed = {'pace_module.cpp', 'pace_image.c', 'mag_pace_recomp.h'}
    chunks = sorted(name for name in names if re.fullmatch(r'ppu_recomp_[0-9]{3}\.cpp', name))
    if (not chunks or names != fixed | set(chunks) or
            chunks != [f'ppu_recomp_{i:03d}.cpp' for i in range(len(chunks))] or
            set(receipt.get('outputs', {})) != names):
        raise ValueError('Prepared libpace source/header set is incomplete or contains unexpected sources')
    for name, path in outputs.items():
        verify_hash(path, receipt['outputs'][name])
    inputs = {p.relative_to(output).as_posix(): p for p in (output/'inputs').iterdir() if p.is_file()}
    required = {'inputs/'+name for name in ('pace_image.bin', 'pace_functions.json', 'pace_exports.json',
                                           'pace_imports.json', 'hle-stubs.json', 'import-references.json')}
    if set(inputs) != required or set(receipt.get('inputs', {})) != required:
        raise ValueError('Prepared libpace input evidence is incomplete or unexpected')
    for name, path in inputs.items():
        verify_hash(path, receipt['inputs'][name])
    image = output/'inputs/pace_image.bin'
    if image.stat().st_size != receipt['image_size']:
        raise ValueError('Prepared libpace relocated image size changed')
    verify_hash(image, selected['relocated_image_sha256'])
    references,summary=import_references(image.read_bytes(),BASE,selected['module_info_offset'])
    linkage=json.loads((output/'inputs/import-references.json').read_text())
    expected_linkage=dict(format=1,module_info_offset=selected['module_info_offset'],summary=summary,references=references)
    if (linkage!=expected_linkage or summary!=selected['import_linkage'] or
            receipt.get('import_linkage')!=summary or receipt.get('module_info_offset')!=selected['module_info_offset']):
        raise ValueError('Prepared libpace import linkage metadata changed or is incomplete')
    counts = Counter()
    for name in chunks:
        text = outputs[name].read_text()
        todos = re.findall(r'/\* TODO: (\S+)', text)
        counts.update(todos)
        if (text.count('_Exit(86);') != len(todos) or '/* unresolved target ' in text or
                '/* missing continuation ' in text):
            raise ValueError('Prepared libpace has unguarded unknown instructions or unresolved control flow')
    if dict(counts) != receipt.get('todo_by_mnemonic'):
        raise ValueError('Prepared libpace instruction census changed')
    return receipt, sorted(path for path in outputs.values() if path.suffix in ('.c', '.cpp'))


def write_sources(path: Path, output: Path, sources):
    """Write only validated preparation paths for CMake's explicit source list."""
    output = output.resolve()
    path = path.resolve()
    validate_target_output(path, TARGET, workspace=WORKSPACE)
    if path.is_relative_to(output) and path != output/'sources.cmake':
        raise ValueError('CMake source listing cannot overwrite prepared libpace artifacts')
    def quoted(value):
        value = Path(value).resolve().as_posix()
        if '\n' in value or '\r' in value:
            raise ValueError('CMake artifact paths cannot contain newlines')
        return '"'+value.replace('\\', '\\\\').replace('"', '\\"').replace(';', '\\;').replace('$', '\\$')+'"'
    lines = ['set(MAG_V212_PACE_SOURCES']+[f'  {quoted(source)}' for source in sources]+[')',
             'set(MAG_V212_PACE_INCLUDE_DIRS '+quoted(output)+')',
             'set(MAG_V212_PACE_PROVENANCE '+quoted(output/'provenance.json')+')']
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines)+'\n')


def generate(source: Path, output: Path, sdk: Path | None = None, variant='libpace_g'):
    profile, profile_path = _profile()
    selected = profile['variants'][variant]
    verify_hash(source, selected['elf_sha256'])
    validate_target_output(output, TARGET, workspace=WORKSPACE)
    framework, framework_path = load_framework()
    pin = framework['ps3recomp'][ROLE]
    sdk = sdk or data_path(pin['path'])
    check_sdk(sdk, expected_revision=pin['commit'], role=ROLE)
    for name, digest in profile['sdk_tools'].items():
        verify_hash(sdk/name, digest)
    spec = importlib.util.spec_from_file_location('mag_v212_pace_prx_parser', sdk/'tools/lift_prx.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    prx = module.Prx(str(source))
    if struct.unpack_from('>H', prx.data, 16)[0] != 0xffa4:
        raise ValueError('Expected an original decrypted relocatable PRX')
    image, relocations = relocate_prx(prx, BASE)
    name, toc, exports, imports = prx.parse_modinfo(image, BASE)
    module_info_offset=next(va+prx.modinfo_off-off for off,va,size,_ in prx.loads
                            if off<=prx.modinfo_off and prx.modinfo_off+0x34<=off+size)
    references,summary=import_references(image,BASE,module_info_offset)
    if variant=='libpace_g' and (module_info_offset!=selected['module_info_offset'] or summary!=selected['import_linkage']):
        raise ValueError('Original libpace import linkage differs from the reviewed primary profile')
    if name != 'pace' or BASE+toc != profile['toc']:
        raise ValueError('Unexpected libpace module name or TOC')
    code_end = BASE + selected['code_end_offset']
    nids = set(exports.get('libpace', {}))
    if len(nids) != 1051 or not set(profile['required_nids']) <= nids:
        raise ValueError('The module does not provide the reviewed libpace export surface')
    _validate_title_placement()
    stubs = import_stubs(image, imports, BASE)
    if len(stubs)!=summary['function_imports']:
        raise ValueError('Function import parser dropped or duplicated original entries')
    seeds = set()
    for group in exports.values():
        for opd in group.values():
            at = int(opd, 0)-BASE
            if not 0 <= at <= len(image)-8:
                raise ValueError('Export descriptor is outside the module')
            seeds.add(struct.unpack_from('>I', image, at)[0])
    for at in range(0, len(image)-7, 4):
        code, candidate_toc = struct.unpack_from('>II', image, at)
        if candidate_toc == BASE+toc and BASE <= code < code_end and not code % 4:
            seeds.add(code)
    if any(not BASE <= address < code_end or address % 4 for address in seeds):
        raise ValueError('Export code is outside the reviewed executable extent')
    functions = [dict(start=hex(start), end=hex(end)) for start, end in
                 zip(sorted(seeds), sorted(seeds)[1:]+[code_end])]
    output.mkdir(parents=True, exist_ok=False)
    inputs = output/'inputs'; inputs.mkdir()
    (inputs/'pace_image.bin').write_bytes(image)
    _json(inputs/'pace_functions.json', functions)
    _json(inputs/'pace_exports.json', dict(module=name, toc=hex(BASE+toc), exports=exports))
    _json(inputs/'pace_imports.json', imports)
    _json(inputs/'hle-stubs.json', stubs)
    _json(inputs/'import-references.json',dict(format=1,module_info_offset=module_info_offset,summary=summary,references=references))
    command = [sys.executable, str(sdk/'tools/ppu_lifter.py'), str(inputs/'pace_image.bin'),
               '--raw', '--base', hex(BASE), '--toc', hex(BASE+toc), '--code-end', hex(code_end),
               '--functions', str(inputs/'pace_functions.json'), '--hle-stubs', str(inputs/'hle-stubs.json'),
               '--symbol-prefix', 'mag_pace_', '--header-name', 'mag_pace_recomp.h',
               '--jobs', '2', '--output', str(output)]
    record = dict(status='failed', target=TARGET, variant=variant, elf_sha256=selected['elf_sha256'],
                  self_sha256=selected['self_sha256'], profile_sha256=sha256_file(profile_path),
                  generator_sha256=sha256_file(Path(__file__)), sdk_revision=pin['commit'], lifter_role=ROLE,
                  sdk_tools=profile['sdk_tools'],
                  framework_sha256=sha256_file(framework_path), base=BASE, image_size=len(image),
                  relocations=relocations, command=command,
                  module_info_offset=module_info_offset,import_linkage=summary,
                  executed_unknown_policy='halt', runtime_verified=False)
    try:
        with (output/'generation.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        sources = sorted(output.glob('ppu_recomp_*.cpp'))
        if not sources or not (output/'mag_pace_recomp.h').is_file():
            raise ValueError('PPU generation produced no module sources')
        counts = Counter()
        for path in sources:
            code = path.read_text()
            counts.update(re.findall(r'/\* TODO: (\S+)', code))
            if '/* unresolved target ' in code or '/* missing continuation ' in code:
                raise ValueError('Module generation has unresolved targets or continuations')
            path.write_text(guard_generated_source(code, retail_entry=False))
        record['todo_by_mnemonic'] = dict(counts)
        _write_module(output, image, exports, selected, references)
        record['outputs'] = {p.name: sha256_file(p) for p in sorted(output.iterdir())
                             if p.suffix in ('.c', '.cpp', '.h')}
        record['inputs'] = {p.relative_to(output).as_posix(): sha256_file(p) for p in sorted(inputs.iterdir())}
        record['status'] = 'complete'
    finally:
        _json(output/'provenance.json', record)
    if variant == 'libpace_g':
        checked, sources = check(output, sdk)
        write_sources(output/'sources.cmake', output, sources)
        return checked
    return record


def _write_module(output, image, exports, selected, references):
    data = ',\n'.join(','.join(f'0x{x:02x}' for x in image[i:i+16]) for i in range(0,len(image),16))
    (output/'pace_image.c').write_text('#include <stdint.h>\nconst uint8_t mag_pace_image[] = {\n'+data+'\n};\n')
    entries = '\n'.join(f'    {{{int(nid,0)}u, {int(opd,0)-BASE}u}},'
                         for nid, opd in exports['libpace'].items())
    system = exports['!syslib']
    reference_rows='\n'.join('    {'+json.dumps(ref['library'])+', '+str(int(ref['nid'],0))+'u, '+
        ('PRX_EXPORT_FUNCTION' if ref['kind']=='function' else 'PRX_EXPORT_VARIABLE')+', '+
        ', '.join(str(int(ref[field],0) if isinstance(ref[field],str) else ref[field])+'u' for field in ('type','target','addend','expected'))+'},'
        for ref in references)
    if not reference_rows:reference_rows='    {nullptr,0,0,0,0,0,0},'
    (output/'pace_module.cpp').write_text(f'''#include "mag_pace_recomp.h"
#include "v212_pace.h"
extern "C" const uint8_t mag_pace_image[];
extern "C" const MagV212PaceModule* mag_v212_pace_prepared_module(void) {{
    static const prx_export exports[] = {{
{entries}
    }};
    static const prx_module module = {{"pace", {BASE}u, mag_pace_image, {len(image)}u,
        reinterpret_cast<const prx_func_entry*>(mag_pace_function_table), mag_pace_function_table_count,
        exports, sizeof(exports)/sizeof(exports[0])}};
    static const MagV212PaceImportReference references[] = {{
{reference_rows}
    }};
    static const MagV212PaceModule descriptor = {{&module, "{selected['guest_path']}",
        {int(system['0xBC9A0086'],0)}u, {int(system['0xAB779874'],0)}u,references,{len(references)}u}};
    return &descriptor;
}}
''')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Separately decrypted original libpace PRX (required for generation)')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--output', type=Path, help='Fresh ignored output directory')
    mode.add_argument('--check', type=Path, help='Verify an existing primary-module preparation')
    parser.add_argument('--sources-out', type=Path, help='Write validated CMake source/header paths')
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--variant', choices=('libpace_g','libpacePrx_g'), default='libpace_g')
    args = parser.parse_args()
    if args.check:
        if args.source or args.variant != 'libpace_g':
            parser.error('--check accepts only the primary prepared module and no --source')
        result, sources = check(args.check, args.sdk)
        if args.sources_out:
            write_sources(args.sources_out, args.check, sources)
    else:
        if not args.source:
            parser.error('--source is required with --output')
        result = generate(args.source, args.output, args.sdk, args.variant)
        if args.sources_out:
            checked, sources = check(args.output, args.sdk)
            write_sources(args.sources_out, args.output, sources)
    print(json.dumps(dict(status=result['status'], target=TARGET, variant=result['variant'],
                          todo_by_mnemonic=result['todo_by_mnemonic'], runtime_verified=False)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
