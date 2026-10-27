"""Build/verify native asset-free installer launchers; never publish or download."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from mag_installer.model import InstallerError
from mag_installer import releasing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('check')
    command.add_argument('--version', required=True)
    command.add_argument('--preview', action='store_true')
    command = commands.add_parser('stage')
    command.add_argument('--output', type=Path, required=True)
    command = commands.add_parser('build')
    command.add_argument('--output', type=Path, required=True)
    command.add_argument('--version', required=True)
    command.add_argument('--preview', action='store_true')
    command = commands.add_parser('verify')
    command.add_argument('--artifact', type=Path, required=True)
    command = commands.add_parser('verify-set')
    command.add_argument('--directory', type=Path, required=True)
    command.add_argument('--version')
    command.add_argument('--source-revision')
    command.add_argument('--preview', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.command == 'check': result = releasing.check(args.version, 'preview' if args.preview else 'release', os.environ.get('GITHUB_REPOSITORY', 'unknown'))
        elif args.command == 'stage': result = {'stage': str(releasing.stage(ROOT, args.output))}
        elif args.command == 'build': result = {'artifact': str(releasing.build(ROOT, args.output, args.version, args.preview))}
        elif args.command == 'verify': result = releasing.verify_artifact(args.artifact, ROOT)
        else: result = releasing.verify_set(args.directory, ROOT, args.version, args.source_revision, args.preview)
    except (InstallerError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(json.dumps({'event': 'release-refused', 'message': str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__': raise SystemExit(main())
