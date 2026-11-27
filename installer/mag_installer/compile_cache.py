"""The compile cache: a pinned ccache that lets unchanged translation units skip compilation.

ccache keys each compilation by the compiler identity, the command line, and the content of the
source and of every file it includes (its direct mode), so a translation unit is reused only when
all of those are byte-identical: an unchanged generated lift source, runtime file or MAG source.

- The compiler identity is ours, not ccache's default (the compiler file's size and time): the
  real compiler's SHA-256 and version, and on macOS the selected SDK, so a compiler update never
  reuses older objects (``/usr/bin/cc`` is a shim whose size and time never change).
- ``base_dir`` is the build folder: ccache passes paths below it to the compiler relative to the
  build directory, so two builds in different folders share results. The compiler then sees
  relative paths whether or not the result came from the cache, so a hit and a miss give the same
  object.
- Only our configuration file is read (``CCACHE_CONFIGPATH``); inherited ``CCACHE_*`` variables
  are removed from the build environment.
- Not on Windows yet (SUPPORTED_HOSTS): every clang-cl compilation there carries ``/clang:``
  options ccache refuses (cmake/MagX86Level.cmake's, and CMake 4's per-file
  ``-clang:-MD -clang:-MF<object>.d`` dependency files), so it would cache nothing; hiding them
  from ccache would also lose the dependency files of cache hits. Windows builds compile as
  before and still reuse tool builds and generated sources.

The objects live in the per-user build cache (build_cache.py), never in the MAG or build folder.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess

from .model import InstallerError

CCACHE_VERSION = '4.14.3'
# ccache's official release archives (GitHub release assets, sizes and SHA-256 from the release's
# published digests). Kept apart from the managed LOCK dictionaries, whose identities key the
# dependency prefixes.
CCACHE_LOCKS = {
    'macos-arm64': {'id': 'ccache-macos', 'archive': 'tar.gz', 'size': 2233488,
                    'sha256': '764dd2a8100ff83e6bbd7b9a1e2016cc401be34bee8a70ea99698186622c34d5',
                    'url': f'https://github.com/ccache/ccache/releases/download/v{CCACHE_VERSION}/ccache-{CCACHE_VERSION}-darwin.tar.gz',
                    'executable': f'ccache-{CCACHE_VERSION}-darwin/ccache',
                    'max_expanded_bytes': 104857600, 'max_entries': 100},
    'linux-x86_64': {'id': 'ccache-linux', 'archive': 'tar.xz', 'size': 1353452,
                     'sha256': 'c94a5688546883eee4c87764b34fa2eb18a4879421a51589c37538aed6312dfc',
                     'url': f'https://github.com/ccache/ccache/releases/download/v{CCACHE_VERSION}/ccache-{CCACHE_VERSION}-linux-x86_64-glibc.tar.xz',
                     'executable': f'ccache-{CCACHE_VERSION}-linux-x86_64-glibc/ccache',
                     'max_expanded_bytes': 104857600, 'max_entries': 100},
}
# CMake languages the MAG and runtime builds compile on each host.
LANGUAGES = {'macos-arm64': ('C', 'CXX', 'OBJC', 'OBJCXX'), 'linux-x86_64': ('C', 'CXX')}
# Hosts whose builds compile through the cache (see the module notes for Windows).
SUPPORTED_HOSTS = tuple(CCACHE_LOCKS)


def ccache_tool(root, host, cache=None, token=None, events=None):
    """The pinned, hash-verified ccache executable inside the managed tools root."""
    from .managed_tools import materialize
    from .safety import Cancellation
    token = token or Cancellation(); events = events or (lambda event: None)
    spec = CCACHE_LOCKS[host]
    location = materialize(spec, Path(root) / 'artifacts', Path(cache) if cache else Path(root) / 'cache', token, events)
    executable = location / spec['executable']
    if not executable.is_file():
        raise InstallerError('The pinned ccache archive has an unexpected layout')
    if os.name != 'nt':
        executable.chmod(0o755)
    return str(executable)


def _run(argv, env):
    try:
        result = subprocess.run([str(a) for a in argv], env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, errors='replace', timeout=60)
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError(f'Cannot identify the compiler for the compile cache: {argv[0]}: {exc}') from exc
    if result.returncode:
        raise InstallerError(f'Cannot identify the compiler for the compile cache: {argv[0]} exited {result.returncode}')
    return (result.stdout + result.stderr).strip()


def _sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def _compilers(host, toolchain=None, env=None):
    env = env if env is not None else {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin'}
    parts = {'host': host}
    if host == 'windows-x86_64':
        clang_cl = Path(toolchain['clang_cl'])
        parts.update(clang_cl=_sha256(clang_cl), version=_run([clang_cl, '--version'], env),
                     msvc={key: env.get(key, '') for key in ('VCToolsVersion', 'WindowsSDKVersion', 'UCRTVersion')})
    elif host == 'linux-x86_64':
        for role in ('cc', 'cxx'):
            path = toolchain[role]
            parts[role] = {'path': path, 'sha256': _sha256(Path(path).resolve()), 'version': _run([path, '--version'], env)}
    else:
        for tool in ('clang', 'clang++'):
            path = _run(['/usr/bin/xcrun', '--find', tool], env)
            parts[tool] = {'path': path, 'sha256': _sha256(Path(path).resolve()), 'version': _run([path, '--version'], env)}
        parts['sdk'] = {'path': _run(['/usr/bin/xcrun', '--show-sdk-path'], env),
                        'version': _run(['/usr/bin/xcrun', '--show-sdk-version'], env)}
    return parts


def toolchain_identity(host, toolchain=None, env=None):
    """What builds the managed dependencies: the compilers (as compiler_identity) and, on Linux,
    the versions of the Ubuntu packages whose headers and libraries they build against."""
    parts = _compilers(host, toolchain, env)
    if host == 'linux-x86_64':
        from .managed_tools import LINUX_PACKAGES
        # dpkg-query exits 1 when a pattern matches nothing; the listing is still what is installed.
        try:
            listing = subprocess.run(['/usr/bin/dpkg-query', '-W', '-f=${Package} ${Version}\\n', *LINUX_PACKAGES,
                                      'libc6-dev', 'linux-libc-dev', 'libstdc++*-dev', 'libgcc*-dev'],
                                     env={'PATH': '/usr/bin:/bin'}, stdin=subprocess.DEVNULL, capture_output=True,
                                     text=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError) as exc:
            raise InstallerError(f'Cannot list the Ubuntu development packages: {exc}') from exc
        parts['packages'] = sorted(listing.splitlines())
    return parts


def compiler_identity(config, env):
    """A digest of the compilers this build runs: their real files, versions and (macOS) SDK."""
    from .native_recipe import config_host
    host = config_host(config)
    toolchain = config['msvc'] if host == 'windows-x86_64' else config.get('linux_toolchain')
    return hashlib.sha256(json.dumps(_compilers(host, toolchain, env), sort_keys=True).encode()).hexdigest()


def cmake_arguments(config, cmake_path):
    """The compiler launcher arguments for each language of this host."""
    from .native_recipe import config_host
    build_cache = config.get('build_cache')
    if not build_cache or not build_cache.get('ccache'):
        return []
    return [f'-DCMAKE_{language}_COMPILER_LAUNCHER=' + cmake_path(build_cache['ccache'])
            for language in LANGUAGES[config_host(config)]]


def environment(config, identity, stats_log):
    """The CCACHE_* variables of one compile stage."""
    from .build_cache import BuildCache
    cache = BuildCache(config['build_cache']['root'])
    directory = cache.objects_directory()
    configuration = directory / 'ccache.conf'
    # ccache reads a bare number as GiB.
    text = f'max_size = {cache.objects_max_bytes() // (1024 * 1024)}MiB\n'
    if not configuration.is_file() or configuration.read_text(encoding='utf-8') != text:
        configuration.write_text(text, encoding='utf-8')
    return {'CCACHE_DIR': str(directory), 'CCACHE_CONFIGPATH': str(configuration),
            'CCACHE_BASEDIR': str(config['build_cache']['base_dir']),
            'CCACHE_COMPILERCHECK': 'string:' + identity,
            'CCACHE_STATSLOG': str(stats_log)}


def timings_path(config):
    """Where the compile times of earlier cache misses are kept, for the time-saved estimate."""
    from .build_cache import BuildCache
    return BuildCache(config['build_cache']['root']).objects_directory() / 'mag-compile-times.json'


HITS = ('direct_cache_hit', 'preprocessed_cache_hit')
MISSES = ('cache_miss',)


def read_stats(path):
    """{source: 'hit' | 'miss' | other counter} from a ccache stats log, plus the totals."""
    results = {}
    current = None
    try:
        lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        lines = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            current = line[1:].strip()
            continue
        if current is None:
            continue
        if line in HITS:
            results[current] = 'hit'
        elif line in MISSES:
            results[current] = 'miss'
        elif current not in results:
            results[current] = line
    hits = sum(1 for value in results.values() if value == 'hit')
    misses = sum(1 for value in results.values() if value == 'miss')
    return results, hits, misses


def read_ninja_log(build_dir):
    """{output: milliseconds} of the latest run of each edge in build_dir/.ninja_log."""
    durations = {}
    try:
        lines = (Path(build_dir) / '.ninja_log').read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return durations
    for line in lines:
        if line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) >= 4 and fields[0].isdigit() and fields[1].isdigit():
            durations[fields[3]] = int(fields[1]) - int(fields[0])
    return durations


def compile_outputs(ninja, build_dir, env):
    """{normalized source path: object output} from Ninja's compilation database."""
    try:
        result = subprocess.run([str(ninja), '-C', str(build_dir), '-t', 'compdb'], env=env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=120)
        records = json.loads(result.stdout) if result.returncode == 0 else []
    except (OSError, subprocess.SubprocessError, ValueError):
        return {}
    mapping = {}
    for record in records if isinstance(records, list) else []:
        if not isinstance(record, dict) or not record.get('file') or not record.get('output'):
            continue
        source = Path(record['file'])
        if not source.is_absolute():
            source = Path(record.get('directory', build_dir)) / source
        mapping[os.path.normcase(os.path.normpath(str(source)))] = record['output']
    return mapping


