"""Bounded synthetic-data fixture using existing real firmware/EDGE build objects.

The existing build is read only. No title executable is run, no game asset is
read, and no generated/original bytes are copied into the source repository.
Compilation is separate from the <=15 second execution budget. Runtime stdout
and stderr together are capped at 8 MiB. Missing private inputs are an explicit
SKIP; changed/mismatched input hashes are failures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shlex
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ''):
    sys.path.insert(0,str(ROOT))
PASS = 'EDGE_QUEUE_PASS jobs=96 producers=2 bytes=393216 counters=0 masks=1,2 wraps=8'


def completed(text, returncode):
    return returncode == 0 and PASS in text.splitlines()


def clean_environment(source):
    env = {k:v for k,v in source.items() if k in
           {'PATH','HOME','USER','LOGNAME','TMPDIR','LANG','LC_ALL','LC_CTYPE','TZ'}}
    env['PS3RECOMP_STRICT_HLE'] = '1'
    return env


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bounded_run(args, deadline, budget):
    data = bytearray()
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          env=clean_environment(os.environ)) as child:
        selector = selectors.DefaultSelector()
        selector.register(child.stdout, selectors.EVENT_READ)
        try:
            while selector.get_map():
                if time.monotonic() >= deadline:
                    raise TimeoutError('15 second combined fixture deadline exceeded\n'+
                                       data[-16000:].decode(errors='replace'))
                for key, _ in selector.select(min(.1, max(0, deadline-time.monotonic()))):
                    block = os.read(key.fd, 65536)
                    if not block:
                        selector.unregister(key.fileobj)
                        continue
                    data.extend(block)
                    if len(data) > budget:
                        raise RuntimeError('8 MiB combined fixture log bound exceeded\n'+
                                           data[-16000:].decode(errors='replace'))
            return child.wait(timeout=max(.01,deadline-time.monotonic())), data.decode(errors='replace')
        finally:
            selector.close()
            if child.poll() is None:
                child.kill()
            child.wait()


def run(build):
    sdk = ROOT/'local/ps3recomp-macos'
    cache=(build/'CMakeCache.txt').read_text()
    fw=next(line.split('=',1)[1] for line in cache.splitlines() if line.startswith('MAG_FIRMWARE_DIR:'))
    firmware = Path(fw).resolve()
    firmware_rel=firmware.relative_to(ROOT).as_posix()
    edge = ROOT/'generated/retail-v1.0/spu-engine-policy-audited'
    profile = json.loads((ROOT/'builds/retail-v1.0-spu.json').read_text())
    elf = ROOT/profile['image']
    ninja = build/'build.ninja'
    receipt = build/'MagRecomp.receipt.json'
    required = [elf,ninja,receipt,firmware/'provenance.json',edge/'provenance.json']
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        print('SKIP: private built fixture prerequisites absent: '+', '.join(missing));return 77
    if digest(elf) != profile['image_sha256']:
        raise ValueError('EDGE ELF hash mismatch')
    from tools.provenance import validate_generated
    modules = {}
    for kind,directory,profile_name in [('firmware',firmware,'retail-v1.0-firmware.json'),
                                       ('spu',edge,'retail-v1.0-engine-spu.json')]:
        modules[kind] = validate_generated(directory,
            json.loads((ROOT/'builds'/profile_name).read_text()),kind)
    build_record=json.loads(receipt.read_text())
    if (build_record.get('firmware_provenance') != modules['firmware'] or
            build_record.get('spu_provenance') != modules['spu']):
        raise ValueError('Engine build receipt does not match audited module sources')
    # Select only runtime/firmware/EDGE objects, explicitly excluding all title
    # PPU, title main/stubs, and unrelated WWS policy objects.
    rels = ['local/ps3recomp-macos/runtime/ppu/'+u+'.cpp.o'
            for u in ['ppu_loader','ppu_hle','ppu_fs','ppu_sysprx']]
    rels += ['ppu_hle_nids.cpp.o',
        'local/ps3recomp-macos/runtime/spu/spu_dispatch_mt.c.o']
    rels += ['generated/retail-v1.0/spu-engine-policy-audited/'+u
             for u in ['spu_workloads.c.o','mag_edgezlib/spu_recomp.c.o']]
    rels += [firmware_rel+'/'+u
             for u in ['firmware_image.c.o','firmware_module.cpp.o','ppu_recomp_000.cpp.o']]
    objects = [build/'CMakeFiles/MagRecomp.dir'/p for p in rels]
    archive = build/'ps3recomp_sdk/libps3recomp_runtime.a'
    if any(not p.is_file() for p in [*objects,archive]):
        print('SKIP: build the audited engine modules first');return 77
    lines = ninja.read_text().splitlines()
    start = next(i for i,s in enumerate(lines) if s.startswith('build MagRecomp:'))
    libs = next(s.split(' = ',1)[1] for s in lines[start:] if s.startswith('  LINK_LIBRARIES ='))
    link = shlex.split(libs)
    link = [str(build/x) if x=='ps3recomp_sdk/libps3recomp_runtime.a' else x for x in link]
    bindings = {str(p):digest(p) for p in [*objects,archive,receipt,elf,
        ROOT/'tests/edge_queue_fixture.cpp',Path(__file__),firmware/'provenance.json',edge/'provenance.json']}
    with tempfile.TemporaryDirectory(prefix='mag-edge-queue-') as temp:
        tmp=Path(temp);binary=tmp/'fixture'
        # Snapshot artifacts before linking: another chat may rebuild the engine.
        snapshots=[]
        for i,p in enumerate(objects):
            dest=tmp/f'{i}.o';dest.write_bytes(p.read_bytes());snapshots.append(dest)
            if digest(dest)!=bindings[str(p)]:
                raise ValueError('Build object changed while snapshotting')
        archive_copy=tmp/'runtime.a';archive_copy.write_bytes(archive.read_bytes())
        if digest(archive_copy)!=bindings[str(archive)]:
            raise ValueError('Runtime archive changed while snapshotting')
        link=[str(archive_copy) if x==str(archive) else x for x in link]
        elf_copy=tmp/'edge.elf';elf_copy.write_bytes(elf.read_bytes())
        if digest(elf_copy)!=bindings[str(elf)]:
            raise ValueError('EDGE ELF changed while snapshotting')
        includes=[ROOT/'generated/retail-v1.0/mage_g-initial',sdk/'include',
                  sdk/'runtime/ppu',sdk/'runtime/spu',sdk/'runtime/platform']
        compiled=subprocess.run(['clang++','-std=c++17','-O1','-UNDEBUG',
            *['-I'+str(x) for x in includes],str(ROOT/'tests/edge_queue_fixture.cpp'),
            *map(str,snapshots),*link,'-lz','-o',str(binary)],check=True,
            timeout=90,capture_output=True)
        if compiled.stderr:
            print(compiled.stderr.decode(errors='replace'),file=sys.stderr)
        deadline=time.monotonic()+15;logs=[];budget=8*1024*1024
        for mode in ['normal','corrupt']:
            code,text=bounded_run([str(binary),str(elf_copy),mode],deadline,budget)
            logs.append(text);budget-=len(text.encode())
            ok = completed(text,code) if mode=='normal' else (code==1 and 'EDGE_QUEUE_FAIL decoded bytes' in text)
            if not ok:
                print(text[-16000:]);raise RuntimeError(f'{mode} fixture failed (exit {code})')
        print(json.dumps(dict(status='pass',jobs=96,producers=2,bytes=393216,
            negative_control='decoded byte corruption rejected',input_sha256=bindings,
            sdk_revision=build_record['sdk_revision'],
            artifact_binding='exact existing object/archive hashes; source association inherited from build receipt',
            elapsed_seconds=15-(deadline-time.monotonic())),indent=2))
        return 0


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'build-engine')
    args=parser.parse_args()
    try:
        raise SystemExit(run(args.build.resolve()))
    except subprocess.CalledProcessError as error:
        print((error.stderr or b'').decode(errors='replace')[-16000:]);raise
