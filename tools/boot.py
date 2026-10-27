"""Run only the native diagnostic target, with bounded time and saved evidence."""
import argparse
import datetime
import json
import os
import re
import signal
import selectors
import subprocess
import sys
import time
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, verify_hash, sha256_file, data_path, load_framework
from tools.provenance import validate_receipt
from tools.network_profile import prepare_overlay


def clone_disc(source: Path, destination: Path) -> None:
    """APFS copy-on-write files are independent; never expose source symlinks."""
    if source.is_symlink() or any(p.is_symlink() for p in source.rglob('*')):
        raise ValueError('Disc source must not contain symlinks')
    destination.mkdir(parents=True, exist_ok=False)
    subprocess.run(['/bin/cp', '-cR', str(source), str(destination/'PS3_GAME')], check=True)


def capture_process(command: list, cwd: Path, env: dict, log_path: Path,
                    seconds: float, log_limit: int = 8 * 1024 * 1024) -> dict:
    """Bound elapsed time AND output; reap the process group even on interruption."""
    started=time.monotonic()
    reason=None
    proc=subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          cwd=cwd, env=env, start_new_session=True)
    try:
        with selectors.DefaultSelector() as selector, log_path.open('wb') as log:
            selector.register(proc.stdout, selectors.EVENT_READ)
            count=0
            while selector.get_map():
                remaining=seconds-(time.monotonic()-started)
                if remaining <= 0:
                    reason='timeout'; break
                for key, _ in selector.select(min(remaining, .1)):
                    data=os.read(key.fileobj.fileno(), min(65536, log_limit-count))
                    if not data:
                        selector.unregister(key.fileobj); break
                    log.write(data); count+=len(data)
                    if count >= log_limit:
                        reason='log_limit'; break
                if reason:
                    break
            if not reason:
                try:
                    proc.wait(timeout=max(.001, seconds-(time.monotonic()-started)))
                except subprocess.TimeoutExpired:
                    reason='timeout'
    finally:
        # Also kill descendants if the group leader has already exited.
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
        proc.stdout.close()
    return dict(returncode=proc.returncode, stop_reason=reason,
                elapsed_seconds=time.monotonic()-started)


def classify_boot(log_text: str, returncode: int | None, timed_out: bool) -> dict:
    if '[mag] unsupported lifted code' in log_text or '[mag] unsupported retail SPU ' in log_text:
        outcome='unsupported_lift'
    elif 'unresolved nid' in log_text.lower():
        outcome='unresolved_import'
    elif 'UNRESOLVED syscall' in log_text:
        outcome='unresolved_syscall'
    elif '[ppu] unresolved indirect call ->' in log_text:
        outcome='unresolved_function'
    elif '-- assertion failed' in log_text:
        outcome='guest_assertion'
    elif timed_out:
        outcome='timeout'
    elif returncode is not None and returncode < 0:
        outcome='signal'
    elif 'ppu_run returned' in log_text:
        outcome='entry_returned'
    else:
        outcome='process_exited'
    frames=[int(n) for n in re.findall(r'\[mag\] guest frame presented (\d+)',log_text)]
    result=dict(guest_frames_presented_lower_bound=max(frames,default=0), entry_reached='[mag] retail_entry 0x00010230' in log_text,
                engine_initialized=False, outcome=outcome, timed_out=timed_out, returncode=returncode)
    if outcome=='guest_assertion':
        result['failure_detail']=next(line for line in log_text.splitlines()
                                      if '-- assertion failed' in line)
    elif outcome=='unresolved_function':
        result['failure_detail']=next(line for line in log_text.splitlines()
                                      if '[ppu] unresolved indirect call ->' in line)
    elif outcome=='unresolved_import':
        result['failure_detail']=next(line for line in log_text.splitlines()
                                      if 'unresolved nid' in line.lower())
    return result


def select_boot_image(profile: dict, image_name: str) -> tuple[dict, Path, str]:
    if image_name not in ('EBOOT','mage_g'):
        raise ValueError('Image must be EBOOT or mage_g')
    image=profile['images'][image_name]
    disc=Path(profile['images']['EBOOT']['source']).parents[1]
    if disc.name != 'PS3_GAME':
        raise ValueError('Expected a PS3_GAME disc source')
    relative=Path(image['source']).relative_to(disc)
    if '..' in relative.parts:
        raise ValueError('Image must stay inside the disc')
    return image,disc,'/dev_bdvd/PS3_GAME/'+relative.as_posix()


