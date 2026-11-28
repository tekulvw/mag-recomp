"""Validate the exact 2.12 engine's reviewed original nonlocal-flow mapping.

Only source identities, addresses, counts and hashes are retained. The pinned
lifter executes the original register restore and transfers to a live matching
SP/LR/context/thread continuation; this tool never emulates a jmp_buf layout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.inventory import check_sdk
from tools.v212_launcher_profile import virtual_bytes, verify_guard_spans

ROOT = Path(__file__).resolve().parents[1]
ELF_SHA256 = '705a6ec6a25458b2401b869e75e2904597304e2c06d9534a171a1dcd54aca2b3'
SDK_REVISION = 'e01945ed11bb63989066e0be9e711b4d33621013'
PROFILE_SHA256 = 'c5c9483086403f457f1200649d49e69e35952386e9e7305aee4897c7e3a65658'
PROFILE_NAME = 'retail-v2.12-nonlocal.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def _review(profile_path):
    path = Path(profile_path)
    if path.is_symlink() or not path.is_file():
        raise ValueError('Nonlocal profile must be a regular file')
    data = path.read_bytes()
    if digest(data) != PROFILE_SHA256:
        raise ValueError('2.12 nonlocal review identity mismatch')
    profile = json.loads(data)
    name = profile['configuration']
    if not name or Path(name).name != name:
        raise ValueError('Nonlocal configuration must be a local basename')
    config = path.parent / name
    if config.is_symlink() or not config.is_file():
        raise ValueError('Nonlocal configuration must be a regular sibling file')
    if digest(config.read_bytes()) != profile['configuration_sha256']:
        raise ValueError('2.12 nonlocal configuration identity mismatch')
    return config, dict(profile, profile_sha256=PROFILE_SHA256)


def validate_record(record, elf_hash):
    """Verify generated metadata without an SDK or any original executable.

    All fields must equal the exact checked-in review, including configuration,
    target, role and tool hashes. Unknown or missing keys cannot widen its scope.
    """
    _, reviewed = _review(ROOT/'profiles'/PROFILE_NAME)
    if elf_hash != ELF_SHA256 or not isinstance(record, dict) or record != reviewed:
        raise ValueError('2.12 nonlocal generated review identity mismatch')
    return dict(reviewed)


def verify_source(data, record):
    if digest(data) != ELF_SHA256 or record['elf_sha256'] != ELF_SHA256:
        raise ValueError('2.12 nonlocal ELF identity mismatch')
    spans = [dict(span, address=int(span['address'], 0)) for span in record['guard_spans']]
    verify_guard_spans(data, spans)
    # Audit the entire source code range for direct linked calls to both TOC
    # stubs and original save/restore entries, rather than copying a v1 mapping.
    code = virtual_bytes(data, 0x10000, 0x1161f4c-0x10000)
    census = {key: [] for key in record['direct_linked_call_census']}
    targets = {int(key, 0): key for key in census}
    for offset in range(0, len(code), 4):
        word = struct.unpack_from('>I', code, offset)[0]
        if word & 0xfc000003 != 0x48000001:
            continue
        delta = word & 0x03fffffc
        if delta & 0x02000000:
            delta -= 0x04000000
        target = (0x10000 + offset + delta) & 0xffffffff
        if target in targets:
            census[targets[target]].append(f'0x{0x10000+offset:08X}')
    if census != record['direct_linked_call_census']:
        raise ValueError('2.12 nonlocal direct call census mismatch')


def validate_profile(profile_path, sdk_root, elf_path, image_name='mage_g'):
    """Return (configuration Path, strict reviewed record), before generation."""
    config, record = _review(profile_path)
    if image_name != 'mage_g':
        raise ValueError('2.12 nonlocal profile belongs only to mage_g')
    validate_record(record, record['elf_sha256'])
    verify_source(Path(elf_path).read_bytes(), record)
    check_sdk(Path(sdk_root), expected_revision=SDK_REVISION, role='lifter-ppu-v212')
    if digest((Path(sdk_root)/'tools/ppu_lifter.py').read_bytes()) != record['lifter_sha256']:
        raise ValueError('2.12 nonlocal lifter source identity mismatch')
    return config, record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, default=ROOT/'profiles'/PROFILE_NAME)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    args = parser.parse_args()
    _, record = validate_profile(args.profile, args.sdk, args.elf)
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
