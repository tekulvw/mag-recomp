#!/usr/bin/env python3
"""Embed a freshly prepared source kit for one player host.

Run from CI in a clean copy of this installer (--root), against a repository
checkout on which tools/fetch_ps3recomp.py has run (--source). Writes
release/native-kit.tar.gz and release/player-build.json under --root. The kit
holds this repository's tooling source and the four pinned ps3recomp checkouts;
it never reads game inputs or builds MAG. The kit is host-neutral; the manifest
names the single host whose installer will carry it.
"""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from mag_installer.model import InstallerError
from mag_installer.native_kit import pinned_sdks, prepare
from mag_installer.player_build import PLAYER_HOSTS


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--host', choices=sorted(PLAYER_HOSTS), required=True)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    archive = root / 'release/native-kit.tar.gz'
    manifest = root / 'release/player-build.json'
    try:
        if archive.exists():
            raise InstallerError('release/native-kit.tar.gz already exists; use a clean stage')
        descriptor = prepare(args.source, pinned_sdks(args.source), archive)
        record = {'schema_version': 1, 'host': args.host, 'distribution': 'source-kit',
                  'kit': {key: descriptor[key] for key in ('sha256', 'size', 'format', 'max_expanded_size', 'max_entries')}
                         | {'embedded': 'release/native-kit.tar.gz'},
                  'native_build_verified': False}
        manifest.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'kit': archive.name, 'manifest': record}, indent=2))
        return 0
    except (InstallerError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
