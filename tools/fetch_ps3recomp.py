"""Check out every pinned ps3recomp revision from profiles/framework.json and verify it.

Each role is cloned (or fetched) into its pinned path, checked out detached at the
pinned commit, and refused unless the role's tag resolves to exactly that commit and
the checkout is clean. The commit is authoritative; the tag is only a verified name.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, WORKSPACE, load_framework


def git(path, *args):
    return subprocess.run(['git', '-C', str(path), *args], check=True, capture_output=True, text=True).stdout.strip()


def pins(framework):
    table = framework['ps3recomp']
    for role, pin in table.items():
        if not all(isinstance(pin.get(k), str) and pin[k] for k in ('tag', 'commit', 'path')):
            raise ValueError(f'Incomplete ps3recomp pin for {role}')
    return table


def fetch(root, url, role, pin):
    path = root / pin['path']
    if not (path / '.git').exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--quiet', '--no-checkout', url, str(path)], check=True)
    else:
        git(path, 'fetch', '--quiet', '--tags', url)
    tagged = git(path, 'rev-parse', '--verify', '--quiet', f"refs/tags/{pin['tag']}^{{commit}}")
    if tagged != pin['commit']:
        raise ValueError(f"{role}: tag {pin['tag']} is {tagged}, pinned commit is {pin['commit']}")
    git(path, 'checkout', '--quiet', '--detach', pin['commit'])
    if git(path, 'rev-parse', 'HEAD') != pin['commit'] or git(path, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError(f'{role}: checkout at {path} is not clean at the pinned commit')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT, help='repository whose profiles/framework.json is used')
    parser.add_argument('--url', help='fork URL (default: the "fork" entry of profiles/framework.json)')
    parser.add_argument('--role', action='append', help='only this role (repeatable)')
    args = parser.parse_args()
    # Run against this repository, the pins in effect (workspace overlay included) are checked out
    # into the workspace; with an explicit --root, that repository's own pins and paths are used.
    own = args.root.resolve() == ROOT
    framework = load_framework()[0] if own else json.loads((args.root / 'profiles/framework.json').read_text())
    target = WORKSPACE if own else args.root
    url = args.url or framework['fork'] + '.git'
    for role, pin in pins(framework).items():
        if args.role and role not in args.role:
            continue
        path = fetch(target, url, role, pin)
        print(f"{role}: {pin['tag']} = {pin['commit'][:12]} at {path.relative_to(target)}")


if __name__ == '__main__':
    sys.exit(main())
