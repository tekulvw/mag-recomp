#!/usr/bin/env python3
"""Owner-readiness-gated, headless, auth-only 2.12 launcher probe.

The private readiness record is an assertion supplied by the isolated server's
owner, not discovered through a preflight connection. This tool never starts a
server, sends controller input, changes accounts or performs engine handoff.
Configuration validation is not an OS egress sandbox. Retain output privately.

Normalized readiness schema: schema_version=1, ready=true, target=retail-v2.12,
protocol=1793, application_id=20231, integration_branch=codex/retail-v2.12,
source_pr=440, server_source (40 hex), image_sha256/data_sha256 (64 hex),
auth_only=true, pod/service {namespace,name,uid,target}, endpoint {host,tcp_port}.
The literal non-global IPv4 host comes only from that qualified owner record;
the selected TCP10073 and retained diagnostic UDP10072 fields must match it.
"""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.v212_boot_probe import (ARCHIVE, TARGET, clean_environment as offline_environment,
    parse_literal_settings, run_owned, validate_archive_settings, validate_build_inputs)
from tools.prepare_inputs import data_path, sha256_file, verify_hash
from tools.targets import get_target

TCP_PORT = 10073
LEGACY_UDP_PORT = 10072
ACCOUNT = 'v212probe'


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Readiness record contains duplicate fields')
        result[key] = value
    return result


def _private_host(host):
    if not isinstance(host, str):
        raise ValueError('Owner endpoint requires a literal IPv4 address')
    try:
        address = ipaddress.IPv4Address(host)
    except ipaddress.AddressValueError as error:
        raise ValueError('Owner endpoint requires a literal IPv4 address') from error
    if (str(address) != host or address.is_global or not address.is_private or
            address.is_loopback or address.is_link_local or address.is_multicast or
            address.is_unspecified or address.is_reserved):
        raise ValueError('Owner endpoint must be a non-global isolated IPv4 address')
    return host


