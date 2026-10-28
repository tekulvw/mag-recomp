"""Build the managed Linux dependency graph and pinned Vulkan runtime without game data.

Uses the same Linux prerequisite probe, verified Python/CMake/Ninja downloads,
Python wheel closure, source dependency build plan, build environment and runtime
options as Build MAG. CI must provision LINUX_PACKAGES before this check: it
never offers an interactive package installation or reads player assets.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'installer'))

from mag_installer import managed_tools, native_build  # noqa: E402
from mag_installer.model import InstallerError  # noqa: E402
from mag_installer.safety import Cancellation  # noqa: E402


def events(event):
    if event.get('event') not in ('stage-activity', 'download'):
        print(json.dumps(event), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--runtime-sdk', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    sdk, work = args.runtime_sdk.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    token = Cancellation()

    # CI is noninteractive: fail with the concrete prerequisite list instead of
    # opening PolicyKit. The explicit Build MAG action owns installation UI.
    toolchain, detail = managed_tools.linux_toolchain_probe()
    if not toolchain: raise InstallerError(detail)
    print('toolchain:', json.dumps(toolchain), flush=True)
    managed = managed_tools.bootstrap(work / 'managed', cache=work / 'cache', token=token,
                                      events=events, build_dependencies=True)
    if not managed['dependencies_ready']: raise InstallerError('Managed dependency build was skipped')
    config = {**{key: managed[key] for key in ('python', 'cmake', 'ninja', 'git', 'managed_prefix', 'linux_toolchain')},
              'host': 'linux-x86_64', 'jobs': 2}
    home = work / 'home'; home.mkdir(exist_ok=True)
    env = native_build._environment(config, home)

    def step(name, argv):
        print('== ' + name, flush=True)
        print('+', ' '.join(map(str, argv)), flush=True)
        subprocess.run([str(value) for value in argv], cwd=work, env=env, check=True)

    step('managed Python wheel closure', [managed['python'], '-c',
         'import Crypto,capstone,construct,tabulate,tqdm; print("managed Python modules verified")'])
    step('system compiler', [toolchain['cxx'], '--version'])
    step('Git', [managed['git'], '--version'])
    wrapper = work / 'runtime-project'; wrapper.mkdir(exist_ok=True)
    (wrapper / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(linux_runtime_check C CXX)\n'
        f'add_subdirectory("{sdk.as_posix()}" sdk EXCLUDE_FROM_ALL)\n'
        f'include("{(ROOT / "cmake/MagLinuxCompat.cmake").as_posix()}")\n'
        f'mag_linux_compat_runtime("{sdk.as_posix()}")\n')
    build = work / 'runtime-build'
    prefix = Path(managed['prefix'])
    step('configure pinned runtime with real Linux Vulkan and FFmpeg', [
        managed['cmake'], '-S', wrapper, '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCMAKE_C_COMPILER=' + toolchain['cc'], '-DCMAKE_CXX_COMPILER=' + toolchain['cxx'],
        '-DCMAKE_MAKE_PROGRAM=' + managed['ninja'], '-DPython3_EXECUTABLE=' + managed['python'],
        '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DPS3RECOMP_RSX_VULKAN=ON',
        '-DPS3RECOMP_FFMPEG_RUNTIME=ON', '-DPS3RECOMP_FFMPEG_INCLUDE_DIR=' + managed['ffmpeg']['include']])
    # Check generated compile commands before claiming the enabled flags were
    # consumed. The runtime's Vulkan unit and translator must both be compiled.
    commands = json.loads((build / 'compile_commands.json').read_text())
    vulkan = [entry for entry in commands if Path(entry['file']).name == 'rsx_vulkan_backend.c']
    shaders = [entry for entry in commands if Path(entry['file']).name == 'rsx_shader_spirv.cpp']
    if not vulkan or not shaders:
        raise InstallerError('Pinned runtime did not configure the Linux Vulkan source units')
    command_text = '\n'.join(entry.get('command', ' '.join(entry.get('arguments', []))) for entry in commands)
    for define in ('PS3RECOMP_RSX_VULKAN', 'PS3RECOMP_FFMPEG_RUNTIME', 'PS3RECOMP_HAVE_SPIRV_TRANSLATION'):
        if define not in command_text:
            raise InstallerError('Pinned runtime did not enable ' + define)
    step('build pinned runtime', [managed['cmake'], '--build', build, '--target', 'ps3recomp_runtime', '--parallel', '2'])
    # Assert the dependency receipts exist; bootstrap rechecks their exact file
    # inventory on every later invocation, including a retry.
    if not (prefix / 'MANAGED-DEPENDENCIES.json').is_file():
        raise InstallerError('Managed dependencies have no completed identity receipt')
    print('Linux managed toolchain and Vulkan runtime build verified (no game data)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
