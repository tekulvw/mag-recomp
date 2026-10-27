"""Exercise the installer's Windows toolchain route end to end, with no game data.

Uses the installer's own code: Build Tools detection, the pinned managed
downloads, the vcvars-derived build environment, then the managed CMake/Ninja
with clang-cl and the static CRT to build the pinned runtime library (with the
Windows POSIX shim, as the MAG build applies it), the managed Python modules,
and the package-time compile of the MAG.exe supervisor. Only lifting and
compiling the player's game is absent.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'installer'))

from mag_installer import managed_tools, native_build, native_package_windows  # noqa: E402
from mag_installer.safety import Cancellation  # noqa: E402


def events(event):
    if event.get('event') not in ('stage-activity', 'download'):
        print(json.dumps(event), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--runtime-sdk', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve(); work.mkdir(parents=True, exist_ok=True)
    sdk = args.runtime_sdk.resolve()
    token = Cancellation()

    print('readiness:', json.dumps(managed_tools.readiness(ROOT / 'installer')), flush=True)
    toolchain = managed_tools.ensure_windows_build_tools(cache=work / 'cache', token=token, events=events)
    print('toolchain:', json.dumps(toolchain), flush=True)
    managed = managed_tools.bootstrap(work / 'managed', cache=work / 'cache', token=token, events=events)
    native_build.check_windows_location(work / 'MAG')

    config = {**{k: managed[k] for k in ('python', 'cmake', 'ninja', 'git')}, 'jobs': 2,
              'msvc': {k: toolchain[k] for k in ('vcvars', 'clang_cl')}}
    home = work / 'home'; home.mkdir(exist_ok=True)
    env = native_build._environment(config, home)

    def step(name, argv, cwd):
        print(f'== {name}', flush=True)
        subprocess.run([str(a) for a in argv], cwd=cwd, env=env, check=True)

    def posix(value): return Path(value).as_posix()
    step('managed git on the build PATH', [managed['git'], '--version'], work)
    step('clang-cl', [toolchain['clang_cl'], '--version'], work)
    step('python modules', [managed['python'], '-c', 'import Crypto, capstone, construct, tabulate, tqdm; print("ok")'], work)
    wrapper = work / 'runtime-project'; wrapper.mkdir(exist_ok=True)
    (wrapper / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(runtime_smoke C CXX)\n'
        f'add_subdirectory("{posix(sdk)}" sdk EXCLUDE_FROM_ALL)\n'
        f'include("{posix(ROOT / "cmake/MagWin32Posix.cmake")}")\n'
        f'mag_win32_posix_runtime("{posix(sdk)}")\n')
    build = work / 'runtime-build'
    step('configure runtime', [managed['cmake'], '-S', wrapper, '-B', build, '-G', 'Ninja',
                               '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MAKE_PROGRAM=' + posix(managed['ninja']),
                               '-DCMAKE_C_COMPILER=' + posix(toolchain['clang_cl']),
                               '-DCMAKE_CXX_COMPILER=' + posix(toolchain['clang_cl']),
                               '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
                               '-DPython3_EXECUTABLE=' + posix(managed['python'])], work)
    step('build runtime', [managed['cmake'], '--build', build, '--target', 'ps3recomp_runtime'], work)

    scratch = work / 'supervisor'; scratch.mkdir(exist_ok=True)
    (scratch / 'supervisor_config.h').write_text(
        '#define MAG_WORKSPACE L"..\\\\build-work\\\\workspace"\n' +
        ''.join(f'#define {n} "{"0" * 64}"\n' for n in ('MAG_LAUNCHER_SHA256', 'MAG_ENGINE_SHA256',
                'MAG_LAUNCHER_ELF_SHA256', 'MAG_ENGINE_ELF_SHA256', 'MAG_ENGINE_SELF_SHA256')))
    (scratch / 'supervisor.c').write_text(native_package_windows._SUPERVISOR)
    native_package_windows._compile(toolchain['clang_cl'], env, scratch, scratch / 'MAG.exe', token)
    print('Windows toolchain route verified (no game data)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
