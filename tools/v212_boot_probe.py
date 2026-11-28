#!/usr/bin/env python3
"""Owned, bounded headless 2.12 bootstrap against proven closed loopback settings.

No input injection, ticket, live host override or UI mode is exposed. Native runs
are never tests of this tool: lifecycle tests use synthetic child processes.
Configuration checks constrain the selected settings; they are not an OS network
sandbox. Original logs and receipts belong only in fresh ignored output folders.
"""
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import subprocess
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import data_path, load_framework, sha256_file, verify_hash
from tools.provenance import validate_receipt, runtime_identity
from tools.targets import get_target, validate_target_profile
from tools.verified_spu_jobs import fresh_output
from tools.psarc import parse as parse_psarc

TARGET = 'retail-v2.12'
MEMBER = '/run/mag/ps3/settings/maglocalsettings.lua'
# The installed title reads this higher-priority archive before game.psarc.
ARCHIVE = 'game/BCUS98110/USRDIR/tpps.psarc.sdat'
MAX_LOG_BYTES = 8 * 1024 * 1024
FIELDS = {'EnableNetwork', 'EnableServerOnClient', 'UseTestClient', 'UseNPTicket',
          'MAPSServerAddr', 'MAPSServerTCPPort', 'MAPSServerUDPPort'}


def parse_loopback_settings(text):
    """Accept literal configuration only, including the original table + override.

    Executable Lua, unknown fields, duplicate initial tables and dynamic values
    cannot be mistaken for a final static endpoint configuration.
    """
    values, table, initialized = {}, False, False
    debug_table, debug_seen, debug_tag_seen = False, False, False
    literal = r'(true|false|[0-9]+|"[A-Za-z0-9._:-]+")'
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('--'):
            continue
        if re.fullmatch(r'MAGDebugSettings\s*=\s*\{', line):
            if initialized or debug_seen:
                raise ValueError('Static debug settings are allowed only once before network settings')
            debug_table, debug_seen = True, True
            continue
        if debug_table:
            if line in ('}', '};'):
                if not debug_tag_seen:
                    raise ValueError('Static debug settings require one literal dbgTag')
                debug_table = False
                continue
            tag = re.fullmatch(r'dbgTag\s*=\s*"([^"\\\r\n]{0,128})",?', line)
            if not tag or debug_tag_seen or any(not 32 <= ord(char) <= 126 for char in tag[1]):
                raise ValueError('Debug settings allow only one bounded literal dbgTag')
            debug_tag_seen = True
            continue
        if line == 'MAGClientNetConfig = {':
            if initialized or table:
                raise ValueError('Unexpected settings table reset')
            table, initialized = True, True
            continue
        if table and line in ('}', '};'):
            table = False
            continue
        if not table and line == 'MAGClientNetConfig = MAGClientNetConfig or {}':
            initialized = True
            continue
        prefix = '' if table else r'MAGClientNetConfig\.'
        match = re.fullmatch(prefix + r'([A-Za-z][A-Za-z0-9]*)\s*=\s*' + literal + r',?', line)
        if not initialized or not match or match[1] not in FIELDS:
            raise ValueError('Settings contain an unreviewed executable statement or field')
        word = match[2]
        values[match[1]] = (word == 'true' if word in ('true', 'false') else
                            word[1:-1] if word.startswith('"') else int(word))
    if table or debug_table or set(values) != FIELDS:
        raise ValueError('Incomplete literal connection settings')
    expected = {'EnableNetwork': True, 'EnableServerOnClient': False,
                'UseTestClient': False, 'UseNPTicket': False, 'MAPSServerAddr': '127.0.0.1'}
    if any(type(values[key]) is not type(want) or values[key] != want for key, want in expected.items()):
        raise ValueError('Probe settings must use literal loopback with no ticket or local server')
    for field in ('MAPSServerTCPPort', 'MAPSServerUDPPort'):
        if type(values[field]) is not int or not 1 <= values[field] <= 65535:
            raise ValueError('Invalid closed loopback port')
    return values


