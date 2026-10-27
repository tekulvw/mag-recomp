"""Validate separately decrypted retail inputs; never launch an emulator."""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Working data (inputs, SDK checkouts, analysis, generated sources, runs and per-machine profiles)
# lives in the workspace; code and pinned profiles always come from ROOT.
WORKSPACE = Path(os.environ['MAG_WORKSPACE']).resolve() if os.environ.get('MAG_WORKSPACE') else ROOT
FRAMEWORK_OVERLAY = 'profiles/framework.json'


def data_path(relative) -> Path:
    return WORKSPACE / relative


def load_framework():
    """The SDK pins in effect: a workspace's own framework file replaces the public one."""
    path = ROOT / FRAMEWORK_OVERLAY
    if WORKSPACE != ROOT and (WORKSPACE / FRAMEWORK_OVERLAY).is_file():
        path = WORKSPACE / FRAMEWORK_OVERLAY
    return json.loads(path.read_text()), path
RETAIL = {
    'EBOOT.BIN': ('ebe4a52656cc38483d24656029a55ddef6fd53adc7f23b4c52c42f272ac8d1f9',
                  'eb916f92e7c32a9401739d79835630273e1f41a92e6a5d17753d9374062da7d9'),
    'bin/mage_g.self': ('54b324a39e5453916768d8e83057ea72865cf53b23a1a8f058e25915d401840c',
                        '0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c'),
}


def sha256_file(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def verify_hash(path: Path, expected: str) -> str:
    actual = sha256_file(path)
    if actual != expected:
        raise ValueError(f'Build identity mismatch: {path}: {actual} != {expected}')
    return actual


def validate_ppu_elf(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x02\x01':
        raise ValueError(f'Expected complete ELF64 big-endian image: {path}')
    typ, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstr = struct.unpack_from('>HHIQQQIHHHHHH', data, 16)
    if (typ, machine, version, ehsize, phsize) != (2, 21, 1, 64, 56) or not phnum:
        raise ValueError('Expected PPU ET_EXEC image with standard ELF64 headers')
    if phoff < 64 or phoff + phsize * phnum > len(data):
        raise ValueError('Program header table outside file')
    if shnum and (shsize != 64 or shoff + shsize * shnum > len(data)):
        raise ValueError('Section header table outside file')
    loads = []
    for i in range(phnum):
        t, f, offset, va, pa, size, memsize, align = struct.unpack_from('>IIQQQQQQ', data, phoff + i * phsize)
        if offset + size > len(data):
            raise ValueError(f'Segment {i} outside file')
        if t == 1 and memsize:
            if size > memsize or va + memsize > 2**32:
                raise ValueError(f'Invalid PPU memory range for segment {i}')
            loads.append(dict(vaddr=va, offset=offset, filesz=size, memsz=memsize, flags=f))
    descriptor = next((s for s in loads if s['vaddr'] <= entry and entry + 8 <= s['vaddr'] + s['filesz']), None)
    if descriptor is None:
        raise ValueError('Entry descriptor outside file-backed load segments')
    code, toc = struct.unpack_from('>II', data, descriptor['offset'] + entry - descriptor['vaddr'])
    if code % 4 or not any(s['flags'] & 1 and s['vaddr'] <= code < s['vaddr'] + s['filesz'] for s in loads):
        raise ValueError('Entry descriptor does not resolve to executable code')
    return dict(bytes=len(data), entry_opd=hex(entry), entry_code=hex(code), entry_toc=hex(toc),
                program_headers=phnum, section_headers=shnum, load_segments=loads)


def prepare_inputs(source_root: Path, output_root: Path) -> dict:
    images = {}
    for name, (source_hash, elf_hash) in RETAIL.items():
        source = source_root / name
        copy = output_root / name
        elf = copy.with_suffix('.elf')
        verify_hash(source, source_hash)
        verify_hash(copy, source_hash)
        verify_hash(elf, elf_hash)
        images[elf.stem] = dict(source=str(source), local=str(copy), elf=str(elf),
                               source_sha256=source_hash, elf_sha256=elf_hash,
                               **validate_ppu_elf(elf))
    return dict(title_id='BCUS98110', app_version='01.00', images=images)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, required=True, help='PS3_GAME/USRDIR of your own BCUS98110 v1.0 disc copy')
    p.add_argument('--local-root', type=Path, default=data_path('local/retail-v1.0/PS3_GAME/USRDIR'))
    a = p.parse_args()
    result = prepare_inputs(a.source_root, a.local_root)
    (ROOT/'profiles').mkdir(exist_ok=True)
    (data_path('profiles/retail-v1.0.json')).write_text(json.dumps(result, indent=2)+'\n')
    print('Validated original, copied SELF and decrypted ELF hashes for both v1.0 executables.')
