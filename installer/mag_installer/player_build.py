"""One player action: validate inputs, acquire tools, build, and package MAG.

The recipe is bundled maintainer data. Players never select SDKs or JSON files.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .model import InstallerError, host_key
from .pipeline import _exclusive_lock, _write_json, _separate
from .safety import Cancellation, contained, file_digest


PLAYER_HOSTS = {'macos-arm64': 'Apple Silicon Macs', 'windows-x86_64': '64-bit Windows PCs'}


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


def install(root, source, output, cache, token=None, events=None):
    token = token or Cancellation()
    events = events or (lambda event: None)
    root, source, output, cache = (Path(p).expanduser().absolute() for p in (root, source, output, cache))
    state = readiness(root)
    if not state['ready']:
        raise InstallerError(state['reason'])
    _separate(source, output, cache)
    token.check()
    events({'event': 'stage-start', 'stage': 'validate', 'message': 'Checking your game files'})
    inputs = locate_inputs(source)
    record = _manifest(root)
    windows = record.get('host', host_key()) == 'windows-x86_64'
    # The OS vendor owns the compiler/SDK installation; settle it before downloads.
    from .managed_tools import bootstrap, ensure_apple_sdk, ensure_windows_build_tools
    from .native_build import install as build_native, _verify_identities, check_windows_location
    from .native_package import package
    if windows:
        check_windows_location(output)
        toolchain = ensure_windows_build_tools(cache=cache, token=token, events=events)
    else:
        ensure_apple_sdk(token=token, events=events)
    with _exclusive_lock(output.with_name('.' + output.name + '.player-lock'), False, events):
        output.mkdir(mode=0o700)
        logs = output / 'logs'; logs.mkdir()
        journal = output / 'build.json'
        status = {'schema_version': 1, 'state': 'building', 'output': str(output)}
        _write_json(journal, status)
        try:
            events({'event': 'stage-start', 'stage': 'prepare-source', 'logs': str(logs)})
            kit_root = output / 'build-kit'
            kit = _load_kit(root, record['kit'], kit_root, cache, token, events)
            source_tree = contained(kit_root, kit['source_tree'])
            # Hash-check prepared inputs before fetching or compiling tools.
            _verify_identities(source_tree, Path(inputs['disc_root']), inputs, token)
            events({'event': 'stage-start', 'stage': 'acquire', 'logs': str(output / 'managed-tools/logs')})
            managed = bootstrap(output / 'managed-tools', cache=cache, token=token, events=events)
            toolchain_fields = ({'msvc': {key: toolchain[key] for key in ('vcvars', 'clang_cl', 'installation', 'version', 'product')}}
                                if windows else {'ffmpeg': managed['ffmpeg']})
            config = {'schema_version': 1, 'source_tree': str(source_tree), **inputs,
                      **{key: managed[key] for key in ('python', 'cmake', 'ninja', 'git')}, **toolchain_fields,
                      'jobs': 2, 'native_graphics': False,
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
            build_native(config_path, native_output, token=token, events=progress)
            events({'event': 'stage-start', 'stage': 'package', 'logs': str(native_output / 'logs')})
            app = package(config, native_output / 'workspace', output / ('MAG' if windows else 'MAG.app'), token=token, events=progress)
            token.check()
            status.update(state='complete', app=str(app), logs=str(native_output / 'logs'))
            _write_json(journal, status)
            done = ('MAG was built. Open the installation folder and start MAG\\MAG.exe.' if windows
                    else 'MAG.app was built. Open the installation folder to use it.')
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(app),
                    'logs': str(native_output / 'logs'), 'message': done})
            return output
        except BaseException as exc:
            status.update(state='incomplete', error=str(exc))
            _write_json(journal, status)
            raise
