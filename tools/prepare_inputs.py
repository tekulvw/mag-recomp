"""Validate separately decrypted retail inputs; never launch an emulator."""
import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.targets import DEFAULT_TARGET, TARGET_NAMES, get_target, validate_target_output, validate_target_profile

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
RETAIL = dict(get_target().executables)
# The settings archive the player-settings server overlay rewrites: the retail
# SDATA file as on the disc, or the same archive already decrypted to PSARC. A
# copy whose settings were changed by any earlier tool is refused before a build.
RETAIL_SETTINGS_ARCHIVE = 'run/mag/ps3/game.psarc.sdat'
RETAIL_SETTINGS_ARCHIVE_SHA256 = (
    'e35e3e40172805422b2a5a21cb1a9b1b38216b2f4f88d1bd505a9c533fd76208',  # SDATA, as on the disc
    'b49c9435be729832029ad7019874d478080dc9764ebde1809b2851d353bc769f',  # the same, decrypted
)


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


def prepare_inputs(source_root: Path, output_root: Path, target=DEFAULT_TARGET) -> dict:
    selected = get_target(target)
    validate_target_output(output_root, target, WORKSPACE)
    images = {}
    for name, (source_hash, elf_hash) in selected.executables.items():
        source = source_root / name
        copy = output_root / name
        elf = copy.with_suffix('.elf')
        verify_hash(source, source_hash)
        verify_hash(copy, source_hash)
        verify_hash(elf, elf_hash)
        images[elf.stem] = dict(source=str(source), local=str(copy), elf=str(elf),
                               source_sha256=source_hash, elf_sha256=elf_hash,
                               **validate_ppu_elf(elf))
    result = dict(title_id=selected.title_id, app_version=selected.app_version, images=images)
    if target != DEFAULT_TARGET:
        result['target'] = target
    return result


def write_image_profile(result: dict, target=DEFAULT_TARGET) -> Path:
    """Write this machine's image profile into the workspace, creating its profiles/ directory."""
    selected = get_target(target)
    validate_target_profile(result, target)
    path = data_path(selected.profile)
    validate_target_output(path, target, WORKSPACE)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2)+'\n')
    return path


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=TARGET_NAMES, default=DEFAULT_TARGET)
    p.add_argument('--source-root', type=Path, required=True, help='Original selected-build USRDIR (disc v1.0 or official 2.12 update)')
    p.add_argument('--local-root', type=Path, help='Copied SELF and independently decrypted ELF directory; target-specific default')
    p.add_argument('--installation', type=Path, help='Also verify a complete assembled 2.12 installation before writing its profile')
    a = p.parse_args()
    selected = get_target(a.target)
    result = prepare_inputs(a.source_root, a.local_root or data_path(selected.local_usrdir), a.target)
    if a.installation:
        if a.target != 'retail-v2.12':
            p.error('--installation verifies only the prepared 2.12 update layout')
        from tools.prepare_v212_install import verify, PROFILE
        verify(a.installation, json.loads(PROFILE.read_text()))
        expected_source = (a.installation / 'dev_hdd0/game/BCUS98110/USRDIR').resolve()
        if a.source_root.resolve() != expected_source:
            p.error('--source-root must be the selected installation HDD update USRDIR')
        result['installation'] = {'root': str(a.installation.resolve()),
                                  'receipt_sha256': sha256_file(a.installation / 'installation-receipt.json')}
    write_image_profile(result, a.target)
    print(f'Validated original, copied SELF and decrypted ELF hashes for both {a.target} executables.')
