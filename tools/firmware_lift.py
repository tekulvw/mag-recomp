"""Lift selected queue exports from the pinned local decrypted firmware image."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
if __package__ in (None,''):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, verify_hash, sha256_file
from tools.lift import guard_generated_source
from tools.inventory import check_sdk


# MAG-owned guest range, audited against this title and the pinned runtime.
# 0x20000000 is occupied by GCM's injected structures; 0x30000000 is MAG's
# GCM IO allocation. Keep the complete module inside this separate 1 MiB slot.
FIRMWARE_BASE = 0x21000000
FIRMWARE_WINDOW_SIZE = 0x100000


def validate_placement(profile: dict, image_size: int | None = None) -> None:
    if (profile.get('base') != FIRMWARE_BASE or
            profile.get('placement',{}).get('reserved_bytes') != FIRMWARE_WINDOW_SIZE):
        raise ValueError('Firmware placement must use the reviewed MAG reservation')
    if image_size is not None and not 0 < image_size <= FIRMWARE_WINDOW_SIZE:
        raise ValueError('Relocated firmware image exceeds its reserved window')
    # These offsets belong to the hash-pinned original PRX. The TOC is the
    # ABI anchor beyond the image end; do not mistake it for an image byte.
    for field,offset in (('toc',0x39AB0),('code_end',0x1DB18)):
        if profile.get(field) != FIRMWARE_BASE+offset:
            raise ValueError(f'Firmware {field} address does not match relocation')
    bridges={b['nid']:int(b['stub'],0) for b in profile.get('hle_bridges',[])}
    if bridges != {'0xB9BC6207':FIRMWARE_BASE+0xB0D0,
                   '0x4E66D483':FIRMWARE_BASE+0xB234} or len(profile['hle_bridges']) != 2:
        raise ValueError('Firmware bridge addresses do not match relocation')


def bridge_function(source: str, address: str, nid: str) -> str:
    # Internal firmware functions are not a contiguous import-stub island.
    # Replace only the selected, hash-pinned function definition, preserving
    # every neighboring definition and call site.
    symbol=f'mag_sre_func_{int(address, 0):08X}'
    pattern=rf'^void {symbol}\(ppu_context\* ctx\) \{{\n.*?^\}}'
    replacement=f'void {symbol}(ppu_context* ctx) {{\n    ps3_hle_call(0x{int(nid, 0):08X}u, ctx);\n}}'
    result,count=re.subn(pattern,lambda _:replacement,source,flags=re.M|re.S)
    if count != 1:
        raise ValueError(f'Firmware bridge requires exactly one definition of {symbol}, got {count}')
    return result


def generate(output: Path) -> dict:
    profile=json.loads((ROOT/'profiles/retail-v1.0-firmware.json').read_text())
    sdk=ROOT/os.environ.get('MAG_FIRMWARE_LIFTER_REPOSITORY', profile['lifter_repository']);check_sdk(sdk,expected_revision=profile['lifter_revision'])
    revision=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    if revision != profile['lifter_revision']:
        raise ValueError('Unexpected firmware lifter revision')
    validate_placement(profile)
    source=ROOT/profile['image'];verify_hash(source,profile['image_sha256'])
    output.mkdir(parents=True,exist_ok=False)
    inputs=output/'inputs';inputs.mkdir()
    base=profile['base'];toc=profile['toc']
    commands=[[sys.executable,str(sdk/'tools/lift_prx.py'),str(source),
               '--base',hex(base),'--output',str(inputs)]]
    with (output/'relocation.log').open('w') as log:
        subprocess.run(commands[0],stdout=log,stderr=subprocess.STDOUT,check=True)
    validate_placement(profile,(inputs/'libsre_image.bin').stat().st_size)
    imports=json.loads((inputs/'libsre_imports.json').read_text())
    stubs=[dict(library=lib,nid=nid,stub=stub) for lib,entries in imports.items() for nid,stub in entries.items()]
    (inputs/'hle-stubs.json').write_text(json.dumps(stubs,indent=2)+'\n')
    commands.append([sys.executable,str(sdk/'tools/ppu_lifter.py'),str(inputs/'libsre_image.bin'),
                     '--raw','--base',hex(base),'--toc',hex(toc),'--code-end',hex(profile['code_end']),
                     '--functions',str(inputs/'libsre_functions.json'),'--hle-stubs',str(inputs/'hle-stubs.json'),
                     '--symbol-prefix','mag_sre_','--header-name','mag_sre_recomp.h','--jobs','2','--output',str(output)])
    with (output/'generation.log').open('w') as log:
        subprocess.run(commands[-1],stdout=log,stderr=subprocess.STDOUT,check=True)
    for bridge in profile.get('hle_bridges', []):
        symbol=f"void mag_sre_func_{int(bridge['stub'],0):08X}("
        matches=[p for p in output.glob('ppu_recomp_*.cpp') if symbol in p.read_text()]
        if len(matches) != 1:
            raise ValueError(f'Expected one source file for bridge {bridge["name"]}')
        path=matches[0]
        path.write_text(bridge_function(path.read_text(),bridge['stub'],bridge['nid']))
    todo_count=0
    for path in output.glob('ppu_recomp_*.cpp'):
        code=path.read_text();todo_count+=len(re.findall(r'TODO:|unresolved target',code))
        path.write_text(guard_generated_source(code))
    image=(inputs/'libsre_image.bin').read_bytes()
    lines=[', '.join(f'0x{x:02x}' for x in image[i:i+16]) for i in range(0,len(image),16)]
    (output/'firmware_image.c').write_text('#include <stdint.h>\nconst uint8_t mag_sre_image[] = {\n'+',\n'.join(lines)+'\n};\n')
    exports=json.loads((inputs/'libsre_exports.json').read_text())['exports']
    flat={nid:int(opd,16)-base for group in exports.values() for nid,opd in group.items()}
    entries='\n'.join(f'    {{{nid}u, 0x{flat[nid]:08X}u}},' for nid in profile['exports'])
    (output/'firmware_module.cpp').write_text(f'''#include "mag_sre_recomp.h"
#include "prx_loader.h"
#include <stdio.h>
#include <stdlib.h>
extern "C" const uint8_t mag_sre_image[];
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*));
static void register_function(uint32_t ea, void (*fn)(void*)) {{
    ppu_register_function(ea, reinterpret_cast<void (*)(ppu_context*)>(fn));
}}
extern "C" void mag_load_queue_firmware(void) {{
    static const prx_export exports[] = {{
{entries}
    }};
    const prx_module module = {{"MAG queue firmware", 0x{base:08X}u, mag_sre_image, {len(image)},
        reinterpret_cast<const prx_func_entry*>(mag_sre_function_table), mag_sre_function_table_count,
        exports, sizeof(exports)/sizeof(exports[0])}};
    if (!prx_load_module(&module, register_function).ok) {{
        fprintf(stderr, "[mag] queue firmware load failed\\n"); _Exit(85);
    }}
}}
''')
    record=dict(status='complete',profile=profile,commands=commands,
                generator_sha256=sha256_file(Path(__file__)),executed_todo_policy='halt',
                guarded_todo_count=todo_count,outputs={p.name:sha256_file(p) for p in sorted(output.iterdir())
                   if p.is_file() and p.suffix in ('.c','.cpp','.h')})
    (output/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();record=generate(args.output.resolve())
    print(f"Generated queue firmware with {record['guarded_todo_count']} guarded TODO sites")
