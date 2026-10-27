"""Compile MAG's own sources for Windows against the pinned runtime, with no game data.

The real build lifts the player's game; CI has none. This builds the pinned
runtime library through cmake/MagWin32Posix.cmake (exactly as the MAG build
applies it), writes the SDK's synthetic smoke-title ppu_recomp.h, and compiles
every MAG translation unit, the SDK boot scaffold, the musttail SPU dispatcher
and the installer's MAG.exe supervisor with clang-cl. Linking the game and
generated title code are out of scope. Run from an MSVC developer environment.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

COMMON = ['src/main.cpp', 'src/mag_jobio_selector.cpp', 'src/mag_timer_gcm_pump_diagnostics.cpp', 'src/stubs.cpp']
LAUNCHER = ['src/retail_handoff.cpp']
ENGINE = ['src/mag_sdk_prepare_screen.c', 'src/mag_jobio_capture.c', 'src/mag_graphics_trace.cpp']
SDK_FIRST = {'src/mag_timer_gcm_pump_diagnostics.cpp', 'src/mag_jobio_selector.cpp'}
SCAFFOLD = ['runtime/ppu/ppu_loader.cpp', 'runtime/ppu/ppu_hle.cpp', 'runtime/ppu/ppu_fs.cpp', 'runtime/ppu/ppu_sysprx.cpp']


def run(argv, **kwargs):
    print('+', ' '.join(map(str, argv)), flush=True)
    return subprocess.run([str(a) for a in argv], **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--runtime-sdk', type=Path, required=True)
    parser.add_argument('--ppu-sdk', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    sdk, work = args.runtime_sdk.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    header_dir = work / 'synthetic-lift'
    header_dir.mkdir(exist_ok=True)
    run([sys.executable, args.ppu_sdk.resolve() / 'tools/make_smoke_elf.py', '--header', header_dir / 'ppu_recomp.h'], check=True)

    # The pinned runtime, with the Windows POSIX shim applied by MAG's own CMake module.
    wrapper = work / 'runtime-project'
    wrapper.mkdir(exist_ok=True)
    (wrapper / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(runtime_check C CXX)\n'
        f'add_subdirectory("{sdk.as_posix()}" sdk EXCLUDE_FROM_ALL)\n'
        f'include("{(ROOT / "cmake/MagWin32Posix.cmake").as_posix()}")\n'
        f'mag_win32_posix_runtime("{sdk.as_posix()}")\n')
    build = work / 'runtime'
    run(['cmake', '-S', wrapper, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'], check=True)
    # -k 0: report every broken runtime unit, then still check MAG's own sources.
    runtime = run(['cmake', '--build', build, '--target', 'ps3recomp_runtime', '--', '-k', '0'])

    shim = ROOT / 'src/win32/posix'
    base = [ROOT / 'src', shim, header_dir, sdk / 'include', sdk / 'runtime/ppu', sdk / 'runtime/spu', sdk / 'runtime/memory',
            sdk / 'runtime/syscalls', sdk / 'runtime/platform', sdk / 'libs/video', ROOT]
    flags = ['/nologo', '/c', '/W0', '-fno-strict-aliasing', '/EHsc', '/O1', '/MT',
             '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE',
             '/DPS3RECOMP_CRITICAL_WRITE_AVAILABLE=1']  # exported by the runtime target
    jobs = []
    for image, extra in (('EBOOT', LAUNCHER), ('mage_g', ENGINE)):
        for name in COMMON + extra:
            includes = list(base)
            if name in SDK_FIRST: includes = [sdk] + includes
            if name in ENGINE: includes = [sdk / 'runtime', sdk] + includes
            defines = ['/DMAG_LAUNCHER_HOOKS'] if image == 'EBOOT' and name == 'src/stubs.cpp' else []
            jobs.append((image, ROOT / name, includes, defines))
        for name in SCAFFOLD:
            jobs.append((image, sdk / name, list(base), []))
    # The musttail SPU dispatch path MAG compiles itself (CMake: SPU_USE_DISPATCH_MT, -O2).
    jobs.append(('mage_g', sdk / 'runtime/spu/spu_dispatch_mt.c', [sdk / 'runtime/spu'] + base, ['/DSPU_USE_DISPATCH_MT', '-O2']))
    failures = []
    for image, source, includes, defines in jobs:
        out = work / 'obj' / image / (source.stem + '.obj')
        out.parent.mkdir(parents=True, exist_ok=True)
        language = ['/TC', '/std:c17'] if source.suffix == '.c' else ['/TP', '/std:c++20']
        result = run(['clang-cl', *flags, *language, *defines, *(f'/I{p}' for p in includes),
                      f'/Fo{out}', source], capture_output=True, text=True)
        if result.returncode:
            failures.append(f'{image}: {source.name}')
            print(result.stdout[-6000:], result.stderr[-6000:], sep='\n', flush=True)

    # The installer compiles MAG.exe at package time; build it with synthetic identities.
    sys.path.insert(0, str(ROOT / 'installer'))
    from mag_installer import native_package_windows as supervisor
    from mag_installer.safety import Cancellation
    scratch = work / 'supervisor'
    scratch.mkdir(exist_ok=True)
    digest = '0' * 64
    (scratch / 'supervisor_config.h').write_text(
        '#define MAG_WORKSPACE ' + supervisor._c_wide('..\\build-work\\workspace') + '\n' +
        ''.join(f'#define {name} "{digest}"\n' for name in ('MAG_LAUNCHER_SHA256', 'MAG_ENGINE_SHA256',
                'MAG_LAUNCHER_ELF_SHA256', 'MAG_ENGINE_ELF_SHA256', 'MAG_ENGINE_SELF_SHA256')))
    (scratch / 'supervisor.c').write_text(supervisor._SUPERVISOR)
    try:
        supervisor._compile('clang-cl', dict(os.environ), scratch, scratch / 'MAG.exe', Cancellation())
        print('MAG.exe supervisor compiled and linked')
    except Exception as exc:
        failures.append(f'supervisor: {exc}')
    print(f'{len(jobs) + 1 - len(failures)}/{len(jobs) + 1} translation units and the supervisor compiled')
    if runtime.returncode:
        failures.append('runtime library (see ninja FAILED lines above)')
    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
