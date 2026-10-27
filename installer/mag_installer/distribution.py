"""Build the tooling-only source bundle from a fixed reviewed file registry."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import zipfile

from .model import InstallerError
from .platforms import publish_directory
from .safety import contained


SHIPPABLE_FILES = (
    'README.md', 'distribution-manifest.json',
    'mag_installer/__init__.py', 'mag_installer/__main__.py',
    'mag_installer/model.py', 'mag_installer/safety.py',
    'mag_installer/tools.py', 'mag_installer/platforms.py',
    'mag_installer/pipeline.py', 'mag_installer/distribution.py',
    'mag_installer/gui.py', 'mag_installer/ux.py', 'mag_installer/capacity.py',
    'mag_installer/native_recipe.py', 'mag_installer/native_build.py', 'docs/local-native-build.md',
    'mag_installer/player_build.py', 'mag_installer/managed_tools.py', 'mag_installer/native_package.py',
    'mag_installer/native_kit.py', 'scripts/prepare_native_kit.py', 'release/player-build.json',
    'mag_installer/native_package_windows.py', 'scripts/prepare_player_kit.py',
    'profiles/mag-unconfigured.json', 'profiles/synthetic-demonstration.json',
    'docs/design.md', 'docs/testing.md', 'docs/profile-format.md',
    'mag_installer/releasing.py', 'scripts/release_installer.py',
    'scripts/frozen_installer.py', 'scripts/publish_release.py',
    'requirements-release.txt', 'release/packager-lock.json',
    'release/notices/NOTICE.txt',
    'docs/release-packaging.md',
)


def bundle(root, destination):
    root, destination = Path(root).absolute(), Path(destination).absolute()
    if destination.exists() or destination.is_symlink():
        raise InstallerError('Distribution destination already exists')
    manifest_path = contained(root, 'distribution-manifest.json')
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise InstallerError('Distribution manifest is missing or invalid') from exc
    if (set(manifest) != {'schema_version', 'kind', 'release_ready', 'includes', 'excludes'}
            or manifest['schema_version'] != 1 or manifest['kind'] != 'asset-free-tooling'
            or type(manifest['release_ready']) is not bool
            or not isinstance(manifest['includes'], list)
            or len(manifest['includes']) != len(SHIPPABLE_FILES)
            or set(manifest['includes']) != set(SHIPPABLE_FILES)):
        raise InstallerError('Distribution manifest differs from the reviewed tooling-only registry')
    payload = {}
    checksums = {}
    for name in SHIPPABLE_FILES:
        path = contained(root, name)
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise InstallerError('Missing or unexpectedly large distribution source file')
        data = path.read_bytes()
        try: data.decode('utf-8')
        except UnicodeError as exc: raise InstallerError('Binary content cannot enter the source bundle') from exc
        if b'\x00' in data: raise InstallerError('Binary content cannot enter the source bundle')
        payload[name] = data
        checksums[name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    payload['BUNDLE-CHECKSUMS.json'] = (json.dumps({'schema_version': 1, 'kind': 'tooling-only', 'files': checksums}, indent=2, sort_keys=True) + '\n').encode()
    if not destination.parent.is_dir() or destination.parent.is_symlink():
        raise InstallerError('Distribution destination requires an existing ordinary parent')
    handle, name = tempfile.mkstemp(prefix='.tooling-bundle-', suffix='.zip', dir=destination.parent)
    import os
    os.close(handle)
    temporary = Path(name)
    try:
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(payload.items()):
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, data)
        publish_directory(temporary, destination)
        return destination
    except OSError as exc:
        raise InstallerError('Cannot create tooling distribution archive') from exc
    finally:
        temporary.unlink(missing_ok=True)