def validate_settings(hdd_root, receipt_path):
    receipt = json.loads(receipt_path.read_text())
    if receipt.get('archive') != ARCHIVE:
        raise ValueError('Offline settings proof must name the authoritative HDD tpps archive')
    archive = hdd_root / ARCHIVE
    verify_hash(archive, receipt['output_sha256'])
    if receipt.get('member') != MEMBER:
        raise ValueError('Offline settings receipt names another archive member')
    entries, names, extract, *_ = parse_psarc(archive.read_bytes())
    if names.count(MEMBER) != 1:
        raise ValueError('Offline settings member must occur exactly once')
    payload = extract(names.index(MEMBER) + 1)
    import hashlib
    if hashlib.sha256(payload).hexdigest() != receipt['after_member_sha256']:
        raise ValueError('Offline settings member identity changed')
    values = parse_loopback_settings(payload.decode('utf-8'))
    if (receipt.get('members_verified') != len(entries) or
            receipt.get('unrelated_members_unchanged') != len(entries) - 1 or
            values['MAPSServerTCPPort'] != receipt.get('loopback_tcp') or
            values['MAPSServerUDPPort'] != receipt.get('loopback_udp')):
        raise ValueError('Offline settings proof does not match its archive or endpoint metadata')
    return dict(archive=str(archive), archive_sha256=receipt['output_sha256'],
                member_sha256=receipt['after_member_sha256'], receipt_sha256=sha256_file(receipt_path), values=values)


class ClosedLoopback:
    """Keep TCP non-listening and UDP non-serving, preventing another bind."""
    def __init__(self, tcp_port, udp_port):
        self.tcp_port, self.udp_port = tcp_port, udp_port
        self.sockets = []

    def __enter__(self):
        try:
            for kind, field in ((socket.SOCK_STREAM, 'tcp_port'), (socket.SOCK_DGRAM, 'udp_port')):
                held = socket.socket(socket.AF_INET, kind)
                self.sockets.append(held)
                held.bind(('127.0.0.1', getattr(self, field)))
                setattr(self, field, held.getsockname()[1])
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def proof(self):
        return dict(host='127.0.0.1', tcp_port=self.tcp_port, udp_port=self.udp_port,
                    tcp_bound_without_listen=True, udp_bound_without_service=True)

    def __exit__(self, *unused):
        for held in self.sockets:
            held.close()
        self.sockets.clear()


