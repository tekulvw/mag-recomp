"""Exercise the installer's Windows toolchain route end to end, with no game data.

Uses the installer's own code: Build Tools detection, the pinned managed
downloads, the vcvars-derived build environment, then the managed CMake/Ninja
with clang-cl and the static CRT to build the pinned runtime library (with the
Windows POSIX shim, as the MAG build applies it), the managed Python modules,
the package-time compile of the MAG.exe supervisor, and the pinned static Opus
that voice builds link. Only lifting and compiling the player's game is absent.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'installer'))

from mag_installer import build_cache, managed_tools, native_build, native_package_windows  # noqa: E402
from mag_installer.safety import Cancellation  # noqa: E402

SEEN = []
# Voice builds link the pinned static Opus; a 40 ms frame through its encoder and decoder.
OPUS_CHECK = r"""#include <opus/opus.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    int error;
    OpusEncoder *encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK) return 1;
    OpusDecoder *decoder = opus_decoder_create(16000, 1, &error);
    if (error != OPUS_OK) return 2;
    opus_int16 pcm[640], decoded[640];
    for (int i = 0; i < 640; i++) pcm[i] = (opus_int16)(8000.0 * sin(i * 0.17));
    unsigned char packet[400];
    int bytes = opus_encode(encoder, pcm, 640, packet, sizeof packet);
    if (bytes <= 0) return 3;
    if (opus_decode(decoder, packet, bytes, decoded, 640, 0) != 640) return 4;
    printf("%s: a 40 ms frame round trip in %d bytes\n", opus_get_version_string(), bytes);
    return 0;
}
"""


def events(event):
    SEEN.append(event)
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
    # The build cache: the dependency prefix built once is restored into a second managed root.
    store = build_cache.BuildCache(work / 'user-cache/build-cache', token, events)
    managed_tools.bootstrap(work / 'managed-first', cache=work / 'cache', token=token, events=events, build_cache=store)
    managed = managed_tools.bootstrap(work / 'managed', cache=work / 'cache', token=token, events=events, build_cache=store)
    if not any(e.get('event') == 'build-cache-hit' and e.get('kind') == 'tools' for e in SEEN):
        raise SystemExit('the second managed root did not reuse the cached dependency prefix')
    native_build.check_windows_location(work / 'MAG')
    try:
        opus = Path(managed_tools.opus_prefix(managed, cache=work / 'cache', token=token, events=events, build_cache=store,
                                              toolchain=toolchain))
    except Exception:
        # The stage logs stay on the runner; show the failing one here.
        for log in sorted((Path(managed['root']) / 'logs').glob('*-opus.log')):
            print(f'== {log.name} (last 60 lines)', flush=True)
            print('\n'.join(log.read_text(errors='replace').splitlines()[-60:]), flush=True)
        raise

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
                               '-DPython3_EXECUTABLE=' + posix(managed['python']),
                               # Controllers through the managed SDL2 package, as the player build does.
                               '-DPS3RECOMP_PAD_USE_SDL2=ON', '-DSDL2_DIR=' + posix(managed['sdl2']['cmake_dir'])], work)
    # Windows builds do not use the compile cache (installer/mag_installer/compile_cache.py).
    step('build runtime', [managed['cmake'], '--build', build, '--target', 'ps3recomp_runtime'], work)

    voice = work / 'opus-check'; voice.mkdir(exist_ok=True)
    (voice / 'opus_check.c').write_text(OPUS_CHECK)
    step('link the pinned static Opus (static CRT)', [toolchain['clang_cl'], '/nologo', '/O2', '/MT', '/I' + str(opus / 'include'),
         'opus_check.c', str(opus / 'lib/opus.lib'), '/Fe:opus_check.exe'], voice)
    step('Opus round trip', [voice / 'opus_check.exe'], voice)

    scratch = work / 'supervisor'; scratch.mkdir(exist_ok=True)
    (scratch / 'supervisor_config.h').write_text(
        '#define MAG_GAME_ROOT L"..\\\\game"\n' +
        ''.join(f'#define {n} "{"0" * 64}"\n' for n in ('MAG_LAUNCHER_SHA256', 'MAG_ENGINE_SHA256',
                'MAG_LAUNCHER_ELF_SHA256', 'MAG_ENGINE_ELF_SHA256', 'MAG_ENGINE_SELF_SHA256')))
    (scratch / 'supervisor.c').write_text(native_package_windows._SUPERVISOR)
    native_package_windows.copy_supervisor_sources(ROOT, scratch)  # the account link and settings rules
    native_package_windows._compile(toolchain['clang_cl'], env, scratch, scratch / 'MAG.exe', token)
    # The installer's check of an updated app (installer update.py) never opens a window: without
    # game files it fails on standard error with exit code 1 and writes no launch-error.log. A
    # message box would instead hold this run until its timeout.
    check = subprocess.run([str(scratch / 'MAG.exe'), '--mag-verify-install'], capture_output=True, text=True, timeout=120)
    if check.returncode != 1 or 'game files are missing' not in check.stderr or (work / 'launch-error.log').exists():
        raise SystemExit(f'MAG.exe --mag-verify-install: exit {check.returncode}, stderr {check.stderr!r}')
    print('MAG.exe --mag-verify-install fails without a window, as it should', flush=True)
    print('Windows toolchain route verified (no game data)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
