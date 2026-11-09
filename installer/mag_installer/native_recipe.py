"""The BUILDING.md recipe as explicit commands: prepare, inventory, lift, build.

The four ps3recomp checkouts are the roles pinned in profiles/framework.json;
each is cloned to its pinned repository-relative path inside the private
workspace, where the profiles and tools expect it. The caller checks host,
Python, SDK cleanliness/revisions and the toolchain before running anything.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re

from .model import InstallerError
from .safety import contained

# Roles of the ps3recomp table in profiles/framework.json.
ROLES = ('runtime', 'lifter-spu', 'lifter-ppu', 'spu-programs')
_REQUIRED = {'schema_version', 'source_tree', 'disc_root', 'eboot_elf', 'engine_elf',
             'firmware', 'python', 'cmake', 'ninja', 'git', 'sdks'}
# ffmpeg: required on macOS/Linux. msvc/linux_toolchain: host compiler records.
# game_controls: opt-in keyboard and mouse controls (docs/native-controls.md), with the
# pinned Rust toolchain's cargo and rustc executables (not rustup proxies).
_OPTIONAL = {'jobs', 'cmake_args', 'native_graphics', 'managed_prefix', 'ffmpeg', 'msvc', 'host', 'linux_toolchain',
             'game_controls'}


def is_windows(config):
    return 'msvc' in config


def _object(value, required, optional, label):
    if not isinstance(value, dict) or set(value) - required - optional or required - set(value):
        raise InstallerError(f'{label} requires {", ".join(sorted(required))}; unknown fields are unsupported')


def _path(value, base, label, directory=False, executable=False, dependency=False, keep_name=False):
    if not isinstance(value, str) or not value or '\x00' in value:
        raise InstallerError(f'{label} must name an existing local path')
    path = Path(value).expanduser()
    if not path.is_absolute(): path = base / path
    path = path.absolute()
    if (path.is_symlink() and not (executable or dependency)) or not (path.is_dir() if directory else path.is_file()):
        raise InstallerError(f'{label} must name an existing ordinary {"directory" if directory else "file"}: {path}')
    if executable and not os.access(path, os.X_OK):
        raise InstallerError(f'{label} must be executable: {path}')
    # A compiler driver picks its language from the name it is run as: Ubuntu's
    # /usr/bin/clang++ resolves to llvm-18/bin/clang, a C driver that links without
    # the C++ runtime defaults (libstdc++, libm). Keep such paths unresolved.
    return str(path) if keep_name else str(path.resolve())


def load_config(path):
    """Load strict local configuration; resolve relative paths beside the file.

    ffmpeg explicitly supplies include plus avcodec/avformat/avutil/swscale paths.
    SDK revisions are checked against profiles/framework.json by preflight.
    This performs no execution.
    """
    path = Path(path).absolute()
    try: raw = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc: raise InstallerError(f'Cannot read native build configuration: {path}') from exc
    _object(raw, _REQUIRED, _OPTIONAL, 'Native build configuration (including ffmpeg)')
    if type(raw['schema_version']) is not int or raw['schema_version'] != 1:
        raise InstallerError('Native build configuration schema_version must be 1')
    result = dict(raw)
    if raw.get('host') not in (None, 'macos-arm64', 'windows-x86_64', 'linux-x86_64'):
        raise InstallerError('Unsupported native configuration host')
    if raw.get('host') == 'linux-x86_64':
        if 'msvc' in raw or 'linux_toolchain' not in raw:
            raise InstallerError('Linux configuration requires linux_toolchain')
        _object(raw['linux_toolchain'], {'cc', 'cxx'}, set(), 'linux_toolchain')
        result['linux_toolchain'] = {key: _path(raw['linux_toolchain'][key], path.parent, 'linux_toolchain.' + key, executable=True, keep_name=True) for key in ('cc', 'cxx')}
    elif 'linux_toolchain' in raw:
        raise InstallerError('linux_toolchain requires host=linux-x86_64')
    for key in ('source_tree', 'disc_root'):
        result[key] = _path(raw[key], path.parent, key, directory=True)
    for key in ('eboot_elf', 'engine_elf', 'firmware'):
        result[key] = _path(raw[key], path.parent, key)
    for key in ('python', 'cmake', 'ninja', 'git'):
        result[key] = _path(raw[key], path.parent, key, executable=True)
    _object(raw['sdks'], set(ROLES), set(), 'sdks')
    result['sdks'] = {}
    for role in ROLES:
        sdk = raw['sdks'][role]
        _object(sdk, {'path', 'revision'}, set(), f'sdks.{role}')
        revision = sdk['revision']
        if not isinstance(revision, str) or re.fullmatch('[0-9a-f]{40}', revision) is None:
            raise InstallerError(f'sdks.{role}.revision must be an exact lowercase 40-character Git SHA')
        result['sdks'][role] = {'path': _path(sdk['path'], path.parent, f'sdks.{role}.path', directory=True),
                                'revision': revision}
    if 'msvc' in raw:
        _object(raw['msvc'], {'vcvars', 'clang_cl'}, {'installation', 'version', 'product'}, 'msvc')
        result['msvc'] = {**raw['msvc'],
                          'vcvars': _path(raw['msvc']['vcvars'], path.parent, 'msvc.vcvars'),
                          'clang_cl': _path(raw['msvc']['clang_cl'], path.parent, 'msvc.clang_cl', executable=True)}
    elif 'ffmpeg' not in raw:
        raise InstallerError('Native build configuration requires ffmpeg on macOS and Linux')
    if 'ffmpeg' in raw:   # optional on Windows: the movie decoder, from the pinned prebuilt
        fields = {'include', 'avcodec', 'avformat', 'avutil', 'swscale'}
        _object(raw['ffmpeg'], fields, set(), 'ffmpeg')
        result['ffmpeg'] = {key: _path(raw['ffmpeg'][key], path.parent, f'ffmpeg.{key}', directory=key == 'include', dependency=True) for key in fields}
    jobs = raw.get('jobs', 2)
    if type(jobs) is not int or not 1 <= jobs <= 4:
        raise InstallerError('jobs must be an integer from 1 to 4')
    result['jobs'] = jobs
    if type(raw.get('native_graphics', False)) is not bool or raw.get('native_graphics', False):
        raise InstallerError('native_graphics must be false')
    result['native_graphics'] = False
    arguments = raw.get('cmake_args', [])
    # Arbitrary CMake -D options can inject executable hooks/toolchains or defeat
    # provenance. Keep this optional tuning surface deliberately narrow.
    if not isinstance(arguments, list) or any(not isinstance(a, str) or re.fullmatch(
            r'-D(?:MAG_GENERATED_PPU_OPT=[012]|MAG_X86_LEVEL=(?:auto|v4|v3|baseline)'
            r'|CMAKE_OSX_DEPLOYMENT_TARGET=[0-9]+(?:\.[0-9]+){0,2})', a) is None for a in arguments):
        raise InstallerError('cmake_args supports only MAG_GENERATED_PPU_OPT=0|1|2, MAG_X86_LEVEL=auto|v4|v3|baseline '
                             'and numeric CMAKE_OSX_DEPLOYMENT_TARGET')
    result['cmake_args'] = list(arguments)
    if raw.get('managed_prefix'):
        result['managed_prefix'] = _path(raw['managed_prefix'], path.parent, 'managed_prefix', directory=True)
    if 'game_controls' in raw:
        _object(raw['game_controls'], {'cargo', 'rustc'}, set(), 'game_controls')
        result['game_controls'] = {key: _path(raw['game_controls'][key], path.parent, 'game_controls.' + key, executable=True)
                                   for key in ('cargo', 'rustc')}
    return result



def x86_level_override(environ, host):
    """The cmake_args of a MAG_X86_LEVEL troubleshooting override; [] when it is unset.

    Player builds pass nothing: CMake's default (auto) uses x86-64-v4 or v3 when the build CPU has it.
    Setting MAG_X86_LEVEL=baseline (or v3, v4 or auto) in the installer's environment forces a level
    on Windows and Linux. The build environment itself never inherits MAG_* variables.
    """
    value = environ.get('MAG_X86_LEVEL', '')
    if not value:
        return []
    if host not in ('windows-x86_64', 'linux-x86_64'):
        raise InstallerError('MAG_X86_LEVEL applies only to Windows and Linux builds; unset it')
    if value not in ('auto', 'v4', 'v3', 'baseline'):
        raise InstallerError(f'MAG_X86_LEVEL must be auto, v4, v3 or baseline, not {value!r}')
    return ['-DMAG_X86_LEVEL=' + value]


def splice_pins(pin):
    """A role's extra tagged commits (the fixed lifters the PPU splices read), as a list.

    `splice_after` is one {tag, commit} or a list of them; a missing key is none."""
    extra = pin.get('splice_after')
    if extra is None:
        return []
    if isinstance(extra, dict):
        return [extra]
    if isinstance(extra, list) and extra:
        return extra
    raise ValueError('splice_after')


def framework_pins(workspace):
    """The pinned ps3recomp table: {role: {tag, commit, path}} (read-only)."""
    try:
        table = json.loads(contained(Path(workspace), 'profiles/framework.json').read_text(encoding='utf-8'))['ps3recomp']
        if set(table) != set(ROLES): raise ValueError('roles')
        for role, pin in table.items():
            if (re.fullmatch('[0-9a-f]{40}', str(pin.get('commit', ''))) is None
                    or not isinstance(pin.get('tag'), str) or not isinstance(pin.get('path'), str)):
                raise ValueError(role)
            for extra in splice_pins(pin):
                if (not isinstance(extra, dict) or set(extra) != {'tag', 'commit'}
                        or re.fullmatch('[0-9a-f]{40}', str(extra.get('commit', ''))) is None
                        or not str(extra.get('tag', '')).startswith('mag-recomp/')):
                    raise ValueError(role)
            contained(Path(workspace), pin['path'])
        return table
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise InstallerError('profiles/framework.json has no complete ps3recomp pin table') from exc


REQUIRED_SOURCE_FILES = (
    'CMakeLists.txt', 'cmake/MagWin32Posix.cmake',
    'tools/prepare_inputs.py', 'tools/inventory.py', 'tools/lift.py',
    'tools/spu_lift.py', 'tools/engine_spu_lift.py', 'tools/firmware_lift.py',
    'tools/spu_programs_lift.py', 'tools/provenance.py',
    'tools/wws_job_lift.py', 'tools/verified_spu_jobs.py',
    'tools/movie_picture_hooks.py', 'tools/graphics_trace_hooks.py',
    'tools/installer_ppu_splice.py', 'tools/ppu_function_splice.py',
    'profiles/retail-v1.0-ppu-jumptable-splice.json', 'profiles/retail-v1.0-ppu-ctr64-splice.json',
    'profiles/framework.json', 'profiles/retail-v1.0-spu.json',
    'profiles/retail-v1.0-engine-spu.json', 'profiles/retail-v1.0-firmware.json',
    'profiles/retail-v1.0-engine-modules.json', 'profiles/retail-v1.0-spu-programs.json',
    'profiles/retail-v1.0-spu-coverage.json', 'profiles/retail-v1.0-EBOOT-reservations.json',
    'profiles/retail-v1.0-nonlocal.json',
)


def validate_source_tree(workspace):
    """Fail early for missing recipe inputs, without reading private assets."""
    for name in REQUIRED_SOURCE_FILES:
        if not contained(workspace, name).is_file():
            raise InstallerError(f'Native source tree is incomplete: missing {name}')
    # The PPU configurations are named by their reviewed profile and must remain
    # beside that profile. Check their bytes before expensive disc copying.
    for name in ('retail-v1.0-EBOOT-reservations.json', 'retail-v1.0-nonlocal.json'):
        try:
            profile = json.loads(contained(workspace, 'profiles/' + name).read_text(encoding='utf-8'))
            configuration = profile['configuration']
            if not isinstance(configuration, str) or Path(configuration).name != configuration:
                raise InstallerError('Native nonlocal configuration must be a profile-adjacent basename')
            path = contained(workspace, 'profiles/' + configuration)
            if hashlib.sha256(path.read_bytes()).hexdigest() != profile['configuration_sha256']:
                raise InstallerError('Native nonlocal configuration identity mismatch: ' + name)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            raise InstallerError('Native nonlocal configuration is missing or incompatible: ' + name) from exc
    return framework_pins(workspace)


def build_plan(config, workspace):
    """Return explicit argv only; the caller runs it in the owned source workspace.

    The SDK checkouts already sit at their pinned paths inside the workspace,
    and the copied disc already contains the supplied decrypted ELFs.
    """
    workspace = Path(workspace).absolute()
    pins = framework_pins(workspace)
    sdk = {role: workspace / pin['path'] for role, pin in pins.items()}
    python, cmake = config['python'], config['cmake']
    generated = workspace / 'generated/retail-v1.0'
    paths = {'launcher': generated / 'EBOOT', 'engine': generated / 'mage_g', 'worker-spu': generated / 'spu-eboot',
             'engine-spu': generated / 'spu-engine', 'firmware': generated / 'libsre',
             'spu-programs': workspace / 'local/spu-programs'}
    plan = []
    def command(name, *argv): plan.append({'id': name, 'argv': [str(a) for a in argv]})
    def script(name, tool, *args): command(name, python, workspace / 'tools' / tool, *args)
    disc = workspace / 'local/retail-v1.0/PS3_GAME/USRDIR'
    script('prepare-inputs', 'prepare_inputs.py', '--source-root', disc, '--local-root', disc)
    script('inventory', 'inventory.py', '--sdk', sdk['lifter-spu'])
    for name, image, profile in [('launcher', 'EBOOT', 'retail-v1.0-EBOOT-reservations.json'),
                                  ('engine', 'mage_g', 'retail-v1.0-nonlocal.json')]:
        script('lift-' + name, 'lift.py', '--image', image, '--sdk', sdk['lifter-ppu'],
               '--nonlocal-profile', workspace / 'profiles' / profile, '--output', paths[name])
    # Single-function lifter fixes, chained in order, each reading the previous output.
    # Each after lifter is a tagged fix on the pinned PPU lifter, fetched with it.
    for step, output, work, profile in (
            ('splice-engine-dispatcher', 'mage_g-dispatcher', 'dispatcher-splice', 'retail-v1.0-ppu-jumptable-splice.json'),
            # Full-width CTR in the audio command builder: without it gameplay audio is silent.
            ('splice-engine-ctr64', 'mage_g-ctr64', 'ctr64-splice', 'retail-v1.0-ppu-ctr64-splice.json')):
        engine_base = paths['engine']
        paths['engine'] = generated / output
        script(step, 'installer_ppu_splice.py',
               '--base', engine_base, '--output', paths['engine'],
               '--work', workspace.parent / 'logs' / work,
               '--elf', disc / 'bin/mage_g.elf', '--inventory', workspace / 'analysis/retail-v1.0/mage_g',
               '--nonlocal-profile', workspace / 'profiles/retail-v1.0-nonlocal.json',
               '--before-sdk', sdk['lifter-ppu'], '--after-sdk', sdk['lifter-ppu'],
               '--profile', workspace / 'profiles' / profile)
    controls = config.get('game_controls')
    if controls:
        # Opt-in keyboard and mouse controls: a copy of the engine lift (after every splice)
        # with the controls seams.
        spliced = paths['engine']
        paths['engine'] = generated / 'mage_g-controls'
        script('prepare-controls-hooks', 'render_hooks/prepare.py', '--base', spliced,
               '--output', paths['engine'], '--stage', 'controls')
    script('worker-spu', 'spu_lift.py', '--output', paths['worker-spu'])
    script('engine-spu', 'engine_spu_lift.py', '--output', paths['engine-spu'])
    # The policy/task lift alone leaves streamed render jobs interpreted.
    # Generate and register every discovered WWS module before configuring CMake.
    # --static-indirect also lifts the code each job reaches only through
    # indirect branches; bounded by direct branches alone, that code ran in the
    # SPU interpreter (most of the workers' interpreted time in Training).
    engine_spu_base = paths['engine-spu']
    paths['engine-spu'] = generated / 'spu-engine-retained'
    script('streamed-jobs', 'wws_job_lift.py',
           '--engine-elf', disc / 'bin/mage_g.elf',
           '--engine-profile', workspace / 'profiles/retail-v1.0-engine-spu.json',
           '--sdk', sdk['runtime'], '--sdk-revision', pins['runtime']['commit'],
           '--baseline', engine_spu_base, '--output', paths['engine-spu'],
           '--merge-chunks', '--require-complete', '--static-indirect')
    script('firmware', 'firmware_lift.py', '--output', paths['firmware'])
    script('spu-programs', 'spu_programs_lift.py', '--sdk', sdk['spu-programs'],
           '--sdk-revision', pins['spu-programs']['commit'], '--output', paths['spu-programs'])
    if is_windows(config):
        # CMake reads backslashes in -D values as escapes in later string use.
        def cmake_path(value): return Path(value).as_posix()
        clang_cl = cmake_path(config['msvc']['clang_cl'])
        common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                  '-DCMAKE_C_COMPILER=' + clang_cl, '-DCMAKE_CXX_COMPILER=' + clang_cl,
                  # Static CRT: the packaged executables need no VC++ redistributable.
                  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded']
        if config.get('managed_prefix'):
            # The player presents through the runtime's Vulkan backend: Vulkan-Headers and
            # glslang come from the managed prefix (glslang through CMAKE_PREFIX_PATH below).
            common += ['-DPS3RECOMP_RSX_VULKAN=ON',
                       '-DPS3RECOMP_VULKAN_INCLUDE_DIR=' + cmake_path(Path(config['managed_prefix']) / 'include')]
        if config.get('ffmpeg'):
            # Movies: the same decoder options as Linux, against the prebuilt's MSVC import libraries.
            ffmpeg = config['ffmpeg']
            common += ['-DPS3RECOMP_SAIL_FFMPEG=ON', '-DPS3RECOMP_FFMPEG_RUNTIME=ON',
                       '-DPS3RECOMP_FFMPEG_INCLUDE_DIR=' + cmake_path(ffmpeg['include'])]
            for name in ('avcodec', 'avformat', 'avutil', 'swscale'):
                common.append('-DPS3RECOMP_FFMPEG_' + name.upper() + '_LIBRARY=' + cmake_path(ffmpeg[name]))
    else:
        def cmake_path(value): return str(value)
        ffmpeg = config['ffmpeg']
        common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                  '-DPS3RECOMP_SAIL_FFMPEG=ON', '-DPS3RECOMP_FFMPEG_INCLUDE_DIR=' + ffmpeg['include']]
        for name in ('avcodec', 'avformat', 'avutil', 'swscale'):
            common.append('-DPS3RECOMP_FFMPEG_' + name.upper() + '_LIBRARY=' + ffmpeg[name])
    if config.get('host') == 'linux-x86_64':
        common += ['-DPS3RECOMP_RSX_VULKAN=ON', '-DPS3RECOMP_FFMPEG_RUNTIME=ON',
                   '-DCMAKE_C_COMPILER=' + config['linux_toolchain']['cc'],
                   '-DCMAKE_CXX_COMPILER=' + config['linux_toolchain']['cxx']]
    common += ['-DPS3RECOMP_DIR=' + cmake_path(sdk['runtime']), '-DPython3_EXECUTABLE=' + cmake_path(python),
               '-DCMAKE_MAKE_PROGRAM=' + cmake_path(config['ninja']), '-DMAG_FIRMWARE_DIR=' + cmake_path(paths['firmware'])]
    common += config.get('cmake_args', [])
    if config.get('managed_prefix'):
        common.append('-DCMAKE_PREFIX_PATH=' + cmake_path(config['managed_prefix']))
    for name, image, spu, build in [('launcher', 'EBOOT', 'worker-spu', 'build'),
                                    ('engine', 'mage_g', 'engine-spu', 'build-engine')]:
        extra = ['-DMAG_SPU_PROGRAMS_DIR=' + cmake_path(paths['spu-programs'])] if name == 'engine' else []
        if name == 'engine' and controls:
            extra += ['-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON',
                      '-DMAG_INPUT_CARGO=' + cmake_path(controls['cargo']), '-DMAG_INPUT_RUSTC=' + cmake_path(controls['rustc'])]
        command('configure-' + name, cmake, '-S', workspace, '-B', workspace / build, *common,
                '-DMAG_IMAGE=' + image, '-DRECOMP_DIR=' + cmake_path(paths[name]), '-DSPU_DIR=' + cmake_path(paths[spu]), *extra)
        command('build-' + name, cmake, '--build', workspace / build, '--target', 'MagRecomp', '--parallel', config.get('jobs', 2))
    # CMake's POST_BUILD provenance command writes adjacent .receipt.json files;
    # no standalone fabricated receipt is produced by the installer.
    return plan