def summarize(stage, build_dir, stats_log, ninja, env, timings_path, wall_seconds):
    """The hit rate of one compile stage and an estimate of the time the hits saved.

    The estimate is, for each object taken from the cache, the time its source's last compilation
    took (remembered from earlier builds in timings_path) minus the time the hit took."""
    results, hits, misses = read_stats(stats_log)
    durations = read_ninja_log(build_dir)
    outputs = compile_outputs(ninja, build_dir, env)
    try:
        remembered = json.loads(Path(timings_path).read_text(encoding='utf-8'))
        if not isinstance(remembered, dict): remembered = {}
    except (OSError, ValueError):
        remembered = {}
    # By source as ccache names it (relative to the build directory, the same in both builds), so a
    # file the launcher compiled and the engine reuses is estimated too.
    known = remembered.setdefault('sources', {})
    if not isinstance(known, dict):
        known = remembered['sources'] = {}
    saved_ms = 0
    unknown = 0
    for source, result in results.items():
        path = Path(source)
        if not path.is_absolute():
            path = Path(build_dir) / path
        output = outputs.get(os.path.normcase(os.path.normpath(str(path))))
        if output is None or output not in durations:
            unknown += result == 'hit'
            continue
        if result == 'miss':
            known[source] = durations[output]
        elif result == 'hit':
            if source in known:
                saved_ms += max(0, known[source] - durations[output])
            else:
                unknown += 1
    try:
        Path(timings_path).write_text(json.dumps(remembered, sort_keys=True), encoding='utf-8')
    except OSError:
        pass
    total = hits + misses
    rate = (100.0 * hits / total) if total else 0.0
    return {'event': 'compile-cache', 'stage': stage, 'hits': hits, 'misses': misses,
            'hit_rate': round(rate, 1), 'saved_seconds': round(saved_ms / 1000.0, 1),
            'unestimated_hits': unknown, 'seconds': round(wall_seconds, 1)}


def message(summary):
    from .build_cache import duration_text
    total = summary['hits'] + summary['misses']
    text = (f'Compile cache ({summary["stage"]}): {summary["hits"]} of {total} compilations reused '
            f'({summary["hit_rate"]:.0f}%)')
    if summary['saved_seconds'] >= 1:
        text += f', about {duration_text(summary["saved_seconds"])} of compiler time saved'
    if summary.get('unestimated_hits'):
        text += f' ({summary["unestimated_hits"]} reused without a recorded compile time, not counted)'
    return text + f'; the stage took {duration_text(summary["seconds"])}.'
