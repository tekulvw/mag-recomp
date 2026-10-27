"""Reproduce the audited retail EDGE zlib SPU lift in a fresh output directory."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
if __package__ in (None, ''):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, sha256_file, verify_hash


def generate(output: Path) -> dict:
    profile=json.loads((ROOT/'profiles/retail-v1.0-spu.json').read_text())
    sdk=ROOT/os.environ.get('MAG_SPU_LIFTER_REPOSITORY', profile['lifter_repository'])
    revision=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    if revision != profile['lifter_revision']:
        raise ValueError('Unexpected SPU lifter revision')
    if subprocess.check_output(['git','-C',str(sdk),'status','--porcelain','--untracked-files=no'],text=True):
        raise ValueError('SPU lifter repository must be clean')
    image=ROOT/profile['image']; verify_hash(image,profile['image_sha256'])
    retail=json.loads((ROOT/'profiles/retail-v1.0.json').read_text())['images']['EBOOT']
    eboot=Path(retail['elf']);verify_hash(eboot,retail['elf_sha256'])
    data=image.read_bytes()
    with eboot.open('rb') as f:
        f.seek(profile['eboot_offset'])
        if f.read(len(data)) != data:
            raise ValueError('SPU image is not the selected retail EBOOT embedded image')
    output.mkdir(parents=True,exist_ok=False)
    target=output/'mag_edgezlib'; target.mkdir()
    command=[sys.executable,str(sdk/'tools/spu_lifter.py'),str(image),
             '--auto-functions',str(image),'--code-end',hex(profile['code_end']),
             '--symbol-prefix','mag_edgezlib_','-o',str(target)]
    with (output/'generation.log').open('w') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    for path in target.glob('*'):
        if path.suffix in ('.c','.h'):
            text=path.read_text()
            text=re.sub(r'#include "(?:\.\./)+runtime/spu/([^\"]+)"',r'#include "\1"',text)
            path.write_text(text)
    source=(target/'spu_recomp.c').read_text()
    unsupported=len(re.findall(r'TODO spu:|spu_unsupported\(',source))
    if unsupported:
        raise ValueError(f'SPU lift has {unsupported} unsupported/unresolved markers')
    image_id=profile['image_id'];entry=profile['entry'];fingerprint=profile['runtime_fingerprint']
    (output/'spu_workloads.c').write_text(f'''/* Generated from profiles/retail-v1.0-spu.json. */
#include "spu_workload.h"
extern void spu_begin_image(int);
extern void mag_edgezlib_spu_recomp_register(void);
extern void mag_edgezlib_spu_func_{entry:08X}(spu_context*);
__attribute__((constructor)) static void mag_register_spu(void) {{
    spu_begin_image({image_id}); mag_edgezlib_spu_recomp_register();
    spu_workload_register_img({fingerprint}ULL, mag_edgezlib_spu_func_{entry:08X}, {image_id}, "MAG retail EDGE zlib");
    spu_begin_image(0);
}}
''')
    record=dict(status='complete',profile=profile,command=command,unsupported_count=unsupported,
                generator_sha256=sha256_file(Path(__file__)),
                outputs={str(p.relative_to(output)):sha256_file(p) for p in sorted(output.rglob('*'))
                         if p.is_file() and p.suffix in ('.c','.h')})
    (output/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    record=generate(args.output.resolve())
    print(f"Generated {len(record['outputs'])} audited SPU source files")
