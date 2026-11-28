"""Verify source identities for the isolated 2.12 engine account adapter.

This offline check establishes exact ELF and semantic spans only. It does not
claim runtime acceptance, server readiness, NP status or protocol compatibility.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from tools.v212_launcher_profile import verify_guard_spans, virtual_bytes

ROOT = Path(__file__).resolve().parents[1]
ELF_SHA256 = '705a6ec6a25458b2401b869e75e2904597304e2c06d9534a171a1dcd54aca2b3'


def load_profile():
    profile = json.loads((ROOT/'profiles/retail-v2.12-engine-network.json').read_text())
    if (profile.get('version') != 1 or profile.get('target') != 'retail-v2.12'
            or profile.get('image_name') != 'mage_g'
            or profile.get('elf_sha256') != ELF_SHA256):
        raise ValueError('Engine network profile identity mismatch')
    return profile


def direct_branch_references(data, start, end, target):
    """All relative/absolute I-form and B-form branches to one code entry."""
    if start % 4 or end % 4 or end <= start:
        raise ValueError('Invalid connect census code range')
    code = virtual_bytes(data, start, end-start)
    found = []
    for offset in range(0, len(code), 4):
        word = struct.unpack_from('>I', code, offset)[0]
        opcode = word >> 26
        if opcode == 18:
            delta = word & 0x03fffffc
            if delta & 0x02000000: delta -= 0x04000000
        elif opcode == 16:
            delta = word & 0xfffc
            if delta & 0x8000: delta -= 0x10000
        else:
            continue
        destination = delta if word & 2 else start + offset + delta
        if destination & 0xffffffff == target:
            found.append(start+offset)
    return found


def verify_engine(path):
    profile = load_profile()
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != profile['elf_sha256']:
        raise ValueError('Engine ELF identity mismatch')
    verify_guard_spans(data, profile['guard_spans'])
    connect = profile['dispatch']['connect']
    references = direct_branch_references(data, connect['code_start'], connect['code_end'], connect['code'])
    if connect['direct_branch_references'] != 0 or references:
        raise ValueError('Final connect has direct branch references that bypass registry dispatch')
    return dict(target=profile['target'], image_name=profile['image_name'],
                elf_sha256=profile['elf_sha256'],
                guard_spans_verified=len(profile['guard_spans']),
                connect_direct_branch_references=len(references), runtime_accepted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify_engine(args.elf), indent=2))


if __name__ == '__main__':
    main()
