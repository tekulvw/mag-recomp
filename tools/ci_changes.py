"""Decide which CI jobs a pull request's changes reach.

The `changes` jobs in .github/workflows/tests.yml, render-frontend.yml and installers.yml run this
and gate their other jobs on its outputs. Pushes, manual and called runs, release-please pull
requests, a change to this file and a diff that cannot be read all run every job. When unsure
whether a job reads a path, list the path: an extra run is cheaper than a missed regression.

Filter map (fnmatch patterns over repository paths; `*` also matches `/`):

- python (required check): every change except documentation, that is files under docs/ and
  Markdown files, other than the documents tests read (DOCS_READ_BY_TESTS).
- rust_input: the mag-input crate and its toolchain.
- windows_native: C/C++ sources, tests, tools, CMake, pins and the mag-input crate, for the hosted
  Windows legs that compile parts of src/.
- macos_settings: the macOS settings panel's sources and fixtures.
- render: the rendering crates, shaders, the render C/C++ boundary and their checks.
- installers: everything a source kit, an installer build or its native compile reads.
- installer_toolchain: the same without src/; gates the Linux managed toolchain build, which reads
  no MAG source.

A job's own workflow file and the shared actions are always in its list. Documentation reaches only
the installer jobs (the installer ships its README and docs, and the kit NOTICE.md).

Synthetic tests: tests/test_ci_changes.py.
"""
import argparse
import fnmatch
import os
import subprocess
import sys

SELF = 'tools/ci_changes.py'

# Markdown that synthetic tests read from the repository (tests/test_sdk_pins.py and
# tools/check_ps3recomp_flags.py), so it is not documentation-only for the python job.
DOCS_READ_BY_TESTS = ('BUILDING.md', 'docs/ps3recomp-flags.md')

ACTIONS = '.github/actions/*'

FILTERS = {
    'rust_input': (
        'crates/mag-input/*', 'rust-toolchain.toml', ACTIONS, '.github/workflows/tests.yml',
    ),
    'windows_native': (
        'src/*', 'tests/*', 'tools/*', 'cmake/*', 'CMakeLists.txt', 'profiles/*',
        'crates/mag-input/*', 'rust-toolchain.toml', ACTIONS, '.github/workflows/tests.yml',
    ),
    'macos_settings': (
        'src/player_settings*', 'src/player_controls*', 'src/player_portal*', 'src/player_report*',
        'src/player_input_desktop_macos*', 'src/player_input_macos*',
        'tests/player_settings*', 'tests/test_player_settings*',
        'tests/player_controls*', 'tests/test_player_controls*',
        'tests/player_reports*', 'tests/test_player_reports*',
        'cmake/MagPlayerSettings*', 'tools/player_settings_preview*',
        '.github/workflows/tests.yml',
    ),
    'render': (
        'crates/mag-render*', 'shaders/*',
        'src/mag_render_*', 'src/render_adapter/*',
        'cmake/MagRender.cmake', 'cmake/MagRenderAdapter.cmake', 'cmake/MagRenderPrograms.cmake',
        'tests/__init__.py', 'tests/test_rust_render_build.py', 'tests/test_render_frontend_*',
        'tests/fixtures/cmake_rust_render/*', 'tests/fixtures/cmake_render_adapter/*',
        'tests/gpu_frontend/*',
        'tools/__init__.py', 'tools/provenance.py',
        'tools/check_render_frontend_*', 'tools/test_render_frontend_*',
        'rust-toolchain.toml', ACTIONS, '.github/workflows/render-frontend.yml',
    ),
    'installers': (
        'installer/*', 'src/*', 'tools/*', 'cmake/*', 'CMakeLists.txt', 'profiles/*',
        'crates/mag-input/*', 'rust-toolchain.toml',
        'LICENSE', 'LICENSE.ps3recomp', 'NOTICE.md', '.release-please-manifest.json',
        '.github/scripts/*', ACTIONS,
        '.github/workflows/installers.yml', '.github/workflows/release-please.yml',
    ),
}
FILTERS['installer_toolchain'] = tuple(p for p in FILTERS['installers'] if p != 'src/*')

FLAGS = ('python', *FILTERS)

# Flags whose jobs package Markdown, so documentation still reaches them.
READS_DOCUMENTATION = ('installers', 'installer_toolchain')


def is_documentation(path):
    if path in DOCS_READ_BY_TESTS:
        return False
    return path.startswith('docs/') or path.lower().endswith('.md')


def matches(path, patterns):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def classify(files, event='pull_request', head_ref=''):
    """Map each flag to whether its jobs run. files is None when the diff is unknown."""
    files = None if files is None else [f.strip() for f in files if f.strip()]
    if (event != 'pull_request' or head_ref.startswith('release-please--') or not files
            or SELF in files):
        return {flag: True for flag in FLAGS}
    code = [f for f in files if not is_documentation(f)]
    result = {'python': bool(code)}
    for flag, patterns in FILTERS.items():
        considered = files if flag in READS_DOCUMENTATION else code
        result[flag] = any(matches(f, patterns) for f in considered)
    return result


def changed_files(base, head):
    try:
        out = subprocess.run(['git', 'diff', '--no-renames', '--name-only', base, head],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'::warning::cannot list changed files ({error}); running every job', file=sys.stderr)
        return None
    return out.splitlines()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--event', default=os.environ.get('GITHUB_EVENT_NAME', ''))
    parser.add_argument('--head-ref', default=os.environ.get('GITHUB_HEAD_REF', ''))
    parser.add_argument('--base', default='HEAD^1', help='revision to diff from (pull requests)')
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--files-from', help='read the changed paths from this file instead of git')
    args = parser.parse_args(argv)
    if args.event != 'pull_request':
        files = None
    elif args.files_from:
        with open(args.files_from, encoding='utf-8') as handle:
            files = handle.read().splitlines()
    else:
        files = changed_files(args.base, args.head)
    result = classify(files, args.event, args.head_ref)
    lines = [f'{flag}={"true" if result[flag] else "false"}' for flag in FLAGS]
    output = os.environ.get('GITHUB_OUTPUT')
    if output:
        with open(output, 'a', encoding='utf-8') as handle:
            handle.write(''.join(line + '\n' for line in lines))
    print(f'{len(files) if files is not None else "unknown"} changed files')
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
