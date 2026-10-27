"""Native launcher packaging and fail-closed release-set validation.

No network/install/publication action is performed here. Build is an explicit
separate operation; source-contract tests never invoke a native build.
"""
from __future__ import annotations

from contextlib import contextmanager
import gzip
import hashlib
import importlib.metadata
import io
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import sysconfig
import tarfile
import tempfile
import unicodedata
import zipfile

from .distribution import SHIPPABLE_FILES, bundle
from .model import InstallerError, Profile, host_key, relative_path
from .platforms import publish_directory
from .safety import contained

TARGETS = ('windows-x86_64', 'macos-arm64')
REPOSITORY = 'tekulvw/mag-recomp'
KINDS = ('preview', 'release')
MAX_ARCHIVE = 256 * 1024 * 1024
MAX_MEMBERS = 2048


def _json(data):
    return (json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + '\n').encode()


def _load(path, limit=1024 * 1024):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
        raise InstallerError('Missing, linked or oversized release metadata')
    try:
        return json.loads(path.read_bytes())
    except (ValueError, UnicodeError) as exc:
        raise InstallerError('Invalid release metadata JSON') from exc


def _hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _ordinary(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_nlink != 1:
        raise InstallerError('Release files must be ordinary files without links')


def _notice_text(data):
    if not data or b'\0' in data:
        raise InstallerError('License notices must be nonempty UTF-8 text')
    try: data.decode('utf-8')
    except UnicodeError as exc:
        raise InstallerError('License notices must be nonempty UTF-8 text') from exc


def _version(version):
    if not isinstance(version, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,79}', version):
        raise InstallerError('Version must be a simple path-safe tag, at most 80 characters')
    return version


def _revision(value, preview=False):
    if preview and value == 'unknown': return value
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{40}', value):
        raise InstallerError('A full lowercase source commit SHA is required')
    return value


def artifact_name(version, target, preview):
    _version(version)
    if target not in TARGETS: raise InstallerError('Target is outside the exact initial release set')
    extension = '.tar.gz' if target.startswith('linux-') else '.zip'
    return f'mag-installer-{version}-{target}{"-preview" if preview else ""}{extension}'


def _registry(root):
    result = {}
    for name in SHIPPABLE_FILES:
        path = contained(root, name)
        _ordinary(path)
        if path.stat().st_size > 1024 * 1024:
            raise InstallerError('Source registry member exceeds its size limit')
        data = path.read_bytes()
        try: data.decode('utf-8')
        except UnicodeError as exc: raise InstallerError('Source registry must contain UTF-8 text only') from exc
        if b'\0' in data: raise InstallerError('Binary input cannot enter source staging')
        # Git source text has one identity across native checkout line endings.
        result[name] = data.replace(b'\r\n', b'\n')
    return result


def _source_digest(root):
    records = {name: hashlib.sha256(data).hexdigest() for name, data in _registry(root).items()}
    return hashlib.sha256(_json(records)).hexdigest()


KIT = 'release/native-kit.tar.gz'
KIT_MANIFEST = 'release/player-build.json'


def _kit_record(manifest, kit, target=None):
    """Check a kit manifest and its kit bytes against each other; return the manifest."""
    try: record = json.loads(manifest)
    except (ValueError, UnicodeError) as exc: raise InstallerError('Invalid source kit manifest') from exc
    entry = record.get('kit') if isinstance(record, dict) else None
    if (not isinstance(entry, dict) or record.get('schema_version') != 1 or entry.get('embedded') != KIT
            or record.get('distribution') != 'source-kit'):
        raise InstallerError('Unrecognized embedded native source kit')
    if target is not None and record.get('host') != target:
        raise InstallerError('Embedded source kit is for another host')
    if len(kit) != entry.get('size') or len(kit) > 64 * 1024 * 1024:
        raise InstallerError('Native source kit size differs from its pinned identity')
    if hashlib.sha256(kit).hexdigest() != entry.get('sha256'):
        raise InstallerError('Native source kit hash differs from its pinned identity')
    return record


def _extra_resources(root, target=None):
    """The per-build source kit and its host manifest, when this stage has one.

    They are not part of the fixed source registry: CI prepares one kit per host
    from the release's source, so each installer verifies its own kit instead.
    The kit contains code, never the player's inputs or anything generated from them.
    """
    manifest = Path(root) / KIT_MANIFEST
    if not manifest.exists():
        return {}
    _ordinary(manifest)
    record = _load(manifest)
    if not (record.get('kit') or {}).get('embedded'):
        return {}  # the source checkout's placeholder: no kit
    path = contained(root, KIT)
    _ordinary(path)
    data = path.read_bytes()
    _kit_record(manifest.read_bytes(), data, target)
    return {KIT_MANIFEST: manifest.read_bytes(), KIT: data}


def check(version, kind, repository):
    """Release policy: CI builds from this repository's source, at a release-please tag.

    Installers carry only this repository's tooling and source kit plus the
    pinned MIT runtime; never anything built or derived from game files.
    """
    if kind not in KINDS: raise InstallerError('Unknown release kind')
    if kind == 'release':
        if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+', version or ''):
            raise InstallerError('Release installers need a release-please version tag such as v0.2.0')
        if repository != REPOSITORY:
            raise InstallerError('Release installers are built only by ' + REPOSITORY + ' CI')
    return {'schema_version': 2, 'kind': kind, 'targets': list(TARGETS)}


def stage(root, output):
    """Copy only fixed registry entries. Never scan source directories for inputs."""
    root, output = Path(root).absolute(), Path(output).absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise InstallerError('Clean stage requires a new path under an existing parent')
    # Reuse source-bundle manifest validation before constructing a clean tree.
    with tempfile.TemporaryDirectory(prefix='.source-stage-', dir=output.parent) as temporary:
        scratch = Path(temporary)
        bundle(root, scratch / 'source.zip')
        clean = scratch / 'clean'
        clean.mkdir()
        for name, data in {**_registry(root), **_extra_resources(root)}.items():
            path = contained(clean, name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o644)
        publish_directory(clean, output)
    return output


def _atomic_file(path, data):
    if path.exists() or path.is_symlink():
        raise InstallerError('Refusing to overwrite release output')
    handle, name = tempfile.mkstemp(prefix='.release-', dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(handle, 'wb') as stream: stream.write(data)
        publish_directory(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _same_or_new(path, data):
    if path.exists() or path.is_symlink():
        _ordinary(path)
        if path.read_bytes() != data: raise InstallerError('Existing generated release metadata differs; refusing overwrite')
    else:
        _atomic_file(path, data)


MAC_APP = 'MAG Installer.app'
MAC_PLIST = MAC_APP + '/Contents/Info.plist'


def _binary_path(target):
    if target.startswith('macos-'):
        return MAC_APP + '/Contents/MacOS/mag-installer'
    return 'mag-installer.exe' if target.startswith('windows-') else 'mag-installer'


def _app_plist(version):
    preview = version.startswith('preview-')
    display_name = 'MAG Installer Preview' if preview else 'MAG Installer'
    identifier = 'net.tekulvw.mag-installer.preview' if preview else 'net.tekulvw.mag-installer'
    return plistlib.dumps({'CFBundleExecutable': 'mag-installer',
                          'CFBundleIdentifier': identifier,
                          'CFBundleName': display_name,
                          'CFBundleDisplayName': display_name,
                          'CFBundlePackageType': 'APPL',
                          'CFBundleVersion': version,
                          'CFBundleShortVersionString': version,
                          'NSHighResolutionCapable': True}, sort_keys=True)


def _prepare_launcher(source, payload, target, version):
    """Wrap the same CLI-capable launcher in a Finder-launchable macOS app."""
    binary = payload / _binary_path(target)
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, binary)
    binary.chmod(0o755)
    if target.startswith('macos-'):
        (payload / MAC_PLIST).write_bytes(_app_plist(version))
    return binary


def package_payload(payload, output, version, target, kind, source_revision, root, smoke_verified=False):
    """Archive an installer-owned native payload; source resources are allowlisted.

    This does not certify a binary. Native build passes smoke_verified only after
    executing the packaged launcher; synthetic archive tests leave it false.
    """
    preview = kind == 'preview'
    check(version, kind, os.environ.get('GITHUB_REPOSITORY', 'unknown') if not preview else REPOSITORY)
    _revision(source_revision, preview)
    name = artifact_name(version, target, preview)
    if not preview and not smoke_verified: raise InstallerError('Release requires native frozen-launcher smoke verification')
    payload, output, root = Path(payload), Path(output), Path(root)
    if payload.is_symlink() or not payload.is_dir() or output.is_symlink():
        raise InstallerError('Payload/output directories must be ordinary local directories')
    output.mkdir(parents=True, exist_ok=True)
    destination = output / name
    if destination.exists() or (output / (name + '.sha256')).exists():
        raise InstallerError('Release archive or checksum already exists')
    binary = _binary_path(target)
    files = {}
    for path in payload.rglob('*'):
        if path.is_symlink(): raise InstallerError('Payload links are unsupported')
        if path.is_dir(): continue
        _ordinary(path)
        relative = path.relative_to(payload).as_posix()
        relative_path(relative)
        if relative != binary and not (target.startswith('macos-') and relative == MAC_PLIST) and not relative.startswith('licenses/'):
            raise InstallerError('Unexpected file in the installer-owned payload')
        if relative != binary and path.stat().st_size > 2 * 1024 * 1024:
            raise InstallerError('Oversized license notice')
        files[relative] = path.read_bytes()
        if relative.startswith('licenses/'): _notice_text(files[relative])
    if binary not in files or not files[binary]: raise InstallerError('Native launcher is absent')
    if target.startswith('macos-') and files.get(MAC_PLIST) != _app_plist(version):
        raise InstallerError('macOS application metadata differs from the required launcher identity')
    for required in ('licenses/PYTHON-LICENSE.txt', 'licenses/PYINSTALLER-COPYING.txt'):
        if required not in files or not files[required]: raise InstallerError('Required Python/PyInstaller redistribution notice is absent')
    registry = _registry(root)
    files.update({'resources/' + key: data for key, data in registry.items()})
    files.update({'resources/' + key: data for key, data in _extra_resources(root, target).items()})
    files['NOTICE.txt'] = registry['release/notices/NOTICE.txt']
    lock = _load(root / 'release/packager-lock.json')
    # The tooling revision differs from the source revision only when installers
    # are rebuilt for an existing release with later tooling.
    tooling_revision = _revision(os.environ.get('INSTALLER_TOOLING_REVISION', source_revision), preview)
    provenance = {'schema_version': 2, 'version': version, 'target': target, 'kind': kind,
                  'source_revision': source_revision, 'tooling_revision': tooling_revision,
                  'source_repository': os.environ.get('GITHUB_REPOSITORY', 'unknown'),
                  'source_registry_sha256': _source_digest(root), 'source_encoding': 'UTF-8-LF',
                  'pyinstaller_version': lock['pyinstaller_version'],
                  'python_version': platform.python_version(), 'smoke_verified': bool(smoke_verified),
                  'release_ready': kind == 'release'}
    files['PROVENANCE.json'] = _json(provenance)
    records = {key: {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                     'mode': 0o755 if key == binary else 0o644} for key, data in files.items()}
    files['INVENTORY.json'] = _json({'schema_version': 1, 'files': records})
    prefix = name.removesuffix('.tar.gz').removesuffix('.zip')
    handle, temporary_name = tempfile.mkstemp(prefix='.native-archive-', dir=output)
    os.close(handle)
    temporary = Path(temporary_name)
    try:
        if target.startswith('linux-'):
            with temporary.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', mtime=0, filename='') as compressed:
                with tarfile.open(fileobj=compressed, mode='w|', format=tarfile.USTAR_FORMAT) as archive:
                    for relative, data in sorted(files.items()):
                        info = tarfile.TarInfo(prefix + '/' + relative)
                        info.size, info.mode, info.mtime = len(data), 0o755 if relative == binary else 0o644, 0
                        archive.addfile(info, io.BytesIO(data))
        else:
            with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
                for relative, data in sorted(files.items()):
                    info = zipfile.ZipInfo(prefix + '/' + relative, (2026, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | (0o755 if relative == binary else 0o644)) << 16
                    archive.writestr(info, data)
        publish_directory(temporary, destination)
        _atomic_file(output / (name + '.sha256'), f'{_hash(destination)}  {name}\n'.encode())
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def _read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size: raise InstallerError('Truncated release archive')
    return data


def _octal(field):
    field = field.rstrip(b'\0 ').lstrip(b' ')
    if not field: return 0
    if any(byte not in b'01234567' for byte in field): raise InstallerError('Nonportable TAR numeric field')
    return int(field, 8)


@contextmanager
def _archive_entries(path):
    if path.name.endswith('.zip'):
        with zipfile.ZipFile(path) as archive:
            if len(archive.infolist()) > MAX_MEMBERS: raise InstallerError('Too many release archive members')
            def entries():
                for info in archive.infolist():
                    if info.is_dir() or info.flag_bits & 1 or stat.S_IFMT(info.external_attr >> 16) != stat.S_IFREG:
                        raise InstallerError('Release ZIP may contain only ordinary unencrypted files')
                    with archive.open(info) as stream:
                        yield info.filename, info.file_size, (info.external_attr >> 16) & 0o7777, stream
            yield entries()
    elif path.name.endswith('.tar.gz'):
        # Only the writer's plain USTAR regular files are accepted. Parse fixed
        # headers directly, refusing PAX/GNU extension allocations and all links.
        with gzip.open(path, 'rb') as stream:
            def entries():
                for _ in range(MAX_MEMBERS + 1):
                    header = _read_exact(stream, 512)
                    if not any(header):
                        trailer = stream.read(10241)
                        if len(trailer) > 10240 or any(trailer): raise InstallerError('Unexpected TAR trailer')
                        return
                    if header[156:157] not in (b'0', b'\0') or header[257:263] != b'ustar\0':
                        raise InstallerError('Only plain USTAR regular files are supported')
                    if _octal(header[148:156]) != sum(header[:148]) + 8 * 32 + sum(header[156:]):
                        raise InstallerError('TAR header checksum mismatch')
                    try:
                        name = header[:100].split(b'\0', 1)[0].decode('utf-8')
                        prefix = header[345:500].split(b'\0', 1)[0].decode('utf-8')
                    except UnicodeError as exc: raise InstallerError('Invalid archive path encoding') from exc
                    size = _octal(header[124:136])
                    if size > MAX_ARCHIVE: raise InstallerError('Release archive member is too large')
                    # Bounded per-member read; total is checked by the consumer.
                    member = io.BytesIO(_read_exact(stream, size))
                    yield ((prefix + '/') if prefix else '') + name, size, _octal(header[100:108]), member
                    if any(_read_exact(stream, (-size) % 512)): raise InstallerError('Nonzero TAR file padding')
                raise InstallerError('Too many release archive members')
            yield entries()
    else:
        raise InstallerError('Unsupported release archive extension')


def verify_artifact(artifact, root):
    artifact, root = Path(artifact), Path(root)
    _ordinary(artifact)
    if artifact.stat().st_size > MAX_ARCHIVE: raise InstallerError('Release archive exceeds its size limit')
    entries, data = {}, {}
    folded = set()
    total = 0
    try:
        with _archive_entries(artifact) as members:
            for name, size, mode, stream in members:
                relative_path(name)
                canonical = unicodedata.normalize('NFC', name).casefold()
                if canonical in folded: raise InstallerError('Duplicate or colliding release archive members')
                folded.add(canonical)
                total += size
                if size < 0 or total > MAX_ARCHIVE: raise InstallerError('Expanded release archive exceeds its limit')
                digest, count, retained = hashlib.sha256(), 0, bytearray()
                keep = not name.endswith(('/mag-installer', '/mag-installer.exe'))
                while block := stream.read(256 * 1024):
                    count += len(block)
                    if count > size: raise InstallerError('Member exceeds its declared size')
                    digest.update(block)
                    if keep:
                        limit = 64 * 1024 * 1024 if name.endswith('/resources/release/native-kit.tar.gz') else 2 * 1024 * 1024
                        if count > limit: raise InstallerError('Release metadata/source member exceeds its limit')
                        retained.extend(block)
                if count != size: raise InstallerError('Truncated release member')
                entries[name] = {'size': size, 'sha256': digest.hexdigest(), 'mode': mode}
                if keep: data[name] = bytes(retained)
    except (OSError, ValueError, EOFError, zipfile.BadZipFile) as exc:
        raise InstallerError('Release archive could not be verified') from exc
    provenance_names = [name for name in data if name.endswith('/PROVENANCE.json')]
    if len(provenance_names) != 1: raise InstallerError('Exactly one release provenance record is required')
    prefix = provenance_names[0].removesuffix('/PROVENANCE.json')
    try:
        provenance = json.loads(data[prefix + '/PROVENANCE.json'])
        inventory = json.loads(data[prefix + '/INVENTORY.json'])
    except (KeyError, ValueError, UnicodeError) as exc: raise InstallerError('Invalid release provenance/inventory') from exc
    fields = {'schema_version', 'version', 'target', 'kind', 'source_revision', 'tooling_revision', 'source_repository',
              'source_registry_sha256', 'source_encoding', 'pyinstaller_version', 'python_version', 'smoke_verified', 'release_ready'}
    if not isinstance(provenance, dict) or set(provenance) != fields or provenance['schema_version'] != 2:
        raise InstallerError('Unsupported release provenance')
    preview = provenance['kind'] == 'preview'
    if provenance['kind'] not in KINDS: raise InstallerError('Unknown release kind')
    expected_name = artifact_name(provenance['version'], provenance['target'], preview)
    if artifact.name != expected_name or prefix != expected_name.removesuffix('.tar.gz').removesuffix('.zip'):
        raise InstallerError('Release filename/root differs from provenance')
    _revision(provenance['source_revision'], preview)
    _revision(provenance['tooling_revision'], preview)
    lock = _load(root / 'release/packager-lock.json')
    if (provenance['source_registry_sha256'] != _source_digest(root)
            or provenance['source_encoding'] != 'UTF-8-LF'
            or provenance['pyinstaller_version'] != lock['pyinstaller_version']
            or type(provenance['smoke_verified']) is not bool
            or type(provenance['release_ready']) is not bool
            or not isinstance(provenance['python_version'], str)
            or not re.fullmatch(r'3\.[0-9]+\.[0-9]+', provenance['python_version'])):
        raise InstallerError('Release source/tool provenance differs from the checked-out source')
    if preview:
        if provenance['release_ready']: raise InstallerError('Preview cannot claim release status')
    else:
        check(provenance['version'], 'release', provenance['source_repository'])
        if not provenance['release_ready'] or not provenance['smoke_verified']:
            raise InstallerError('Release requires frozen smoke results')
    binary = _binary_path(provenance['target'])
    if not isinstance(inventory, dict) or set(inventory) != {'schema_version', 'files'} or inventory['schema_version'] != 1 or not isinstance(inventory['files'], dict):
        raise InstallerError('Invalid release inventory')
    actual = {name.removeprefix(prefix + '/'): record for name, record in entries.items() if name != prefix + '/INVENTORY.json'}
    if any(not name.startswith(prefix + '/') for name in entries) or actual != inventory['files']:
        raise InstallerError('Release members, hashes or modes differ from inventory')
    allowed = {binary, 'NOTICE.txt', 'PROVENANCE.json'} | {'resources/' + name for name in SHIPPABLE_FILES}
    kit_names = {'resources/' + KIT, 'resources/' + KIT_MANIFEST}
    if kit_names & set(actual):
        if not kit_names <= set(actual): raise InstallerError('Source kit and its manifest must travel together')
        _kit_record(data[prefix + '/resources/' + KIT_MANIFEST], data[prefix + '/resources/' + KIT], provenance['target'])
        allowed |= kit_names
    elif not preview:
        raise InstallerError('A release installer must carry its source kit')
    if provenance['target'].startswith('macos-'):
        allowed.add(MAC_PLIST)
        if data.get(prefix + '/' + MAC_PLIST) != _app_plist(provenance['version']):
            raise InstallerError('macOS application metadata differs from the required launcher identity')
    if not allowed.issubset(actual) or any(name not in allowed and not name.startswith('licenses/') for name in actual):
        raise InstallerError('Unexpected or missing release payload member')
    if not actual[binary]['size']: raise InstallerError('Native launcher must not be empty')
    for name, record in actual.items():
        if name.startswith('licenses/'): _notice_text(data[prefix + '/' + name])
        if record['mode'] != (0o755 if name == binary else 0o644): raise InstallerError('Release executable/data modes are incorrect')
    if entries[prefix + '/INVENTORY.json']['mode'] != 0o644:
        raise InstallerError('Inventory mode is incorrect')
    for name in ('licenses/PYTHON-LICENSE.txt', 'licenses/PYINSTALLER-COPYING.txt'):
        if name not in actual or not actual[name]['size']: raise InstallerError('Required license notice is missing')
    for name, expected in _registry(root).items():
        if data.get(prefix + '/resources/' + name) != expected: raise InstallerError('Bundled source resource differs from the fixed registry')
    if data[prefix + '/NOTICE.txt'] != _registry(root)['release/notices/NOTICE.txt']:
        raise InstallerError('Release notice differs from source')
    return provenance


def verify_set(directory, root, version=None, source_revision=None, preview=False):
    directory, root = Path(directory), Path(root)
    if directory.is_symlink() or not directory.is_dir(): raise InstallerError('Release set directory must be ordinary')
    if not preview:
        _version(version)
        _revision(source_revision)
        check(version, 'release', REPOSITORY)
    children = list(directory.iterdir())
    for path in children: _ordinary(path)
    archives = [path for path in children if path.name.endswith(('.zip', '.tar.gz'))]
    if len(archives) != len(TARGETS): raise InstallerError('Release set must contain exactly one archive per supported target')
    reports = [verify_artifact(path, root) for path in archives]
    inferred_version, inferred_revision = reports[0]['version'], reports[0]['source_revision']
    version = version if version is not None else inferred_version
    source_revision = source_revision if source_revision is not None else inferred_revision
    _version(version)
    _revision(source_revision, preview)
    kind = 'preview' if preview else 'release'
    if ({record['target'] for record in reports} != set(TARGETS)
            or any(record['version'] != version or record['source_revision'] != source_revision or record['kind'] != kind for record in reports)):
        raise InstallerError('Release set target/version/source/kind binding differs')
    names = {artifact_name(version, target, preview) for target in TARGETS}
    base = names | {name + '.sha256' for name in names}
    optional = {'SHA256SUMS.txt', 'RELEASE-ASSETS.json'}
    if {path.name for path in children} - optional != base:
        raise InstallerError('Unexpected, missing or nested file in the exact release set')
    sums = []
    for name in sorted(names):
        line = f'{_hash(directory / name)}  {name}\n'
        if (directory / (name + '.sha256')).read_bytes() != line.encode():
            raise InstallerError('Archive companion checksum differs')
        sums.append(line)
    sums_data = ''.join(sums).encode()
    sums_path = directory / 'SHA256SUMS.txt'
    if sums_path.exists() and sums_path.read_bytes() != sums_data:
        raise InstallerError('Existing checksum set differs')
    records = [{'name': name, 'size': (directory / name).stat().st_size, 'sha256': _hash(directory / name)} for name in sorted(base)]
    records.append({'name': 'SHA256SUMS.txt', 'size': len(sums_data), 'sha256': hashlib.sha256(sums_data).hexdigest()})
    manifest = {'schema_version': 1, 'version': version, 'source_revision': source_revision, 'kind': kind,
                'files': sorted(records, key=lambda record: record['name'])}
    manifest_path = directory / 'RELEASE-ASSETS.json'
    if manifest_path.exists() and manifest_path.read_bytes() != _json(manifest):
        raise InstallerError('Existing release attachment manifest differs')
    _same_or_new(sums_path, sums_data)
    _same_or_new(manifest_path, _json(manifest))
    return manifest


def _packager_versions(root, target):
    lock = _load(Path(root) / 'release/packager-lock.json')
    for name, record in lock['packages'].items():
        if name == 'macholib' and not target.startswith('macos-'): continue
        if name in ('pefile', 'pywin32-ctypes') and not target.startswith('windows-'): continue
        try: version = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError as exc:
            raise InstallerError('Install the hash-locked build-only requirements in an isolated environment') from exc
        if version != record['version']: raise InstallerError('Installed packager dependency differs from its exact pin')
    return lock


def _licenses(payload, lock, target):
    destination = payload / 'licenses'
    destination.mkdir()
    candidates = [Path(sys.base_prefix) / 'LICENSE.txt', Path(sys.base_prefix) / 'LICENSE',
                  Path(sysconfig.get_path('stdlib')) / 'LICENSE.txt']
    python_license = next((path for path in candidates if path.is_file()), None)
    if python_license is None: raise InstallerError('Installed Python license was not found; release build is blocked')
    shutil.copyfile(python_license, destination / 'PYTHON-LICENSE.txt')
    copied_pyinstaller = False
    for name in lock['packages']:
        if name == 'macholib' and not target.startswith('macos-'): continue
        if name in ('pefile', 'pywin32-ctypes') and not target.startswith('windows-'): continue
        distribution = importlib.metadata.distribution(name)
        for entry in distribution.files or ():
            if not any(token in entry.name.lower() for token in ('license', 'copying', 'copyright')): continue
            path = Path(distribution.locate_file(entry))
            if not path.is_file() or path.stat().st_size > 2 * 1024 * 1024: continue
            notice = destination / name / str(entry).replace('/', '_').replace('\\', '_')
            notice.parent.mkdir(exist_ok=True)
            shutil.copyfile(path, notice)
            if name == 'pyinstaller' and 'copying' in entry.name.lower():
                shutil.copyfile(path, destination / 'PYINSTALLER-COPYING.txt')
                copied_pyinstaller = True
    if not copied_pyinstaller: raise InstallerError('Pinned PyInstaller COPYING notice was not found')


def _smoke_command(command, **kwargs):
    try:
        return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        def bounded(value):
            if isinstance(value, bytes):
                value = value.decode('utf-8', errors='replace')
            return (value or '')[-6000:]
        status = f'exit {exc.returncode}' if isinstance(exc, subprocess.CalledProcessError) else 'timeout'
        raise InstallerError(f'Frozen smoke command {command[1:]} failed ({status}).\nstdout:\n{bounded(exc.stdout)}\nstderr:\n{bounded(exc.stderr)}') from exc


def _smoke(binary, scratch):
    """Native executable only; no Python in PATH or game input is used."""
    no_programs = scratch / 'empty-program-path'
    no_programs.mkdir()
    env = {'PATH': str(no_programs), 'HOME': str(scratch), 'USERPROFILE': str(scratch),
           'TEMP': str(scratch), 'TMP': str(scratch), 'TMPDIR': str(scratch)}
    if 'SystemRoot' in os.environ: env['SystemRoot'] = os.environ['SystemRoot']
    _smoke_command([str(binary), '--help'], cwd=scratch, env=env, timeout=30)
    status = _smoke_command([str(binary), 'status'], cwd=scratch, env=env, timeout=30)
    report = json.loads(status.stdout)
    if report.get('kind') != 'production': raise InstallerError('Frozen status did not load its production profile')
    source = scratch / 'owned synthetic input'
    source.mkdir()
    (source / 'SYNTHETIC-INPUT.txt').write_bytes(b'This is synthetic installer input.\n')
    output = scratch / 'synthetic output'
    result = _smoke_command([str(binary), 'install', '--synthetic', '--source', str(source), '--output', str(output), '--cache', str(scratch / 'cache')],
                            cwd=scratch, env=env, timeout=60)
    if 'synthetic-complete' not in result.stdout or not (output / 'SYNTHETIC-RESULT.txt').is_file():
        raise InstallerError('Frozen synthetic demonstration did not complete')


def _check_tk_runtime():
    """Require a usable GUI binding without opening a build-host display."""
    try:
        subprocess.run([sys.executable, '-c',
                        'import tkinter; import _tkinter; t = tkinter.Tcl(); t.eval("info patchlevel")'],
                       check=True, capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError('Native release builds require Python with a working Tcl/Tk runtime; install a Tk-enabled build Python before packaging') from exc


def build(root, output, version, preview=False):
    """Explicit native build operation. Never downloads or installs dependencies."""
    root, output = Path(root).absolute(), Path(output).absolute()
    target = host_key()
    artifact_name(version, target, preview)
    if os.environ.get('EXPECTED_TARGET', target) != target:
        raise InstallerError('EXPECTED_TARGET differs from the native host; cross-compiling is forbidden')
    if platform.python_implementation() != 'CPython' or sys.version_info < (3, 11):
        raise InstallerError('Build requires CPython 3.11 or newer')
    check(version, 'preview' if preview else 'release', os.environ.get('GITHUB_REPOSITORY', 'unknown'))
    revision = _revision(os.environ.get('INSTALLER_SOURCE_REVISION', 'unknown'), preview)
    _check_tk_runtime()
    lock = _packager_versions(root, target)
    if output.is_symlink(): raise InstallerError('Build output cannot be a symlink')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.installer-build-', dir=output.parent) as temporary:
        scratch = Path(temporary)
        clean = stage(root, scratch / 'source')
        for name in ('home', 'tmp', 'spec', 'work', 'dist', 'smoke'): (scratch / name).mkdir()
        environment = {'PATH': str(Path(sys.executable).parent) + os.pathsep + os.defpath,
                       'HOME': str(scratch / 'home'), 'USERPROFILE': str(scratch / 'home'),
                       'TEMP': str(scratch / 'tmp'), 'TMP': str(scratch / 'tmp'), 'TMPDIR': str(scratch / 'tmp'),
                       'PYTHONNOUSERSITE': '1', 'PYINSTALLER_CONFIG_DIR': str(scratch / 'config')}
        if 'SystemRoot' in os.environ: environment['SystemRoot'] = os.environ['SystemRoot']
        command = [sys.executable, '-m', 'PyInstaller', '--onefile', '--console', '--noupx', '--clean', '--noconfirm',
                   '--hidden-import', 'tkinter', '--name', 'mag-installer', '--paths', str(clean), '--add-data', f'{clean}:resources',
                   '--specpath', str(scratch / 'spec'), '--workpath', str(scratch / 'work'),
                   '--distpath', str(scratch / 'dist'), str(clean / 'scripts/frozen_installer.py')]
        subprocess.run(command, cwd=clean, env=environment, check=True, timeout=900)
        payload = scratch / 'payload'
        payload.mkdir()
        binary_name = 'mag-installer.exe' if target.startswith('windows-') else 'mag-installer'
        binary = _prepare_launcher(scratch / 'dist' / binary_name, payload, target, version)
        _licenses(payload, lock, target)
        _smoke(binary, scratch / 'smoke')
        artifact = package_payload(payload, output, version, target, 'preview' if preview else 'release', revision, clean, smoke_verified=True)
        verify_artifact(artifact, clean)
        return artifact
