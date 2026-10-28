"""Compile the pinned Linux runtime and MAG translation units using synthetic headers.

No game inputs or generated title code are read. Full player builds are separate.
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

    build = work / 'runtime'
    run(['cmake', '-S', sdk, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
         '-DPS3RECOMP_RSX_VULKAN=ON', '-DPS3RECOMP_FFMPEG_RUNTIME=ON'], check=True)
    runtime = run(['cmake', '--build', build, '--target', 'ps3recomp_runtime', '--parallel', '2'])
    base = [ROOT / 'src', header_dir, sdk / 'include', sdk / 'runtime/ppu', sdk / 'runtime/spu',
            sdk / 'runtime/memory', sdk / 'runtime/syscalls', sdk / 'runtime/platform', sdk / 'libs/video', ROOT]
    flags = ['-c', '-w', '-fno-strict-aliasing', '-O1', '-DPS3RECOMP_CRITICAL_WRITE_AVAILABLE=1', '-DPS3RECOMP_RSX_VULKAN=1']
    jobs = []
    for image, extra in (('EBOOT', LAUNCHER), ('mage_g', ENGINE)):
        for name in COMMON + extra:
            includes = list(base)
            if name in SDK_FIRST: includes = [sdk] + includes
            if name in ENGINE: includes = [sdk / 'runtime', sdk] + includes
            defines = ['-DMAG_LAUNCHER_HOOKS'] if image == 'EBOOT' and name == 'src/stubs.cpp' else []
            jobs.append((image, ROOT / name, includes, defines))
        for name in SCAFFOLD:
            jobs.append((image, sdk / name, list(base), []))
    # The musttail SPU dispatch path MAG compiles itself (CMake: SPU_USE_DISPATCH_MT, -O2).
    jobs.append(('mage_g', sdk / 'runtime/spu/spu_dispatch_mt.c', [sdk / 'runtime/spu'] + base, ['-DSPU_USE_DISPATCH_MT', '-O2']))
    failures = []
    for image, source, includes, defines in jobs:
        out = work / 'obj' / image / (source.stem + '.obj')
        out.parent.mkdir(parents=True, exist_ok=True)
        language = ['-std=c17'] if source.suffix == '.c' else ['-std=c++20']
        result = run(['clang' if source.suffix == '.c' else 'clang++', *flags, *language, *defines, *(f'-I{p}' for p in includes),
                      '-o', out, source], capture_output=True, text=True)
        if result.returncode:
            failures.append(f'{image}: {source.name}')
            print(result.stdout[-6000:], result.stderr[-6000:], sep='\n', flush=True)

    # Native synthetic supervisor execution is exercised by this platform-specific suite.
    supervisor = run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests',
                      '-p', 'test_native_package_linux.py'], cwd=ROOT / 'installer')
    if runtime.returncode: failures.append('runtime library')
    if supervisor.returncode: failures.append('Linux supervisor tests')
    if failures:
        print('FAILED: ' + ', '.join(failures))
        return 1
    print(f'{len(jobs)} Linux translation units and supervisor checks passed')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
