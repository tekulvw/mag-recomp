"""Assemble/verify an offline BCUS98110 disc 2.12 data layout; never boot a game."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'profiles/retail-v2.12-inputs.json'
RECEIPT = 'installation-receipt.json'


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def relative(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value:
        raise ValueError(f'Invalid relative path: {value!r}')
    path = PurePosixPath(value)
    if path.is_absolute() or '..' in path.parts or str(path) != value or value == '.':
        raise ValueError(f'Invalid relative path: {value!r}')
    return path


def checked_file(root, name):
    path = root
    if root.is_symlink():
        raise ValueError(f'Input root is a symlink: {root}')
    for part in relative(name).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f'Input contains a symlink: {path}')
    if not path.is_file():
        raise ValueError(f'Missing regular file: {path}')
    return path


def check_identity(path, row):
    if not path.is_file() or path.stat().st_size != row['size'] or digest(path) != row['sha256']:
        raise ValueError(f'Input identity mismatch: {path}')


def validate_files(root, rows):
    for row in rows:
        check_identity(checked_file(root, row['path']), row)


def profile_digest(profile):
    return hashlib.sha256(json.dumps(profile, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def validate_profile(profile):
    if profile.get('schema') != 1 or profile.get('title_id') != 'BCUS98110':
        raise ValueError('Unsupported installation profile')
    for section in ('disc_files', 'update_files', 'patches'):
        seen = set()
        if not profile.get(section):
            raise ValueError(f'Empty profile section: {section}')
        for row in profile[section]:
            name = str(relative(row['name' if section == 'patches' else 'path']))
            if section == 'patches' and '/' in name:
                raise ValueError('Patch names must be basenames')
            if name.casefold() in seen:
                raise ValueError(f'Duplicate profile path: {name}')
            seen.add(name.casefold())
            if not isinstance(row['size'], int) or row['size'] < 0:
                raise ValueError(f'Invalid size: {name}')
            if len(row['sha256']) != 64 or any(c not in '0123456789abcdef' for c in row['sha256']):
                raise ValueError(f'Invalid SHA-256: {name}')
    if profile['settings_source'] not in {r['path'] for r in profile['disc_files']}:
        raise ValueError('Settings source is absent from disc profile')
    level = 0
    for patch in profile['patches']:
        if patch['requires'] != level or patch['provides'] <= level:
            raise ValueError('Invalid profile patch dependency chain')
        level = patch['provides']
    if level != profile['content_level']:
        raise ValueError('Profile patch dependency chain has wrong final level')
    output_files(profile)  # Reject destination collisions before touching output.


def patch_dependency(path):
    """Read only the names and patch.xml from PSARC 1.0/1.3; no payload extraction."""
    with path.open('rb') as stream:
        header = stream.read(32)
        if len(header) != 32:
            raise ValueError(f'Truncated PSARC: {path}')
        magic, version, compression, toc, stride, count, block_size, flags = struct.unpack('>4sI4sIIIII', header)
        size = path.stat().st_size
        if (magic != b'PSAR' or version not in (0x10000, 0x10003) or compression != b'zlib'
                or stride != 30 or count < 2 or not 1 < block_size <= 1024 * 1024
                or block_size & (block_size - 1) or not 32 + count * stride <= toc <= min(size, 64 * 1024 * 1024)):
            raise ValueError(f'Unsupported PSARC header: {path}')
        table = stream.read(toc - 32)
        width = ((block_size - 1).bit_length() + 7) // 8
        lengths = table[count * stride:]
        if len(lengths) % width:
            raise ValueError(f'Invalid PSARC block table: {path}')
        sizes = [int.from_bytes(lengths[i:i + width], 'big') or block_size
                 for i in range(0, len(lengths), width)]

        def read_member(index, limit):
            row = table[index * stride:(index + 1) * stride]
            first = int.from_bytes(row[16:20], 'big')
            length = int.from_bytes(row[20:25], 'big')
            offset = int.from_bytes(row[25:30], 'big')
            blocks = (length + block_size - 1) // block_size
            if (length > limit or first + blocks > len(sizes) or offset < toc
                    or offset + sum(sizes[first:first + blocks]) > size):
                raise ValueError(f'Invalid PSARC metadata member: {path}')
            stream.seek(offset)
            out = bytearray()
            for i in range(blocks):
                stored = sizes[first + i]
                expected = min(block_size, length - i * block_size)
                raw = stream.read(stored)
                if stored == expected:
                    plain = raw
                else:
                    decoder = zlib.decompressobj()
                    plain = decoder.decompress(raw, expected + 1)
                    if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                        raise ValueError(f'Invalid compressed PSARC block: {path}')
                if len(plain) != expected:
                    raise ValueError(f'Invalid PSARC expanded length: {path}')
                out.extend(plain)
            return bytes(out)

        names = read_member(0, 16 * 1024 * 1024).decode('utf-8').splitlines()
        if len(names) != count - 1 or len(set(names)) != len(names):
            raise ValueError(f'Invalid PSARC member names: {path}')
        matches = [i for i, name in enumerate(names, 1) if name.lstrip('/') == 'patch.xml']
        if len(matches) != 1:
            raise ValueError(f'PSARC must contain one patch.xml: {path}')
        xml = ET.fromstring(read_member(matches[0], 65536))
        if xml.tag != 'patch' or len(xml.findall('requires')) != 1 or len(xml.findall('provides')) != 1:
            raise ValueError(f'Invalid patch dependency XML: {path}')
        return int(xml.findtext('requires')), int(xml.findtext('provides')), len(names)


def validate_patches(root, profile):
    for row in profile['patches']:
        path = checked_file(root, row['name'])
        check_identity(path, row)
        if patch_dependency(path) != (row['requires'], row['provides'], row['member_count']):
            raise ValueError(f'Patch dependency or member count mismatch: {path}')


def output_files(profile):
    rows = {}
    def add(name, row):
        relative(name)
        if name.casefold() in {p.casefold() for p in rows}:
            raise ValueError(f'Output path collision: {name}')
        rows[name] = {'size': row['size'], 'sha256': row['sha256']}
    for row in profile['disc_files']:
        add('disc/' + row['path'], row)
    prefix = f"dev_hdd0/game/{profile['title_id']}/"
    for row in profile['update_files']:
        add(prefix + row['path'], row)
    for row in profile['patches']:
        add(prefix + 'USRDIR/patches/' + row['name'], row)
    settings = next(r for r in profile['disc_files'] if r['path'] == profile['settings_source'])
    add(prefix + 'USRDIR/game.psarc.sdat', settings)
    return rows


def receipt_for(profile):
    return {'schema': 1, 'status': 'offline_verified_installation_layout',
            'title_id': profile['title_id'], 'app_version': profile['app_version'],
            'content_level': profile['content_level'], 'profile_sha256': profile_digest(profile),
            'runtime_verified': False, 'native_v212_supported': False,
            'files': output_files(profile)}


def verify_tree(root, profile):
    expected = output_files(profile)
    actual = set()
    if root.is_symlink():
        raise ValueError(f'Installation root is a symlink: {root}')
    for path in root.rglob('*'):
        if path.is_symlink():
            raise ValueError(f'Installation contains a symlink: {path}')
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    extra = actual - set(expected) - {RECEIPT}
    if extra:
        raise ValueError(f'Installation contains unexpected files: {sorted(extra)}')
    for name, row in expected.items():
        check_identity(checked_file(root, name), row)
    validate_patches(root / f"dev_hdd0/game/{profile['title_id']}/USRDIR/patches", profile)


def verify(root, profile):
    root = Path(root)
    validate_profile(profile)
    verify_tree(root, profile)
    receipt = json.loads(checked_file(root, RECEIPT).read_text())
    expected = receipt_for(profile)
    if receipt != expected:
        raise ValueError('Installation receipt differs from trusted profile or offline-only scope')
    return receipt


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'darwin':
        # Copy-on-write clones stay independent when either file changes.
        result = subprocess.run(['/bin/cp', '-c', str(source), str(target)], capture_output=True)
        if result.returncode == 0:
            return
    shutil.copyfile(source, target)


def extract_package(pkg, destination, extractor, profile):
    """Use the existing SDK helper, without importing or embedding its keys here."""
    if pkg.is_symlink() or not pkg.is_file():
        raise ValueError(f'PKG must be a regular non-symlink file: {pkg}')
    check_identity(pkg, profile['update_pkg'])
    if extractor.is_symlink() or not extractor.is_file():
        raise ValueError('Missing SDK PKG extractor; run tools/fetch_ps3recomp.py --role runtime')
    if digest(extractor) != profile['pkg_extractor_sha256']:
        raise ValueError('SDK PKG extractor differs from the verified source identity')
    try:
        subprocess.run([sys.executable, str(extractor), str(pkg), str(destination)],
                       cwd=destination.parent, check=True, capture_output=True, timeout=180)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        raise ValueError('Offline PKG extraction failed or timed out; output was not finalized') from exc
    validate_files(destination, profile['update_files'])
    return destination


def assemble(disc, patches, output, profile, *, update_tree=None, update_pkg=None, pkg_extractor=None):
    disc, patches, output = Path(disc).absolute(), Path(patches).absolute(), Path(output).absolute()
    validate_profile(profile)
    if (update_tree is None) == (update_pkg is None):
        raise ValueError('Provide exactly one of update_tree and update_pkg')
    sources = [disc, patches, Path(update_tree if update_tree is not None else update_pkg).absolute()]
    for source in sources:
        a, b = output.resolve(), source.resolve()
        if a == b or a in b.parents or b in a.parents:
            raise ValueError(f'Output and input paths overlap: {output}, {source}')
    if os.path.lexists(output):
        raise ValueError(f'Output already exists; choose a new directory: {output}')
    print('Verifying original disc and patch inputs...', file=sys.stderr)
    validate_files(disc, profile['disc_files'])
    validate_patches(patches, profile)
    if update_tree is not None:
        update_tree = Path(update_tree).absolute()
        validate_files(update_tree, profile['update_files'])
    else:
        update_pkg = Path(update_pkg).absolute()
        check_identity(checked_file(update_pkg.parent, update_pkg.name), profile['update_pkg'])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f'.{output.name}.staging-', dir=output.parent) as scratch:
        work = Path(scratch)
        if update_tree is None:
            print('Extracting verified update PKG offline...', file=sys.stderr)
            extractor = Path(pkg_extractor or ROOT / 'local/ps3recomp-runtime/tools/pkg_extract.py').absolute()
            update_tree = extract_package(update_pkg, work / 'pkg', extractor, profile)
        stage = work / 'install'
        stage.mkdir()
        print('Copying installation files...', file=sys.stderr)
        for row in profile['disc_files']:
            copy_file(checked_file(disc, row['path']), stage / 'disc' / row['path'])
        installed = stage / 'dev_hdd0/game' / profile['title_id']
        for row in profile['update_files']:
            copy_file(checked_file(update_tree, row['path']), installed / row['path'])
        copy_file(checked_file(disc, profile['settings_source']), installed / 'USRDIR/game.psarc.sdat')
        for row in profile['patches']:
            copy_file(checked_file(patches, row['name']), installed / 'USRDIR/patches' / row['name'])
        print('Verifying completed installation layout...', file=sys.stderr)
        receipt = receipt_for(profile)
        (stage / RECEIPT).write_text(json.dumps(receipt, indent=2) + '\n')
        verify(stage, profile)
        if os.path.lexists(output):
            raise ValueError(f'Output appeared during assembly; refusing to replace it: {output}')
        stage.rename(output)
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    build = commands.add_parser('assemble', help='create a new offline installation layout')
    build.add_argument('--disc', type=Path, required=True, help='root containing PS3_DISC.SFB and PS3_GAME')
    build.add_argument('--patches', type=Path, required=True, help='directory containing the four installed PSARCs')
    update = build.add_mutually_exclusive_group(required=True)
    update.add_argument('--update-pkg', type=Path, help='original official 02.12 update package')
    update.add_argument('--update-tree', type=Path, help='already extracted, byte-identical package files')
    build.add_argument('--pkg-extractor', type=Path, help='verified SDK pkg_extract.py (default: pinned local runtime)')
    build.add_argument('--output', type=Path, required=True, help='new directory; existing directories are refused')
    check = commands.add_parser('verify', help='verify a completed layout against the shipped identity profile')
    check.add_argument('--install', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        profile = json.loads(PROFILE.read_text())
        if args.command == 'assemble':
            result = assemble(args.disc, args.patches, args.output, profile,
                              update_tree=args.update_tree, update_pkg=args.update_pkg, pkg_extractor=args.pkg_extractor)
        else:
            result = verify(args.install, profile)
        print(json.dumps({k: result[k] for k in ('status', 'title_id', 'app_version', 'content_level',
                                               'runtime_verified', 'native_v212_supported')}, indent=2))
        print(f"Verified {len(result['files'])} installation files.")
        return 0
    except (ValueError, OSError, KeyError, ET.ParseError, zlib.error, UnicodeError) as exc:
        print(f'Preparation failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
