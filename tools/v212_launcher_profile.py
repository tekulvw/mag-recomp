"""Offline verification of the exact 2.12 launcher's semantic address profile.

Only identities and mapping metadata are returned. No payload, key or original
instruction data is exported. Native execution acceptance remains separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[1]
ELF_SHA256='feb5ed5a3e8f3aab74601ae3edc737e3c010208523d3eb322be997cbe3d79318'


def load_profile():
    profile=json.loads((ROOT/'profiles/retail-v2.12-launcher.json').read_text())
    if (profile.get('version')!=1 or profile.get('target')!='retail-v2.12'
            or profile.get('elf_sha256')!=ELF_SHA256):
        raise ValueError('Launcher profile identity mismatch')
    return profile


def virtual_bytes(data, address, length):
    """Read only an unambiguous, fully file-backed ELF64 BE PPC load range."""
    if (len(data)<64 or data[:7]!=b'\x7fELF\x02\x02\x01'
            or struct.unpack_from('>H',data,18)[0]!=21):
        raise ValueError('Expected ELF64 big-endian PowerPC64 image')
    if not isinstance(address,int) or not isinstance(length,int) or length<0 or not 0<=address<2**32 or length>2**32-address:
        raise ValueError('Invalid guest virtual range')
    offset=struct.unpack_from('>Q',data,32)[0]
    stride,count=struct.unpack_from('>HH',data,54)
    if stride<56 or offset>len(data) or count>(len(data)-offset)//stride:
        raise ValueError('Invalid ELF program header table')
    matches=[]
    for i in range(count):
        kind,_,file_offset,vaddr,_,filesz,memsz,_=struct.unpack_from('>IIQQQQQQ',data,offset+i*stride)
        if kind!=1:continue
        if filesz>memsz or file_offset>len(data) or filesz>len(data)-file_offset:
            raise ValueError('Invalid ELF load segment')
        if vaddr<=address and address-vaddr<=filesz and length<=filesz-(address-vaddr):
            start=file_offset+address-vaddr
            matches.append(data[start:start+length])
    if len(matches)!=1:
        raise ValueError('Guest virtual range is not uniquely file-backed')
    return matches[0]


def verify_guard_spans(data, spans):
    for span in spans:
        digest=hashlib.sha256(virtual_bytes(data,span['address'],span['length'])).hexdigest()
        if digest!=span['sha256']:
            raise ValueError('Launcher semantic guard identity mismatch: '+span.get('name','unnamed'))


def verify_launcher(path):
    profile=load_profile()
    data=Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest()!=profile['elf_sha256']:
        raise ValueError('Launcher ELF identity mismatch')
    verify_guard_spans(data,profile['guard_spans'])
    return {'target':profile['target'],'elf_sha256':profile['elf_sha256'],
            'guard_spans_verified':len(profile['guard_spans']),
            'runtime_accepted':False,'unresolved':profile['unresolved']}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf',type=Path)
    args=parser.parse_args()
    print(json.dumps(verify_launcher(args.elf),indent=2))


if __name__=='__main__':main()
