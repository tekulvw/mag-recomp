"""Attach a verified release installer set to its existing GitHub release."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote


class PublicationError(ValueError):
    pass


def _digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _ordinary(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_nlink != 1:
        raise PublicationError('Release attachments must be ordinary files, without links')


def publish(directory, tag, repository, source_revision, run=subprocess.run):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,79}', tag):
        raise PublicationError('Release version must be a simple tag, at most 80 characters')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise PublicationError('Expected the current GitHub owner/repository')
    if not re.fullmatch(r'[0-9a-f]{40}', source_revision):
        raise PublicationError('Expected the checked-out source commit SHA')
    directory = Path(directory).absolute()
    if directory.is_symlink() or not directory.is_dir():
        raise PublicationError('Release directory must be an ordinary local directory')
    manifest_path = directory / 'RELEASE-ASSETS.json'
    _ordinary(manifest_path)
    if manifest_path.stat().st_size > 65536:
        raise PublicationError('Release attachment manifest is unexpectedly large')
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise PublicationError('Invalid release attachment manifest') from exc
    if (not isinstance(manifest, dict) or manifest.get('schema_version') != 1
            or manifest.get('version') != tag
            or manifest.get('source_revision') != source_revision
            or manifest.get('kind') != 'release'):
        raise PublicationError('Only release assets bound to this tag and commit may be uploaded')
    archives = {
        f'mag-installer-{tag}-windows-x86_64.zip',
        f'mag-installer-{tag}-macos-arm64.zip',
    }
    expected = archives | {name + '.sha256' for name in archives} | {'SHA256SUMS.txt'}
    records = manifest.get('files')
    if (not isinstance(records, list) or len(records) != len(expected)
            or any(not isinstance(record, dict) for record in records)
            or any(not isinstance(record.get('name'), str) for record in records)
            or {record.get('name') for record in records} != expected):
        raise PublicationError('Release manifest must contain the exact per-platform attachment set')
    if {path.name for path in directory.iterdir()} != expected | {'RELEASE-ASSETS.json'}:
        raise PublicationError('Unexpected file or directory in release attachments')
    for record in records:
        path = directory / record['name']
        _ordinary(path)
        if (type(record.get('size')) is not int or record['size'] < 1
                or path.stat().st_size != record['size']
                or not isinstance(record.get('sha256'), str)
                or not re.fullmatch(r'[0-9a-f]{64}', record['sha256'])
                or _digest(path) != record['sha256']):
            raise PublicationError('Release attachment bytes differ from the verified set')

    # Revalidate archive contents at the publication boundary, after retrieval.
    validator = Path(__file__).with_name('release_installer.py')
    result = run([sys.executable, str(validator), 'verify-set', '--directory', str(directory),
                  '--version', tag, '--source-revision', source_revision],
                 check=False, timeout=120)
    if result.returncode:
        raise PublicationError('Release archive verification refused publication')

    endpoint = f'repos/{repository}/releases/tags/{quote(tag, safe="")}'
    result = run(['gh', 'api', '--method', 'GET', endpoint], check=False,
                 capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise PublicationError('Cannot inspect the existing GitHub release')
    try:
        release = json.loads(result.stdout)
    except (ValueError, TypeError) as exc:
        raise PublicationError('GitHub returned invalid release metadata') from exc
    if (not isinstance(release, dict) or release.get('tag_name') != tag
            or release.get('draft') is not False
            or type(release.get('immutable')) is not bool):
        raise PublicationError('Release identity, publication state or mutability is unknown')
    if release['immutable']:
        raise PublicationError('Release is immutable; attach installers to a draft before publishing')
    existing = release.get('assets')
    if not isinstance(existing, list) or any(not isinstance(item, dict) for item in existing):
        raise PublicationError('Cannot inspect existing release assets')
    upload_names = expected | {'RELEASE-ASSETS.json'}
    if any(item.get('name') in upload_names for item in existing):
        raise PublicationError('Installer assets already exist; refusing to replace or delete them')
    paths = [str(directory / name) for name in sorted(upload_names)]
    result = run(['gh', 'release', 'upload', tag, '--repo', repository, *paths],
                 check=False, timeout=300)
    if result.returncode:
        raise PublicationError('GitHub rejected upload; existing assets were not replaced or deleted')
    return sorted(upload_names)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--source-revision', required=True)
    args = parser.parse_args(argv)
    try:
        files = publish(args.directory, args.tag, os.environ.get('GH_REPO', ''), args.source_revision)
    except (PublicationError, OSError, subprocess.SubprocessError) as exc:
        print(f'Installer publication refused: {exc}', file=sys.stderr)
        return 2
    print(json.dumps({'event': 'installer-assets-uploaded', 'files': files}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