def clean_environment(source, disc_root, hdd_root, output, loopback_sockets=False, image='EBOOT', latest_frame=False):
    # Keep basic locale/user lookup only. No inherited game, loader, input,
    # network proxy, ticket, scripting, automation or rendering variables.
    env = {key: value for key, value in source.items() if key in
           ('HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'LC_CTYPE', 'TZ')}
    guest_paths = {'EBOOT': '/dev_hdd0/game/BCUS98110/USRDIR/EBOOT.BIN',
                   'mage_g': '/dev_hdd0/game/BCUS98110/USRDIR/bin/mage_g.self'}
    if image not in guest_paths:
        raise ValueError('Unknown 2.12 boot image')
    env.update(PS3_BOOT_PATH=guest_paths[image], PATH=os.defpath, TMPDIR=str(output / 'tmp'), XDG_CACHE_HOME=str(output / 'cache'),
               PS3_VFS_ROOT=str(disc_root), PS3_HDD0_ROOT=str(hdd_root),
               PS3RECOMP_METAL_HEADLESS='1',
               PS3RECOMP_METAL_FRAME_DUMP=str(output / 'frame.ppm'),
               PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK='1',
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               PS3_MUTE='1', PAD_NO_KEYBOARD='1', SDL_JOYSTICK_HIDAPI='0',
               SDL_JOYSTICK_IOKIT='0', SDL_JOYSTICK_MFI='0',
               SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xFFFF/0xFFFF',
               SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='0',
               MAG_LOCAL_ACCOUNT_NAME='Port212Test', MAG_LOCAL_NO_TICKET='1',
               MAG_NATIVE_CONSUMER='0', PS3_HLE_UNRESOLVED='halt', PS3_SYSCALL_UNRESOLVED='halt')
    if latest_frame:
        env['PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK'] = '0'
    if loopback_sockets:
        env['PS3_NET_ONLINE'] = '1'
        env['PS3_NETCTL_IPV4_OVERRIDE'] = '127.0.0.1'
    return env


def validate_runtime_receipt(receipt):
    framework, framework_path = load_framework()
    strict = set(framework.get('strict_runtime_revisions', [])) & set(framework.get('strict_hle_runtime_revisions', []))
    candidate = receipt.get('runtime_candidate')
    if candidate:
        if not isinstance(candidate, dict) or candidate.get('base_runtime_revision') not in strict:
            raise ValueError('Runtime candidate base must support both strict failure policies')
        manifest = Path(candidate['manifest_path'])
        verify_hash(manifest, candidate['manifest_sha256'])
        actual = runtime_identity(Path(receipt['sdk_path']), framework, framework_path, manifest, TARGET)
        if actual.get('runtime_candidate') != candidate:
            raise ValueError('Runtime candidate identity changed since build')
    else:
        if receipt['sdk_revision'] not in strict:
            raise ValueError('Runtime must support both strict failure policies')
        actual = runtime_identity(Path(receipt['sdk_path']), framework, framework_path, target=TARGET)
    if actual['sdk_revision'] != receipt['sdk_revision'] or actual['sdk_path'] != receipt['sdk_path']:
        raise ValueError('Runtime SDK identity changed since build')
    return actual


def validate_inputs(binary, elf, image, disc_root, hdd_root, settings_receipt):
    definition = get_target(TARGET)
    profile_path = data_path(definition.profile)
    manifest = json.loads(profile_path.read_text())
    validate_target_profile(manifest, TARGET)
    expected = manifest['images'][image]['elf_sha256']
    verify_hash(elf, expected)
    receipt = validate_receipt(binary, expected, target=TARGET)
    framework = load_framework()[0]
    lift = receipt.get('lift_provenance', {})
    runtime = validate_runtime_receipt(receipt)
    if (receipt.get('image_name') != image or
            lift.get('target') != TARGET or lift.get('lifter_role') != 'lifter-ppu-v212' or
            lift.get('sdk_revision') != framework['ps3recomp']['lifter-ppu-v212']['commit'] or
            any(value for key, value in lift.items() if 'hooks' in key or key.startswith('nonlocal'))):
        raise ValueError('Probe requires the exact selected 2.12 image and reviewed build receipt')
    if (disc_root == hdd_root or disc_root.is_relative_to(hdd_root) or hdd_root.is_relative_to(disc_root) or
            not (disc_root / 'PS3_GAME').is_dir() or
            not (hdd_root / 'game/BCUS98110/PARAM.SFO').is_file()):
        raise ValueError('Probe requires explicit separate disc and installed HDD roots')
    # Bind both original copied update executables, not just their directory name.
    for relative, (source_hash, _) in definition.executables.items():
        verify_hash(hdd_root / 'game/BCUS98110/USRDIR' / relative, source_hash)
    settings = validate_settings(hdd_root, settings_receipt)
    return dict(target=TARGET, image=image, binary=str(binary), elf=str(elf),
                binary_sha256=receipt['binary_sha256'], elf_sha256=expected,
                build_receipt_sha256=sha256_file(binary.with_suffix('.receipt.json')),
                profile_sha256=sha256_file(profile_path), disc_root=str(disc_root), hdd_root=str(hdd_root),
                settings=settings, runtime_validation=runtime, not_runtime_verified=True)


def run_owned(command, environment, output, seconds, identity=None, log_limit=MAX_LOG_BYTES):
    if (isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or
            not math.isfinite(seconds) or not 1 <= seconds <= 60 or
            type(log_limit) is not int or not 1 <= log_limit <= MAX_LOG_BYTES):
        raise ValueError('Probe deadline must be 1-60 seconds and logs must stay within the bounded cap')
    if os.name != 'posix' or not hasattr(os, 'waitid') or not hasattr(os, 'WNOWAIT'):
        raise ValueError('Owned bootstrap lifecycle currently requires POSIX waitid/WNOWAIT')
    output = fresh_output(output)
    for name in ('tmp', 'cache'):
        (output / name).mkdir(mode=0o700)
    result = dict(identity or {}, command=[str(x) for x in command], owner_pid=os.getpid(),
                  deadline_seconds=seconds, headless=True, input='none',
                  started_utc=datetime.now(timezone.utc).isoformat(),
                  pid=None, owned_process_group=None, ownership_verified=False,
                  timed_out=False, log_limit_reached=False, term_sent=False, kill_sent=False,
                  exit_code=None, reaped=False, errors=[])
    child, selector, written = None, selectors.DefaultSelector(), 0
    start = time.monotonic()

    def status_without_reap():
        return os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)

    def live_group_members():
        # Read only PID/group/state metadata, retaining only this proven group.
        rows = subprocess.check_output(['/bin/ps', '-axo', 'pid=,pgid=,stat='],
                                       text=True, timeout=2, env={'PATH': os.defpath, 'LC_ALL': 'C'})
        members = []
        for line in rows.splitlines():
            fields = line.split()
            if len(fields) == 3 and int(fields[1]) == result['owned_process_group'] and not fields[2].startswith('Z'):
                members.append(int(fields[0]))
        return members

    def group_signal(kind):
        # The unreaped session leader reserves its PID/PGID throughout cleanup.
        if not result['ownership_verified'] or result['owned_process_group'] in (None, os.getpgrp()):
            raise ValueError('Owned process group proof is missing')
        try:
            os.killpg(result['owned_process_group'], kind)
            result['term_sent' if kind == signal.SIGTERM else 'kill_sent'] = True
        except ProcessLookupError:
            pass

    with (output / 'console.log').open('xb') as log:
        def drain(wait=0):
            nonlocal written
            for key, _ in selector.select(wait):
                try:
                    chunk = os.read(key.fd, 65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    selector.unregister(key.fd)
                    continue
                available = log_limit - written
                log.write(chunk[:available])
                log.flush()
                written += min(available, len(chunk))
                if written >= log_limit:
                    result['log_limit_reached'] = True
        try:
            child = subprocess.Popen(command, env=environment, cwd=output,
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, close_fds=True, start_new_session=True)
            result.update(pid=child.pid, owned_process_group=os.getpgid(child.pid), session_id=os.getsid(child.pid))
            if result['owned_process_group'] != child.pid or result['session_id'] != child.pid:
                raise ValueError('Child did not establish its exclusive process group/session')
            result['ownership_verified'] = True
            (output / 'ownership.json').write_text(json.dumps(result, indent=2) + '\n')
            os.set_blocking(child.stdout.fileno(), False)
            selector.register(child.stdout.fileno(), selectors.EVENT_READ)
            deadline = time.monotonic() + seconds
            while True:
                drain()
                if status_without_reap() is not None:
                    result['leader_exited_before_cleanup'] = True
                    break
                if result['log_limit_reached']:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    result['timed_out'] = True
                    break
                drain(min(0.05, remaining))
        except (Exception, KeyboardInterrupt) as error:
            result['errors'].append(type(error).__name__ + ': ' + str(error))
        finally:
            if child is not None:
                try:
                    if result['ownership_verified']:
                        members = live_group_members()
                        result['live_group_before_cleanup'] = members
                        if members:
                            group_signal(signal.SIGTERM)
                            grace = time.monotonic() + 0.2
                            while time.monotonic() < grace:
                                drain(min(0.02, max(0, grace - time.monotonic())))
                        if live_group_members():
                            group_signal(signal.SIGKILL)
                            # Allow killed descendants to publish exit state.
                            time.sleep(0.05)
                        result['live_group_after_cleanup'] = live_group_members()
                        result['group_cleanup_verified'] = not result['live_group_after_cleanup']
                    else:
                        child.kill()
                        result['group_cleanup_verified'] = False
                except Exception as error:
                    result['errors'].append('cleanup: ' + type(error).__name__ + ': ' + str(error))
                    result['group_cleanup_verified'] = False
                    # Even a denied group operation cannot skip direct-child reap.
                    child.kill()
                finally:
                    result['exit_code'] = child.wait(timeout=2)
                    result['reaped'] = True
                    drain()
                    child.stdout.close()
            selector.close()
    result.update(elapsed_seconds=round(time.monotonic() - start, 3), log_bytes=written,
                  console_sha256=sha256_file(output / 'console.log'),
                  cleanup=('owned group verified stopped; direct child reaped'
                           if result.get('group_cleanup_verified') and result['reaped']
                           else 'cleanup incomplete; inspect retained ownership and errors'))
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def probe(binary, elf, image, disc_root, hdd_root, settings_receipt, output, seconds=15, loopback_sockets=False, latest_frame=False):
    binary, elf = binary.resolve(strict=True), elf.resolve(strict=True)
    disc_root, hdd_root = disc_root.resolve(strict=True), hdd_root.resolve(strict=True)
    identity = validate_inputs(binary, elf, image, disc_root, hdd_root, settings_receipt)
    settings = identity['settings']['values']
    retained_inputs = {str(path): sha256_file(path) for path in
                       (binary, elf, binary.with_suffix('.receipt.json'), settings_receipt,
                        hdd_root / ARCHIVE, hdd_root / 'game/BCUS98110/PARAM.SFO',
                        data_path(get_target(TARGET).profile))}
    with ClosedLoopback(settings['MAPSServerTCPPort'], settings['MAPSServerUDPPort']) as held:
        identity['loopback_reservations'] = held.proof()
        identity['loopback_sockets'] = loopback_sockets
        identity['frame_capture_mode'] = 'periodic_latest' if latest_frame else 'first_nonblack'
        identity['network_scope'] = 'verified loopback settings; non-serving reservations; no OS egress sandbox'
        verify_hash(binary, identity['binary_sha256'])
        verify_hash(elf, identity['elf_sha256'])
        env = clean_environment(os.environ, disc_root, hdd_root, output.resolve(), loopback_sockets, image, latest_frame)
        result = run_owned([str(binary), str(elf)], env, output, seconds, identity)
    result['reservations_released'] = True
    result['input_changes_after_run'] = [path for path, expected in retained_inputs.items()
                                         if not Path(path).is_file() or sha256_file(Path(path)) != expected]
    if result['input_changes_after_run']:
        result['errors'].append('Validated inputs changed during the bounded probe')
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--image', choices=('EBOOT', 'mage_g'), default='EBOOT')
    parser.add_argument('--disc-root', type=Path, required=True)
    parser.add_argument('--hdd-root', type=Path, required=True)
    parser.add_argument('--settings-receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='fresh private ignored evidence directory')
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--latest-frame', action='store_true', help='overwrite the owned frame.ppm every 120 SDK capture calls instead of first-nonblack capture')
    parser.add_argument('--loopback-sockets', action='store_true', help='enable native host sockets only with the proven reserved loopback settings')
    args = parser.parse_args()
    result = probe(args.binary, args.elf, args.image, args.disc_root, args.hdd_root,
                   args.settings_receipt, args.output, args.seconds, args.loopback_sockets, args.latest_frame)
    print(json.dumps({key: result[key] for key in ('pid', 'owned_process_group', 'exit_code', 'timed_out',
                                                  'log_limit_reached', 'reaped', 'elapsed_seconds', 'errors')}, indent=2))
    return 0 if result['exit_code'] == 0 and not result['errors'] and not result['timed_out'] and not result['log_limit_reached'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
