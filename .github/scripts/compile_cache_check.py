"""Check the installer's compile cache on a real toolchain, with no game data.

A small synthetic C and C++ project is built three times through the pinned ccache, with the
environment the installer's compile steps use (installer/mag_installer/compile_cache.py):

1. in folder one with an empty cache: every compilation is a miss;
2. in folder two with that cache: every compilation must be a hit, although every path differs;
3. in folder two again, with a second empty cache: a cold build in the same place.

The objects of 2 (from the cache) and 3 (compiled) must be identical: a cache hit gives what the
compiler would have produced there. COFF objects carry a creation time, which is masked.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'installer'))

from mag_installer import compile_cache  # noqa: E402

SOURCES = {
    'CMakeLists.txt': 'cmake_minimum_required(VERSION 3.20)\nproject(cache_check C CXX)\n'
                      'add_library(cache_check STATIC a.c b.cpp nested/c.c)\n'
                      'target_include_directories(cache_check PRIVATE include)\n'
                      'set_target_properties(cache_check PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)\n'
                      # clang-cl options ccache cannot parse, as cmake/MagX86Level.cmake adds them.
                      'if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")\n'
                      '  target_compile_options(cache_check PRIVATE /clang:-ffp-contract=off)\n'
                      'endif()\n',
    'include/shared.h': '#define SHARED_VALUE 41\nstatic inline int shared_add(int x) { return x + SHARED_VALUE; }\n',
    'a.c': '#include "shared.h"\nconst char *a_file = __FILE__;\nint a(int x) { return shared_add(x); }\n',
    'b.cpp': '#include "shared.h"\n#include <string>\n#include <vector>\n'
             'std::string b(int x) { std::vector<int> v(3, shared_add(x)); return std::to_string(v[1]) + __FILE__; }\n',
    'nested/c.c': '#include "shared.h"\nint c(void) { return SHARED_VALUE * 2; }\n',
}


def objects(build):
    found = {}
    for path in sorted(build.rglob('*')):
        if path.suffix in ('.o', '.obj') and path.is_file():
            data = bytearray(path.read_bytes())
            if path.suffix == '.obj' and len(data) >= 8:
                data[4:8] = b'\0\0\0\0'   # COFF TimeDateStamp
            found[path.relative_to(build).as_posix()] = bytes(data)
    return found


def build(cmake, ninja, ccache, compilers, env, identity, folder, cache_root, label):
    source = folder / 'src'
    for name, text in SOURCES.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    out = folder / 'build'
    config = {'build_cache': {'root': str(cache_root), 'ccache': str(ccache), 'base_dir': str(folder)}}
    stats = folder / 'ccache-stats.log'
    launcher = [f'-DCMAKE_{language}_COMPILER_LAUNCHER={Path(ccache).as_posix()}' for language in ('C', 'CXX')]
    subprocess.run([str(cmake), '-S', str(source), '-B', str(out), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_MAKE_PROGRAM=' + Path(ninja).as_posix(), *compilers, *launcher],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    stage_env = {**env, **compile_cache.environment(config, identity, stats, compile_cache.passthrough_options(ninja, out, env))}
    subprocess.run([str(cmake), '--build', str(out)], env=stage_env, check=True)
    results, hits, misses = compile_cache.read_stats(stats)
    print(f'{label}: {hits} hits, {misses} misses: {json.dumps(results)}', flush=True)
    return hits, misses, objects(out)


def check(cmake, ninja, ccache, compilers, env, identity, work):
    work = Path(work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    first = build(cmake, ninja, ccache, compilers, env, identity, work / 'one', work / 'cache-a', 'cold, folder one')
    warm = build(cmake, ninja, ccache, compilers, env, identity, work / 'two', work / 'cache-a', 'warm, folder two')
    shutil.rmtree(work / 'two')
    cold = build(cmake, ninja, ccache, compilers, env, identity, work / 'two', work / 'cache-b', 'cold, folder two')
    if first[:2] != (0, 3) or cold[:2] != (0, 3):
        raise SystemExit(f'cold builds must compile all three sources: {first[:2]}, {cold[:2]}')
    if warm[:2] != (3, 0):
        raise SystemExit(f'the build in another folder must take all three objects from the cache: {warm[:2]}')
    if warm[2] != cold[2] or len(warm[2]) != 3:
        differing = sorted(name for name in set(warm[2]) | set(cold[2]) if warm[2].get(name) != cold[2].get(name))
        raise SystemExit(f'cache hits differ from a cold build in the same folder: {differing}')
    print('compile cache: hits across folders, identical to a cold build', flush=True)


def diagnose(ccache, env, folder, stats):
    """What ccache did when its stats log shows no cacheable compilation: its counters, and its
    own log of one compile command run again."""
    print(f'stats log {stats}: exists={stats.exists()} size={stats.stat().st_size if stats.exists() else 0}', flush=True)
    if stats.exists():
        print('stats log head:', stats.read_text(errors='replace')[:400], flush=True)
    subprocess.run([str(ccache), '--show-stats', '-v'], env=env)
    ninja = shutil.which('ninja', path=env.get('PATH') or env.get('Path'))
    commands = subprocess.run([ninja or 'ninja', '-C', str(folder), '-t', 'commands'], env=env, capture_output=True, text=True)
    compile = next((line for line in commands.stdout.splitlines() if 'ccache' in line), None)
    print('compile command:', (compile or commands.stderr)[:1500], flush=True)
    if compile:
        log = folder.with_name(folder.name + '-ccache-debug.log')
        subprocess.run(compile, shell=True, cwd=folder, env={**env, 'CCACHE_LOGFILE': str(log)})
        if log.exists():
            lines = log.read_text(errors='replace').splitlines()
            print('ccache log:', *[line for line in lines if 'nsupported' in line or 'too hard' in line.lower()][:20] or lines[-30:], sep='\n')


def rebuild_hits(cmake, ccache, env, identity, cache_root, base_dir, configure, build_args, folders, minimum=0.95):
    """Build one project in two build folders through the compile cache: the first compiles, the
    second must take at least `minimum` of its compilations from the cache."""
    config = {'build_cache': {'root': str(cache_root), 'ccache': str(ccache), 'base_dir': str(base_dir)}}
    launcher = [f'-DCMAKE_{language}_COMPILER_LAUNCHER={Path(ccache).as_posix()}' for language in ('C', 'CXX')]
    counts = []
    for folder in folders:
        stats = folder.with_name(folder.name + '-ccache.log')
        subprocess.run([str(a) for a in configure(folder)] + launcher, env=env, check=True)
        # As the installer does: clang-cl's /clang: options enter the identity once configured.
        ninja = shutil.which('ninja', path=env.get('PATH') or env.get('Path'))
        passthrough = compile_cache.passthrough_options(ninja, folder, env)
        print(f'{folder.name}: /clang: options {passthrough["options"]}', flush=True)
        stage_env = {**env, **compile_cache.environment(config, identity, stats, passthrough)}
        subprocess.run([str(cmake), '--build', str(folder), *map(str, build_args)], env=stage_env, check=True)
        counts.append(compile_cache.read_stats(stats)[1:])
        print(f'{folder.name}: {counts[-1][0]} hits, {counts[-1][1]} misses', flush=True)
        if not sum(counts[-1]):
            diagnose(ccache, stage_env, folder, stats)
    (cold_hits, cold_misses), (hits, misses) = counts
    if not cold_misses or not hits or hits < minimum * (hits + misses):
        raise SystemExit(f'the second build must reuse at least {minimum:.0%} of its compilations: {counts}')
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for name in ('cmake', 'ninja', 'ccache', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--cxx', required=True)
    args = parser.parse_args()
    env = dict(os.environ)
    for key in [k for k in env if k.upper().startswith('CCACHE_')]:
        env.pop(key)
    if os.name == 'nt':
        config = {'msvc': {'clang_cl': args.cc}}
    elif sys.platform.startswith('linux'):
        config = {'host': 'linux-x86_64', 'linux_toolchain': {'cc': args.cc, 'cxx': args.cxx}}
    else:
        config = {}
    identity = compile_cache.compiler_identity(config, env)
    compilers = ['-DCMAKE_C_COMPILER=' + Path(args.cc).as_posix(), '-DCMAKE_CXX_COMPILER=' + Path(args.cxx).as_posix()]
    check(args.cmake, args.ninja, args.ccache, compilers, env, identity, args.work)
    return 0


if __name__ == '__main__':
    sys.exit(main())
