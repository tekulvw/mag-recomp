"""Check Conventional Commit PR titles and require releasable SDK updates.

The input is JSON containing title, files (GitHub PR file records), and
changed_files (the API's total). No PR-controlled code is loaded or executed.
"""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK_PATHS = frozenset({'profiles/framework.json'})
SDK_TYPES = frozenset({'feat', 'fix', 'perf'})
TITLE = re.compile(r'([a-z]+)(?:\([^()\s:]+\))?(!)?: (\S[^\r\n]*)')


def validate_title(title, paths, config):
    """Return policy errors; configured conventional types remain valid elsewhere."""
    if not isinstance(title, str) or any(ord(c) < 32 or ord(c) == 127 for c in title):
        return ['PR title must be a single line without control characters.']
    match = TITLE.fullmatch(title)
    types = {section['type'] for section in config['changelog-sections']}
    if not match or match[1] not in types:
        return ['Use a Conventional Commit title: type(scope): description '
                '(optional scope and !); configured types: ' + ', '.join(sorted(types)) + '.']
    if SDK_PATHS.intersection(paths) and match[1] not in SDK_TYPES:
        return ['SDK profile/pin changes require feat:, fix:, or perf: '
                '(optional scope and !), e.g. fix(sdk): update the reviewed runtime pin. '
                'A build/chore/ci/docs title can omit the SDK change from a release.']
    return []


def changed_paths(data):
    """Reject missing/truncated metadata; inspect both sides of renamed files."""
    files, total = data['files'], data['changed_files']
    if not isinstance(files, list) or type(total) is not int or total < 0:
        raise ValueError('Invalid PR changed-file metadata.')
    if len(files) != total:
        raise ValueError('Incomplete PR file list; cannot safely check SDK changes '
                         '(GitHub returns at most 3000 files).')
    paths = set()
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get('filename'), str):
            raise ValueError('Invalid PR filename metadata.')
        paths.add(entry['filename'])
        if 'previous_filename' in entry:
            if not isinstance(entry['previous_filename'], str):
                raise ValueError('Invalid previous PR filename metadata.')
            paths.add(entry['previous_filename'])
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True,
                        help='JSON metadata collected with the read-only GitHub API')
    args = parser.parse_args()
    try:
        data = json.loads(args.input.read_text())
        config = json.loads((ROOT / 'release-please-config.json').read_text())
        errors = validate_title(data['title'], changed_paths(data), config)
    except (OSError, ValueError, KeyError, TypeError) as error:
        errors = [str(error)]
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print('PR title satisfies the Conventional Commit and SDK release policy.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
