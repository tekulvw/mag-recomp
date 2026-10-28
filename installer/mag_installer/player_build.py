"""One player action: validate inputs, acquire tools, build, and package MAG.

The recipe is bundled maintainer data. Players never select SDKs or JSON files.
"""
from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path
from contextlib import nullcontext

from .model import InstallerError, host_key
from .pipeline import _exclusive_lock, _write_json, _separate
from .safety import Cancellation, contained, file_digest


PLAYER_HOSTS = {'macos-arm64': 'Apple Silicon Macs', 'windows-x86_64': '64-bit Windows PCs', 'linux-x86_64': '64-bit Linux PCs'}


def _manifest(root):
    try:
        record = json.loads((Path(root) / 'release/player-build.json').read_text(encoding='utf-8'))
        if record.get('schema_version') != 1 or record.get('host') not in PLAYER_HOSTS:
            raise ValueError('unsupported manifest')
        return record
    except (OSError, ValueError, TypeError) as exc:
        raise InstallerError('This installer is missing its maintained MAG build kit. Obtain a complete installer build.') from exc


def readiness(root):
    try:
        record = _manifest(root)
        if host_key() != record['host']:
            raise InstallerError(f'This build of the MAG installer supports {PLAYER_HOSTS[record["host"]]}. Download the installer for this computer.')
        if not record.get('kit'):
            raise InstallerError(record.get('reason') or 'The maintained MAG source kit is missing from this installer.')
        kit = record['kit']
        if 'embedded' in kit:
            path = contained(Path(root), kit['embedded'])
            if not path.is_file() or path.stat().st_size != kit['size']:
                raise InstallerError('The installer’s MAG source kit is missing or incomplete. Obtain a complete installer build.')
        elif not str(kit.get('url', '')).startswith('https://'):
            raise InstallerError('The installer has no supported source-kit download.')
        from .managed_tools import readiness as tools_ready
        tools = tools_ready(root)
        if not tools.get('ready'):
            raise InstallerError(tools.get('reason', 'Managed build tools are unavailable.'))
        return {'ready': True, 'reason': '', 'experimental': True}
    except InstallerError as exc:
        return {'ready': False, 'reason': str(exc)}


def locate_inputs(source):
    """Accept a prepared input folder or its PS3_GAME child, without searching the disk."""
    source = Path(source).expanduser().resolve()
    if not source.is_dir():
        raise InstallerError('Choose the folder containing your MAG game files.')
    disc = source / 'PS3_GAME' if (source / 'PS3_GAME').is_dir() else source
    base = disc.parent if disc.name == 'PS3_GAME' else source
    selected = {
        'disc_root': disc,
        'eboot_elf': disc / 'USRDIR/EBOOT.elf',
        'engine_elf': disc / 'USRDIR/bin/mage_g.elf',
        'firmware': base / 'firmware/libsre.prx',
    }
    required = [disc / 'PARAM.SFO', disc / 'USRDIR/EBOOT.BIN', disc / 'USRDIR/bin/mage_g.self',
                selected['eboot_elf'], selected['engine_elf'], selected['firmware']]
    missing = [path.relative_to(base).as_posix() if path.is_relative_to(base) else str(path)
               for path in required if not path.is_file() or path.is_symlink()]
    if missing:
        raise InstallerError('Your game folder is missing required prepared files: ' + ', '.join(missing) +
                             '. The installer needs the original game plus separately decrypted executables and firmware; it cannot decrypt retail files yet.')
    return {key: str(value) for key, value in selected.items()}


def _load_kit(root, record, destination, cache, token, events):
    from .tools import acquire
    from .managed_tools import extract_managed
    if 'embedded' in record:
        archive = contained(Path(root), record['embedded'])
    else:
        archive = acquire({**record, 'id': 'mag-source-kit'}, cache / 'downloads', token, events)
    size, digest = file_digest(archive, token, record['size'])
    if size != record['size'] or digest != record['sha256']:
        raise InstallerError('The MAG source kit failed verification. Obtain a fresh installer build.')
    extraction = {**record, 'archive': record.get('archive', record.get('format', 'tar.gz')),
                  'max_expanded_bytes': record.get('max_expanded_bytes', record.get('max_expanded_size'))}
    extract_managed(archive, destination, extraction, token=token)
    try:
        metadata = json.loads((destination / 'KIT.json').read_text(encoding='utf-8'))
        kit = {key: metadata[key] for key in ('schema_version', 'source_tree', 'sdks', 'jobs', 'native_graphics')}
        for item in metadata['source_inventory']:
            path = contained(contained(destination, kit['source_tree']), item['path'])
            found_size, found_hash = file_digest(path, token, item['size'])
            if (found_size, found_hash) != (item['size'], item['sha256']):
                raise InstallerError('The native source kit inventory does not match its files.')
        return kit
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise InstallerError('The maintained native source kit metadata is incomplete.') from exc


