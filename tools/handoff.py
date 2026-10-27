"""Exact prepared-image adapter and one bounded launcher-to-engine transition."""
import hashlib
import json
import os
import stat
from pathlib import Path
import time
import uuid
from tools.prepare_inputs import ROOT, verify_hash, validate_ppu_elf

GUEST_ENGINE='/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self'


def regular_file(path: Path):
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ValueError('Prepared input must be a regular file')
        return os.fdopen(fd,'rb')
    except BaseException:
        os.close(fd)
        raise


def checked_hash(path: Path, expected: str) -> None:
    with regular_file(path) as stream:
        actual=hashlib.file_digest(stream,'sha256').hexdigest()
    if actual!=expected:
        raise ValueError('Prepared input identity mismatch: '+str(path))


def checked_receipt(binary: Path, elf_hash: str) -> dict:
    with regular_file(binary.with_suffix('.receipt.json')) as stream:
        raw=stream.read(2*1024*1024+1)
    if len(raw)>2*1024*1024:
        raise ValueError('Engine receipt exceeds size limit')
    receipt=json.loads(raw)
    if receipt['elf_sha256']!=elf_hash:
        raise ValueError('Native binary was built for a different ELF')
    checked_hash(binary,receipt['binary_sha256'])
    return receipt


def validate_mapping(mapping: dict) -> dict:
    """Recheck every immutable input before accepting a process transition."""
    vfs=Path(mapping['vfs']).resolve()
    image=Path(mapping['self'])
    expected=vfs/'PS3_GAME/USRDIR/bin/mage_g.self'
    if image != expected or image.is_symlink() or image.resolve()!=expected:
        raise ValueError('Engine SELF must be the exact private disc file')
    for field in ('self','elf','binary'):
        checked_hash(Path(mapping[field]),mapping[field+'_sha256'])
    receipt=checked_receipt(Path(mapping['binary']),mapping['elf_sha256'])
    if receipt.get('image_name')!='mage_g':
        raise ValueError('Engine receipt must identify mage_g')
    if receipt['sdk_revision'] not in mapping['strict_revisions']:
        raise ValueError('Engine runtime must support both strict failure policies')
    if mapping['guest_path']!=GUEST_ENGINE or mapping['command']!=[mapping['binary'],mapping['elf']]:
        raise ValueError('Engine command or guest path does not match prepared image')
    return receipt


def provision_handoff(vfs: Path, output: Path, engine_binary: Path) -> tuple[dict,dict]:
    images=json.loads((ROOT/'profiles/retail-v1.0.json').read_text())['images']
    framework=json.loads((ROOT/'profiles/framework.json').read_text())
    engine=images['mage_g'];launcher=images['EBOOT']
    verify_hash(Path(launcher['elf']),launcher['elf_sha256'])
    # Read the retail launcher's key identity without storing or logging its bytes.
    layout=validate_ppu_elf(Path(launcher['elf']))
    segment=next(s for s in layout['load_segments'] if s['vaddr']<=0x32A660 and 0x32A670<=s['vaddr']+s['filesz'])
    with Path(launcher['elf']).open('rb') as f:
        f.seek(segment['offset']+0x32A660-segment['vaddr']); key_digest=hashlib.sha256(f.read(16)).hexdigest()
    binary=engine_binary.resolve();receipt=checked_receipt(binary,engine['elf_sha256'])
    mapping=dict(version=1,token=uuid.uuid4().hex,request=str(output/'handoff-request.json'),
        vfs=str(vfs.resolve()),guest_path=GUEST_ENGINE,self=str(vfs.resolve()/'PS3_GAME/USRDIR/bin/mage_g.self'),
        elf=engine['elf'],binary=str(binary),self_sha256=engine['source_sha256'],
        elf_sha256=engine['elf_sha256'],binary_sha256=receipt['binary_sha256'],
        key_sha256=key_digest,strict_revisions=sorted(set(framework['strict_runtime_revisions'])&set(framework['strict_hle_runtime_revisions'])),
        command=[str(binary),engine['elf']])
    validate_mapping(mapping)
    env={'MAG_HANDOFF_REQUEST':mapping['request'],'MAG_HANDOFF_TOKEN':mapping['token']}
    for field in ('self','elf','binary'):
        env['MAG_ENGINE_'+field.upper()]=mapping[field]
        env['MAG_ENGINE_'+field.upper()+'_SHA256']=mapping[field+'_sha256']
    env['MAG_ENGINE_KEY_SHA256']=key_digest
    return mapping,env


def capture_handoff(command: list, cwd: Path, env: dict, log_path: Path,
                    seconds: float, mapping: dict, log_limit: int=8*1024*1024) -> dict:
    from tools.boot import capture_process
    started=time.monotonic()
    first=capture_process(command,cwd,env,log_path,seconds,log_limit)
    result=dict(first,final_log=str(log_path),launcher_capture=first,
                launcher_spawn_requested=False,engine_process_started=False)
    if first['returncode']!=90 or first['stop_reason'] is not None:
        return result
    try:
        request=Path(mapping['request'])
        fd=os.open(request,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
        try:
            info=os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size>4096:
                raise ValueError('Invalid handoff request file')
            raw=os.read(fd,4097)
            if len(raw)>4096:
                raise ValueError('Handoff request exceeds size limit')
            record=json.loads(raw)
        finally:
            os.close(fd)
        expected=dict(version=1,token=mapping['token'],guest_path=mapping['guest_path'],kind='mage_g')
        if record!=expected:
            raise ValueError('Handoff request identity or contract mismatch')
        engine_receipt=validate_mapping(mapping)
        result['engine_identity']={
            'sdk_revision':engine_receipt.get('sdk_revision'),
            'spu_registered':'spu_provenance' in engine_receipt,
            'firmware_registered':'firmware_provenance' in engine_receipt}
        result['launcher_spawn_requested']=True
        remaining=seconds-(time.monotonic()-started)
        budget=log_limit-log_path.stat().st_size
        if remaining<=0 or budget<=0:
            result['stop_reason']='timeout' if remaining<=0 else 'log_limit'
            return result
        child_env={k:v for k,v in env.items() if not k.startswith(('MAG_HANDOFF_','MAG_ENGINE_'))
                   and k not in ('MAG_LOCAL_ACCOUNT_NAME','MAG_LOCAL_NO_TICKET','MAG_TRACE_NETWORK','PS3_ARGV')}
        child_env['PS3_BOOT_PATH']=mapping['guest_path']
        child_env['PS3RECOMP_METAL_FRAME_DUMP']=str(cwd/'engine-frame.ppm')
        engine_log=cwd/'engine-console.log'
        child=capture_process(mapping['command'],cwd,child_env,engine_log,remaining,budget)
        result.update(child,final_log=str(engine_log),engine_process_started=True,engine_capture=child)
    except (OSError,ValueError,KeyError) as error:
        result['handoff_error']=str(error)
    finally:
        result['elapsed_seconds']=time.monotonic()-started
    return result
