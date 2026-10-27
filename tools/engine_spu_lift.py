"""Lift pinned retail engine SPU images beside the unchanged audited EDGE worker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
if __package__ in (None, ''):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, sha256_file, verify_hash, data_path
from tools.spu_lift import generate as generate_edge


def runtime_fingerprint(data: bytes) -> int:
    # Preserve the SDK's historical (nonstandard) FNV offset basis exactly.
    value=1469598103934665603
    for byte in data:
        value=((value^byte)*1099511628211)&0xFFFFFFFFFFFFFFFF
    return value


def extract_policy(image: bytes, profile: dict) -> bytes:
    if hashlib.sha256(image).hexdigest() != profile['elf_sha256']:
        raise ValueError('Engine ELF identity does not match policy profile')
    policy=profile['policy'];offset=policy['offset'];size=policy['size']
    if offset < 0 or size <= 0 or offset+size > len(image):
        raise ValueError('Policy slice is outside the engine ELF')
    data=image[offset:offset+size]
    if hashlib.sha256(data).hexdigest() != policy['sha256']:
        raise ValueError('Embedded policy slice hash mismatch')
    if runtime_fingerprint(data) != int(policy['runtime_fingerprint'],0):
        raise ValueError('Embedded policy dispatch fingerprint mismatch')
    if len(image)<64 or image[:6] != b'\x7fELF\x02\x02':
        raise ValueError('Engine must be a big-endian ELF64')
    phoff=struct.unpack_from('>Q',image,32)[0]
    entsize,count=struct.unpack_from('>HH',image,54)
    if entsize < 56 or phoff+entsize*count > len(image):
        raise ValueError('Engine program header bounds are invalid')
    for i in range(count):
        kind,flags,start,ea,pa,filesz,memsz,align=struct.unpack_from('>IIQQQQQQ',image,phoff+i*entsize)
        if kind==1 and start<=offset and offset+size<=start+filesz:
            if ea+offset-start == policy['guest_ea']:
                return data
    raise ValueError('Policy file slice does not map to the reviewed guest address')


def extract_training(image: bytes, profile: dict, key: str = 'training_task') -> bytes:
    task=profile[key]
    data=extract_policy(image,dict(elf_sha256=profile['elf_sha256'],policy=task))
    if len(data)<52 or data[:7] != b'\x7fELF\x01\x02\x01' or struct.unpack_from('>H',data,18)[0] != 23:
        raise ValueError('Training task must be a big-endian SPU ELF32')
    if struct.unpack_from('>I',data,24)[0] != task['entry']:
        raise ValueError('Training task ELF entry mismatch')
    shoff=struct.unpack_from('>I',data,32)[0]
    entsize,count=struct.unpack_from('>HH',data,46)
    if entsize<40 or not count or shoff+entsize*count>len(data):
        raise ValueError('Training task section table bounds are invalid')
    executable_end=0
    for i in range(count):
        _,kind,flags,addr,offset,size,*_=struct.unpack_from('>IIIIIIIIII',data,shoff+i*entsize)
        if flags&4 and kind==1:
            if offset+size>len(data) or addr+size>0x40000:
                raise ValueError('Training executable section is outside its backing or LS')
            executable_end=max(executable_end,addr+size)
    if executable_end != task['code_end']:
        raise ValueError('Training code boundary differs from executable ELF sections')
    return data


def render_engine_registration(profile: dict) -> str:
    code='/* Exact retail engine SPU images; no substitute job modules. */\n#include "spu_workload.h"\nextern void spu_begin_image(int);\n'
    images=[('policy','mag_wws_policy_','MAG retail WWS policy'),
            ('training_task','mag_training_','MAG retail training task'),
            ('queued_training_task','mag_training_queued_','MAG retail queued training task')]
    for key,prefix,name in images:
        item=profile[key];entry=item['entry']
        code+=f'extern void {prefix}spu_recomp_register(void);\nextern void {prefix}spu_func_{entry:08X}(spu_context*);\n'
    code+='__attribute__((constructor)) static void mag_register_engine_policy(void) {\n'
    for key,prefix,name in images:
        item=profile[key];entry=item['entry'];image_id=item['image_id'];fp=item['runtime_fingerprint']
        code+=f'    spu_begin_image({image_id}); {prefix}spu_recomp_register();\n'
        code+=f'    spu_workload_register_img({fp}ULL, {prefix}spu_func_{entry:08X}, {image_id}, "{name}");\n'
    return code+'    spu_begin_image(0);\n}\n'


def guard_policy_source(source: str, label: str = 'policy') -> tuple[str,int]:
    # A TODO comment can stand alone or precede spu_unsupported. Count it once
    # and replace the entire unsupported instruction site with a process halt.
    pattern=r'/\* TODO spu:.*?\*/\s*(?:spu_unsupported\([^;]*\);|;)?|\bspu_unsupported\([^;{}]*\);'
    halt='do { fprintf(stderr, "[mag] unsupported retail SPU policy in %s\\n", __func__); _Exit(86); } while (0);'
    halt=halt.replace('retail SPU policy','retail SPU '+label)
    source,count=re.subn(pattern,lambda _:halt,source,flags=re.S)
    return '#include <stdio.h>\n#include <stdlib.h>\n'+source,count


def generate(output: Path) -> dict:
    profile=json.loads((ROOT/'profiles/retail-v1.0-engine-spu.json').read_text())
    sdk=data_path(os.environ.get('MAG_SPU_LIFTER_REPOSITORY', profile['lifter_repository']))
    revision=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    if revision != profile['lifter_revision']:
        raise ValueError('Unexpected engine SPU lifter revision')
    if subprocess.check_output(['git','-C',str(sdk),'status','--porcelain','--untracked-files=no'],text=True):
        raise ValueError('Engine SPU lifter repository must be clean')
    worker_profile=ROOT/'profiles/retail-v1.0-spu.json'
    verify_hash(worker_profile,profile['worker_profile_sha256'])
    image=json.loads((data_path('profiles/retail-v1.0.json')).read_text())['images']['mage_g']
    if image['elf_sha256'] != profile['elf_sha256']:
        raise ValueError('Selected engine image does not match SPU profile')
    data=Path(image['elf']).read_bytes();raw=extract_policy(data,profile)
    training=extract_training(data,profile)
    queued_training=extract_training(data,profile,'queued_training_task')
    worker=json.loads(worker_profile.read_text());embedded=profile['embedded_worker']
    if embedded['offset']<0 or embedded['size'] != worker['image_bytes']:
        raise ValueError('Engine embedded worker bounds mismatch')
    worker_data=data[embedded['offset']:embedded['offset']+embedded['size']]
    if (hashlib.sha256(worker_data).hexdigest() != worker['image_sha256'] or
            embedded['sha256'] != worker['image_sha256']):
        raise ValueError('Engine embedded worker does not match the audited EDGE image')
    # The existing generator enforces fresh output and reproduces the exact
    # worker sources/registration. Keep that receipt alongside the new one.
    edge_record=generate_edge(output)
    (output/'edge-provenance.json').write_text(json.dumps(edge_record,indent=2)+'\n')
    inputs=output/'inputs';inputs.mkdir()
    raw_path=inputs/'wws-policy.bin';raw_path.write_bytes(raw)
    wrapped=inputs/'wws-policy.elf';policy=profile['policy']
    commands=[[sys.executable,str(sdk/'tools/wrap_spu_elf.py'),str(raw_path),
               '--base',hex(policy['base']),'--entry',hex(policy['entry']),'--out',str(wrapped)]]
    target=output/'mag_wws_policy'
    commands.append([sys.executable,str(sdk/'tools/spu_lifter.py'),str(wrapped),
        '--auto-functions',str(wrapped),'--code-end',hex(policy['code_end']),
        '--extra-funcs',','.join(hex(x) for x in policy['extra_functions']),
        '--symbol-prefix','mag_wws_policy_','-o',str(target)])
    training_path=inputs/'training-task.elf';training_path.write_bytes(training)
    training_target=output/'mag_training';task=profile['training_task']
    commands.append([sys.executable,str(sdk/'tools/spu_lifter.py'),'--auto-functions',str(training_path),
        '--code-end',hex(task['code_end']),'--symbol-prefix','mag_training_','-o',str(training_target)])
    queued_path=inputs/'queued-training-task.elf';queued_path.write_bytes(queued_training)
    queued_target=output/'mag_training_queued';queued_task=profile['queued_training_task']
    commands.append([sys.executable,str(sdk/'tools/spu_lifter.py'),'--auto-functions',str(queued_path),
        '--code-end',hex(queued_task['code_end']),'--symbol-prefix','mag_training_queued_','-o',str(queued_target)])
    # Never leave the worker's successful receipt looking like a complete
    # engine receipt if either subsequent command fails.
    (output/'provenance.json').write_text(json.dumps(dict(status='incomplete',profile=profile))+'\n')
    with (output/'policy-generation.log').open('w') as log:
        for command in commands:
            subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    for path in [*target.glob('*'),*training_target.glob('*'),*queued_target.glob('*')]:
        if path.suffix in ('.c','.h'):
            code=re.sub(r'#include "(?:\.\./)+runtime/spu/([^\"]+)"',r'#include "\1"',path.read_text())
            path.write_text(code)
    path=target/'spu_recomp.c';original=path.read_text()
    guarded,count=guard_policy_source(original);path.write_text(guarded)
    if count != policy.get('expected_unsupported_count',profile['expected_unsupported_count']):
        raise ValueError('Policy unsupported census changed; review the generated sites')
    entry=policy['entry']
    if f'void mag_wws_policy_spu_func_{entry:08X}(' not in original:
        raise ValueError('Policy entry was not lifted')
    training_source=(training_target/'spu_recomp.c').read_text()
    training_guarded,training_count=guard_policy_source(training_source,'training task')
    (training_target/'spu_recomp.c').write_text(training_guarded)
    if training_count != task['expected_unsupported_count']:
        raise ValueError('Training unsupported census changed; review generated sites')
    if f"void mag_training_spu_func_{task['entry']:08X}(" not in training_source:
        raise ValueError('Training task entry was not lifted')
    queued_source=(queued_target/'spu_recomp.c').read_text()
    queued_guarded,queued_count=guard_policy_source(queued_source,'queued training task')
    (queued_target/'spu_recomp.c').write_text(queued_guarded)
    if queued_count != queued_task['expected_unsupported_count']:
        raise ValueError('Queued training unsupported census changed; review generated sites')
    if f"void mag_training_queued_spu_func_{queued_task['entry']:08X}(" not in queued_source:
        raise ValueError('Queued training task entry was not lifted')
    policy_count=count
    count+=training_count+queued_count
    if count != profile['expected_unsupported_count']:
        raise ValueError('Combined engine SPU unsupported census changed')
    (output/'engine_policy_workload.c').write_text(render_engine_registration(profile))
    record=dict(status='complete',profile=profile,commands=commands,
        generator_sha256=sha256_file(Path(__file__)),elf_sha256=profile['elf_sha256'],
        training_sha256=hashlib.sha256(training).hexdigest(),
        queued_training_sha256=hashlib.sha256(queued_training).hexdigest(),
        queued_training_function_count=len(re.findall(r'^void mag_training_queued_spu_func_[0-9A-F]+\(',queued_source,re.M)),
        queued_training_unsupported_count=queued_count,
        training_function_count=len(re.findall(r'^void mag_training_spu_func_[0-9A-F]+\(',training_source,re.M)),
        training_unsupported_count=training_count,policy_unsupported_count=policy_count,
        policy_sha256=hashlib.sha256(raw).hexdigest(),wrapped_elf_sha256=sha256_file(wrapped),
        worker_provenance=edge_record,unsupported_count=count,guarded_todo_count=count,
        unsupported_sites=[{'pc':int(pc,16),'mnemonic':mnemonic} for pc,mnemonic in
                           re.findall(r'spu_unsupported\(0x([0-9A-Fa-f]+), "([^"]+)"\)',original+'\n'+training_source+'\n'+queued_source)],
        executed_todo_policy='halt',
        audit_meaning='Static generation census with fail-closed unsupported sites; not an execution or correctness claim.',
        policy_function_count=len(re.findall(r'^void mag_wws_policy_spu_func_[0-9A-F]+\(',original,re.M)),
        outputs={str(p.relative_to(output)):sha256_file(p) for p in sorted(output.rglob('*'))
                 if p.is_file() and p.suffix in ('.c','.h','.cpp')})
    (output/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();record=generate(args.output.resolve())
    print(f"Generated engine SPU: {record['policy_function_count']} policy functions, {record['unsupported_count']} guarded unsupported sites")