def _binding(inputs, record, token):
    identities = {}
    disc = Path(inputs['disc_root'])
    files = {**{key: Path(inputs[key]) for key in ('eboot_elf', 'engine_elf', 'firmware')},
             **{name: disc / name for name in ('PARAM.SFO', 'USRDIR/EBOOT.BIN', 'USRDIR/bin/mage_g.self')}}
    for key, path in files.items():
        size, digest = file_digest(path, token)
        identities[key] = {'path': str(path.resolve()), 'size': size, 'sha256': digest}
    return {'host': record['host'], 'kit': record['kit'], 'input_identities': record.get('input_identities'), 'inputs': identities}


def _retry_status(output, binding):
    if output.is_symlink() or not output.is_dir() or (output / 'build.json').is_symlink():
        raise InstallerError('Retry requires an owned incomplete MAG build folder, without symlinks.')
    try:
        status = json.loads((output / 'build.json').read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise InstallerError('Unknown output folder; no matching MAG retry journal. Existing files are preserved.') from exc
    if not isinstance(status, dict) or status.get('schema_version') != 2 or status.get('state') not in ('incomplete', 'building') or status.get('output') != str(output):
        raise InstallerError('Retry requires a matching incomplete build with the same input identities, source kit and host. Existing files are preserved.')
    if status.get('binding') != binding:
        previous = status.get('binding', {})
        # A rejected preflight has never entered tool setup or a build. Permit
        # correction of those same selected files, while retaining the failed
        # attempt evidence. Host/source-kit changes still require a fresh folder.
        compatible = isinstance(previous, dict) and all(previous.get(key) == binding.get(key)
                     for key in ('host', 'kit', 'input_identities'))
        old_inputs = previous.get('inputs', {}) if isinstance(previous, dict) else {}
        same_paths = isinstance(old_inputs, dict) and set(old_inputs) == set(binding['inputs']) and all(
            isinstance(old_inputs[key], dict) and old_inputs[key].get('path') == value['path']
            for key, value in binding['inputs'].items())
        if not (status.get('state') == 'incomplete' and status.get('preflight_rejected') is True and status.get('tools_started') is False and compatible and same_paths):
            raise InstallerError('Retry requires a matching incomplete build with the same input identities, source kit and host. Existing files are preserved.')
    if type(status.get('attempt')) is not int or status['attempt'] < 1 or type(status.get('managed_complete')) is not bool:
        raise InstallerError('Unknown retry journal; existing files are preserved.')
    allowed = {'logs', 'build.json', 'build-kit', 'managed-tools', 'build-settings.json', 'build-work', 'MAG', 'MAG.app', '.build-work.native-lock', 'build.pending', 'build-settings.pending'}
    if any(p.name not in allowed or p.is_symlink() for p in output.iterdir()):
        raise InstallerError('Unknown files or symlinks in retry output; existing files are preserved.')
    # Stage directories are renamed intact, so their legitimate SDK/tool symlinks
    # are never followed. Log destinations, which we write into, must be ordinary.
    if any(p.is_symlink() for p in (output / 'logs').rglob('*')):
        raise InstallerError('Retry logs contain symlinks; existing files are preserved.')
    return status


def _prepare_retry(output, status, token):
    """Retain small diagnostics, then discard only journal-owned scratch roots.

    This is replayable after a power failure: the attempt number changes only
    after all cleanup succeeds, and the original attempt journal is never replaced.
    """
    native_lock = output / '.build-work.native-lock'
    with (_exclusive_lock(native_lock, True) if native_lock.exists() else nullcontext()):
        _clean_retry_scratch(output, status, token)


def _clean_retry_scratch(output, status, token):
    attempt = output / 'logs' / 'attempts' / str(status['attempt'])
    attempt.mkdir(parents=True, exist_ok=True)
    prior = attempt / 'build.json'
    if not prior.exists():
        _write_json(prior, status)
    diagnostics = [('build-settings.json', 'build-settings.json'),
                   ('build-work/native-build.json', 'native-build.json')]
    for stage in ('build', 'build-engine'):
        parent = output / 'build-work/workspace' / stage
        if parent.is_symlink() or not parent.resolve().is_relative_to(output.resolve()):
            continue
        for path in parent.glob('*.receipt.json'):
            diagnostics.append((str(path.relative_to(output)), stage + '-' + path.name))
    for source, target in diagnostics:
        path = output / source
        if path.is_file() and not path.is_symlink():
            shutil.copy2(path, attempt / target)
    stages = ['build-kit', 'build-work', 'MAG', 'MAG.app']
    if not status['managed_complete']:
        stages.append('managed-tools')
    for name in stages:
        token.check()
        stage = output / name
        if not stage.exists():
            continue
        logs = stage / 'logs'
        if logs.is_dir() and not logs.is_symlink():
            if any(path.is_symlink() for path in logs.rglob('*')):
                raise InstallerError('Stage logs contain symlinks; retry cleanup was stopped.')
            shutil.copytree(logs, attempt / name / 'logs', dirs_exist_ok=True)
        # These exact roots are owned scratch recorded by the matching journal;
        # rmtree unlinks SDK symlinks rather than following their targets.
        if stage.is_symlink() or not stage.is_dir():
            raise InstallerError('Retry scratch must be an ordinary owned directory.')
        shutil.rmtree(stage)
    for name in ('build-settings.json', 'build.pending', 'build-settings.pending'):
        path = output / name
        if path.exists():
            path.unlink()


def install(root, source, output, cache, token=None, events=None, retry=False):
    token = token or Cancellation()
    events = events or (lambda event: None)
    root, source, output, cache = (Path(p).expanduser().absolute() for p in (root, source, output, cache))
    state = readiness(root)
    if not state['ready']:
        raise InstallerError(state['reason'])
    if retry and (output.exists() or output.is_symlink()):
        if output.is_symlink() or not output.parent.is_dir() or output.parent.is_symlink():
            raise InstallerError('Retry requires an ordinary output folder and parent, without symlinks.')
        source_real, output_real, cache_real = source.resolve(), output.resolve(), cache.resolve()
        for writable in (output_real, cache_real):
            if writable.is_relative_to(source_real) or source_real.is_relative_to(writable):
                raise InstallerError('Output and cache must be separate from the owned input tree')
        if cache_real.is_relative_to(output_real) or output_real.is_relative_to(cache_real):
            raise InstallerError('Output and cache must be separate directories')
    else:
        _separate(source, output, cache)
    token.check()
    events({'event': 'stage-start', 'stage': 'validate', 'message': 'Checking your game files'})
    inputs = locate_inputs(source)
    record = _manifest(root)
    host = record.get('host', host_key())
    windows = host == 'windows-x86_64'
    linux = host == 'linux-x86_64'
    binding = _binding(inputs, record, token)
    # The OS vendor owns the compiler/SDK installation; settle it before downloads.
    from .managed_tools import bootstrap, ensure_apple_sdk, ensure_windows_build_tools
    from .native_build import install as build_native, _verify_identities, check_windows_location
    from .native_package import package
    if windows:
        check_windows_location(output)
    with _exclusive_lock(output.with_name('.' + output.name + '.player-lock'), retry, events):
        if output.exists() or output.is_symlink():
            if not retry:
                raise InstallerError('Output already exists. Use Retry build or build --retry for a matching failed build.')
            status = _retry_status(output, binding)
            recovering = True
        else:
            recovering = False
            output.mkdir(mode=0o700)
            status = {'schema_version': 2, 'state': 'building', 'output': str(output),
                      'binding': binding, 'attempt': 1, 'managed_complete': False, 'tools_started': False}
        logs = output / 'logs'; logs.mkdir(exist_ok=True)
        journal = output / 'build.json'
        retry_prepared = False
        checking_inputs = False
        try:
            if recovering:
                _prepare_retry(output, status, token)
                status.update(state='building', attempt=status['attempt'] + 1, binding=binding)
                retry_prepared = True
            _write_json(journal, status)
            checking_inputs = True
            events({'event': 'stage-start', 'stage': 'prepare-source', 'logs': str(logs)})
            kit_root = output / 'build-kit'
            if 'embedded' not in record['kit'] and record['kit']:
                from .native_build import _verify_hash
                identities = record.get('input_identities')
                if not isinstance(identities, dict) or set(identities) != set(binding['inputs']):
                    raise InstallerError('Remote source kits require bound input_identities in the maintained installer manifest before downloading. Obtain a complete embedded installer.')
                for key, identity in binding['inputs'].items():
                    _verify_hash(identity['path'], identities[key], token)
            kit = _load_kit(root, record['kit'], kit_root, cache, token, events)
            source_tree = contained(kit_root, kit['source_tree'])
            # Hash-check prepared inputs before fetching or compiling tools.
            _verify_identities(source_tree, Path(inputs['disc_root']), inputs, token)
            checking_inputs = False
            status['preflight_rejected'] = False
            status['tools_started'] = True
            _write_json(journal, status)
            if windows:
                toolchain = ensure_windows_build_tools(cache=cache, token=token, events=events)
            elif linux:
                from .managed_tools import ensure_linux_build_tools
                toolchain = ensure_linux_build_tools(cache=cache, token=token, events=events)
            else:
                ensure_apple_sdk(token=token, events=events)
            events({'event': 'stage-start', 'stage': 'acquire', 'logs': str(output / 'managed-tools/logs')})
            managed = bootstrap(output / 'managed-tools', cache=cache, token=token, events=events)
            status['managed_complete'] = True
            _write_json(journal, status)
            toolchain_fields = ({'msvc': {key: toolchain[key] for key in ('vcvars', 'clang_cl', 'installation', 'version', 'product')}}
                                if windows else {'ffmpeg': managed['ffmpeg']})
            config = {'schema_version': 1, 'source_tree': str(source_tree), **inputs,
                      **{key: managed[key] for key in ('python', 'cmake', 'ninja', 'git')}, **toolchain_fields,
                      'jobs': 2, 'native_graphics': False,
                      **({'host': host, 'linux_toolchain': {key: toolchain[key] for key in ('cc', 'cxx')}} if linux else {}),
                      'sdks': {role: {'path': str(contained(kit_root, item['path'])), 'revision': item['revision']}
                               for role, item in kit['sdks'].items()}}
            if managed.get('prefix'):
                config['managed_prefix'] = str(managed['prefix'])
            config_path = output / 'build-settings.json'
            _write_json(config_path, config)
            native_output = output / 'build-work'
            def progress(event):
                if event.get('event') == 'experimental-local-build-complete':
                    return
                events(event)
            events({'event': 'stage-start', 'stage': 'build', 'logs': str(native_output / 'logs')})
            build_native(config_path, native_output, token=token, events=progress, **({'retry_lock': True} if recovering else {}))
            events({'event': 'stage-start', 'stage': 'package', 'logs': str(native_output / 'logs')})
            app = package(config, native_output / 'workspace', output / ('MAG' if windows or linux else 'MAG.app'), token=token, events=progress)
            token.check()
            status.update(state='complete', app=str(app), logs=str(native_output / 'logs'))
            _write_json(journal, status)
            done = ('MAG was built. Open the installation folder and start MAG\\MAG.exe.' if windows
                    else ('MAG was built. Open the installation folder and start MAG/MAG.' if linux
                          else 'MAG.app was built. Open the installation folder to use it.'))
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(app),
                    'logs': str(native_output / 'logs'), 'message': done})
            return output
        except BaseException as exc:
            status['state'] = 'incomplete'
            if checking_inputs:
                status['preflight_rejected'] = True
            if recovering and not retry_prepared:
                status.setdefault('error', str(exc))
                status['retry_error'] = str(exc)
            else:
                status['error'] = str(exc)
            try:
                _write_json(journal, status)
            except OSError:
                pass  # Preserve the original build/cancellation error.
            raise