def run_boot(binary: Path, seconds: float, output_dir: Path, syscall_trace: str = 'fail', visible: bool = False, network_profile: Path | None = None, trace_network: bool = False, image_name: str = 'EBOOT', engine_handoff: bool = False, first_nonblack: bool = False, trace_queue: bool = False) -> dict:
    if not 0 < seconds <= 120:
        raise ValueError('Diagnostic duration must be within 0..120 seconds')
    if syscall_trace not in ('1', 'fail'):
        raise ValueError('Syscall trace must be 1 or fail')
    binary=binary.resolve(); output_dir=output_dir.resolve()
    image,source,guest_path=select_boot_image(json.loads((data_path('profiles/retail-v1.0.json')).read_text()),image_name)
    if engine_handoff and image_name!='EBOOT':
        raise ValueError('Only the launcher can request an engine handoff')
    if image_name != 'EBOOT' and (network_profile is not None or trace_network):
        raise ValueError('Launcher network hooks are verified only for EBOOT')
    elf=Path(image['elf']);verify_hash(elf, image['elf_sha256'])
    receipt=validate_receipt(binary,image['elf_sha256'])
    framework=load_framework()[0]
    if receipt['sdk_revision'] not in framework['strict_runtime_revisions']:
        raise ValueError('This diagnostic run requires a recorded runtime with strict syscall support')
    if receipt['sdk_revision'] not in framework.get('strict_hle_runtime_revisions', []):
        raise ValueError('This diagnostic run requires a recorded runtime with strict HLE support')
    if first_nonblack and receipt['sdk_revision'] not in framework.get('first_nonblack_capture_revisions', []):
        raise ValueError('This recorded runtime does not support first-nonblack frame capture')
    output_dir.mkdir(parents=True, exist_ok=False)
    env=os.environ.copy()
    for key in list(env):
        if key.startswith(('PS3_', 'PPU_', 'PS3RECOMP_', 'SPU_', 'SPURS_', 'LBP_', 'YDKJ_', 'MAG_', 'PSNR_')):
            del env[key]
    verify_hash(Path(image['source']), image['source_sha256'])
    vfs=output_dir/'vfs'
    clone_disc(source, vfs)
    network=None
    if network_profile is not None:
        network=prepare_overlay(vfs,network_profile)
        (output_dir/'network-profile.json').write_text(json.dumps(network,indent=2)+'\n')
        if network['host_sockets']:
            env['PS3_NET_ONLINE']='1'
            env['PS3_NET_TRACE']='1'
        if network['restore_ticket_setting']:
            env['MAG_LOCAL_NO_TICKET']='1'
        if network.get('local_account_name'):
            env['MAG_LOCAL_ACCOUNT_NAME']=network['local_account_name']
            # The native name dialog remains editable and requires confirmation.
            env['PS3RECOMP_OSK_INITIAL_TEXT']=network['local_account_name']
        if network.get('local_np_ticket'):
            env['PS3RECOMP_NP_LOCAL_TICKET']='1'
            env['PS3_NP_ONLINE_ID']=network['local_account_name']
        trace_network=True
    if trace_network:
        env['MAG_TRACE_NETWORK']='1'
    env.update(PS3_VFS_ROOT=str(vfs), PS3_BOOT_PATH=guest_path, PS3_HLE_UNRESOLVED='halt',
               PS3_SYSCALL_UNRESOLVED='halt',
               PS3_HLE_TRACE='1', PS3_SCTRACE=syscall_trace,
               PS3RECOMP_METAL_FRAME_DUMP=str(output_dir/'frame.ppm'))
    if first_nonblack:
        env['PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK']='1'
    if trace_queue:
        env['MAG_TRACE_QUEUE']='1'
    if not visible:
        env['PS3RECOMP_METAL_HEADLESS']='1'
    command=[str(binary),str(elf)]
    mapping=None
    if engine_handoff:
        from tools.handoff import provision_handoff, capture_handoff, checked_receipt
        mapping,handoff_env=provision_handoff(vfs,output_dir,ROOT/'build-engine/MagRecomp')
        if first_nonblack:
            engine_receipt=checked_receipt(Path(mapping['binary']),mapping['elf_sha256'])
            if engine_receipt['sdk_revision'] not in framework.get('first_nonblack_capture_revisions', []):
                raise ValueError('The recorded engine runtime does not support first-nonblack frame capture')
        env.update(handoff_env)
        (output_dir/'handoff-provision.json').write_text(json.dumps(mapping,indent=2)+'\n')
        capture=capture_handoff(command,output_dir,env,output_dir/'console.log',seconds,mapping)
    else:
        capture=capture_process(command, output_dir, env, output_dir/'console.log', seconds)
    final_log=Path(capture.get('final_log',output_dir/'console.log'))
    text=final_log.read_text(errors='replace')
    result=classify_boot(text, capture['returncode'], capture['stop_reason']=='timeout')
    if capture['stop_reason']=='log_limit' and result['outcome']=='signal':
        result['outcome']='log_limit'
    result.update(capture)
    result.update(image_name=image_name, guest_boot_path=guest_path, command=command, binary_sha256=receipt['binary_sha256'], elf_sha256=image['elf_sha256'],
                  sdk_revision=receipt['sdk_revision'], syscall_unresolved='halt',
                  seconds=seconds, vfs_root=env['PS3_VFS_ROOT'], unresolved_hle='halt', headless_metal=not visible,
                  spu_registered='spu_provenance' in receipt, firmware_registered='firmware_provenance' in receipt,
                  syscall_trace=syscall_trace, queue_trace=trace_queue, frame_capture_mode='first_nonblack' if first_nonblack else 'periodic',
                  limitations='No verified engine startup marker; SPU execution is experimental; registered placeholder handlers may still return success.')
    if capture.get('handoff_error'):
        result['outcome']='handoff_rejected'
        result['failure_detail']=capture['handoff_error']
    if capture.get('engine_process_started'):
        result['engine_entry_reached']=result['entry_reached']
        result['launcher_entry_reached']='[mag] retail_entry 0x00010230' in (output_dir/'console.log').read_text(errors='replace')
        result['launcher_sdk_revision']=result['sdk_revision']
        result['launcher_spu_registered']=result['spu_registered']
        result['launcher_firmware_registered']=result['firmware_registered']
        result.update(capture['engine_identity'])
        result['final_image_name']='mage_g'
        result['engine_binary_sha256']=mapping['binary_sha256']
        result['engine_elf_sha256']=mapping['elf_sha256']
    result['network_profile']=network
    (output_dir/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path)
    parser.add_argument('--image',choices=['EBOOT','mage_g'],default='EBOOT')
    parser.add_argument('--seconds',type=float,default=15)
    parser.add_argument('--output',type=Path,default=data_path('runs')/datetime.datetime.now(datetime.timezone.utc).strftime('native-%Y%m%dT%H%M%SZ'))
    parser.add_argument('--syscall-trace', choices=['1','fail'], default='fail')
    parser.add_argument('--engine-handoff',action='store_true',help='Continue an exact verified launcher spawn in a fresh engine process')
    parser.add_argument('--visible', action='store_true', help='Show the native Metal window')
    parser.add_argument('--trace-queue', action='store_true', help='Observe bounded queue descriptors and completion counters')
    parser.add_argument('--first-nonblack', action='store_true', help='Retain the first nonblack presented frame')
    parser.add_argument('--network-profile',type=Path,help='Hash-pinned local replacement-service profile')
    parser.add_argument('--trace-network',action='store_true',help='Observe retail network settings and callbacks')
    a=parser.parse_args()
    binary=a.binary or ROOT/('build-engine' if a.image=='mage_g' else 'build')/'MagRecomp'
    print(json.dumps(run_boot(binary,a.seconds,a.output,a.syscall_trace,a.visible,a.network_profile,a.trace_network,a.image,a.engine_handoff,a.first_nonblack,a.trace_queue),indent=2))