def _read_readiness(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 65536:
        raise ValueError('Owner readiness must be a bounded ordinary private JSON file')
    payload = path.read_bytes()
    if len(payload) > 65536:
        raise ValueError('Owner readiness record exceeded its bounded size')
    record = json.loads(payload, object_pairs_hook=_unique_object)
    if not isinstance(record, dict):
        raise ValueError('Owner readiness must be a JSON object')
    expected = dict(schema_version=1, ready=True, target=TARGET, protocol=1793,
        application_id=20231, integration_branch='codex/retail-v2.12', source_pr=440, auth_only=True)
    for key, value in expected.items():
        if type(record.get(key)) is not type(value) or record[key] != value:
            raise ValueError('Owner readiness identity mismatch: '+key)
    for key, length in [('server_source', 40), ('image_sha256', 64), ('data_sha256', 64)]:
        if not isinstance(record.get(key), str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}', record[key]):
            raise ValueError('Owner readiness requires immutable source/image/data identities: '+key)
    for key in ('pod', 'service'):
        resource = record.get(key)
        if not isinstance(resource, dict) or set(resource) != {'namespace', 'name', 'uid', 'target'}:
            raise ValueError('Owner readiness requires complete pod and service identities')
        if resource['target'] != TARGET:
            raise ValueError('Owner readiness resource names another target')
        for field in ('namespace', 'name', 'uid'):
            if not isinstance(resource[field], str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.:-]{0,252}', resource[field]):
                raise ValueError('Owner readiness resource identity is invalid: '+field)
    endpoint = record.get('endpoint')
    if (not isinstance(endpoint, dict) or set(endpoint) != {'host', 'tcp_port'} or
            type(endpoint.get('tcp_port')) is not int or endpoint['tcp_port'] != TCP_PORT):
        raise ValueError('Owner readiness must select the isolated MAPS TCP endpoint')
    _private_host(endpoint.get('host'))
    return record, hashlib.sha256(payload).hexdigest()


def load_readiness(path):
    """Validate the owner's normalized private handoff without network access."""
    return _read_readiness(path)[0]


def _connected_values(values, host):
    host = _private_host(host)
    if (values['MAPSServerAddr'] != host or values['MAPSServerTCPPort'] != TCP_PORT or
            values['MAPSServerUDPPort'] != LEGACY_UDP_PORT or
            values.get('PlayerName', ACCOUNT) != ACCOUNT):
        raise ValueError('Connected settings must retain exact isolated TCP and legacy diagnostic UDP fields')
    return values


def parse_connected_settings(text, host):
    return _connected_values(parse_literal_settings(text), host)


def validate_settings(hdd_root, receipt_path, host):
    host = _private_host(host)
    proof, receipt = validate_archive_settings(hdd_root, receipt_path)
    _connected_values(proof['values'], host)
    destination = receipt.get('destination')
    # A descriptive DNS label in the private receipt is never resolved or used
    # to authorize another endpoint. Only the ready-bound literal IPv4 counts.
    if (not isinstance(destination, dict) or destination.get('ip') != host or
            type(destination.get('maps_tcp')) is not int or
            destination['maps_tcp'] != TCP_PORT or type(destination.get('legacy_maps_udp_field')) is not int or
            destination['legacy_maps_udp_field'] != LEGACY_UDP_PORT or
            receipt.get('account_name') != ACCOUNT or receipt.get('use_np_ticket') is not False):
        raise ValueError('Connected settings proof does not match the selected disposable account and endpoint')
    return proof


def clean_environment(source, disc_root, hdd_root, output):
    env = offline_environment(source, disc_root, hdd_root, output, image='EBOOT')
    env.update(MAG_LOCAL_ACCOUNT_NAME=ACCOUNT, MAG_LOCAL_NO_TICKET='1', MAG_TRACE_NETWORK='1',
               PS3_NET_ONLINE='1', PS3_NET_TRACE='1')
    # Use the SDK's actual host address discovery. A loopback override here
    # would advertise an unusable client address to the isolated remote service.
    env.pop('PS3_NETCTL_IPV4_OVERRIDE', None)
    return env


def probe(binary, elf, disc_root, hdd_root, settings_receipt, readiness_path, output, seconds=30):
    readiness_path = Path(readiness_path)
    # Parse and bind one bounded read, before build input validation or sockets.
    ready, readiness_hash = _read_readiness(readiness_path)
    verify_hash(readiness_path, readiness_hash)
    binary, elf = Path(binary).resolve(strict=True), Path(elf).resolve(strict=True)
    disc_root, hdd_root = Path(disc_root).resolve(strict=True), Path(hdd_root).resolve(strict=True)
    settings_receipt, output = Path(settings_receipt).resolve(strict=True), Path(output).resolve()
    identity = validate_build_inputs(binary, elf, 'EBOOT', disc_root, hdd_root)
    identity['settings'] = validate_settings(hdd_root, settings_receipt, ready['endpoint']['host'])
    identity.update(owner_readiness=ready, owner_readiness_sha256=readiness_hash,
        account_name=ACCOUNT, auth_only=True, engine_handoff=False,
        network_scope='owner-qualified isolated MAPS TCP; original UDP10072 retained; no OS egress sandbox')
    paths = (binary, elf, binary.with_suffix('.receipt.json'), settings_receipt, readiness_path,
        hdd_root/ARCHIVE, hdd_root/'game/BCUS98110/PARAM.SFO', data_path(get_target(TARGET).profile))
    retained = {str(path): sha256_file(path) for path in paths}
    retained[str(readiness_path)] = readiness_hash
    retained[str(binary)] = identity['binary_sha256']
    retained[str(elf)] = identity['elf_sha256']
    retained[str(binary.with_suffix('.receipt.json'))] = identity['build_receipt_sha256']
    retained[str(data_path(get_target(TARGET).profile))] = identity['profile_sha256']
    retained[str(hdd_root/ARCHIVE)] = identity['settings']['archive_sha256']
    retained[str(settings_receipt)] = identity['settings']['receipt_sha256']
    for relative, (source_hash, _) in get_target(TARGET).executables.items():
        path = hdd_root/'game/BCUS98110/USRDIR'/relative
        retained[str(path)] = source_hash
    for path, digest in retained.items():
        verify_hash(Path(path), digest)
    env = clean_environment(os.environ, disc_root, hdd_root, output)
    result = run_owned([str(binary), str(elf)], env, output, seconds, identity)
    result['input_changes_after_run'] = [path for path, digest in retained.items()
        if not Path(path).is_file() or sha256_file(Path(path)) != digest]
    if result['input_changes_after_run']:
        result['errors'].append('Validated inputs or owner readiness changed during the connected probe')
    (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--disc-root', type=Path, required=True)
    parser.add_argument('--hdd-root', type=Path, required=True)
    parser.add_argument('--settings-receipt', type=Path, required=True)
    parser.add_argument('--owner-readiness', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=30)
    args = parser.parse_args()
    result = probe(args.binary, args.elf, args.disc_root, args.hdd_root, args.settings_receipt,
                   args.owner_readiness, args.output, args.seconds)
    print(json.dumps(result, indent=2))
    return 0 if result.get('group_cleanup_verified') and not result.get('errors') else 1


if __name__ == '__main__':
    raise SystemExit(main())
