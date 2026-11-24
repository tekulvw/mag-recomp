"""One player action: validate inputs, acquire tools, build, and package MAG.

The recipe is bundled maintainer data. Players never select SDKs or JSON files.

Layout, in the installation location the player chooses:

- ``MAG``: the self-contained MAG folder: the app (``MAG.app``, or ``MAG/`` holding ``MAG.exe`` or
  ``MAG``), the game files it reads (``game/``) and, once played, its saves (``home/``) and launch
  logs (``logs/``). Nothing in it refers to the build folder; it can be moved or copied as a whole.
- ``MAG-build``: everything the build needs and leaves: its journal (``build.json``), settings,
  source kit, managed tools, workspace and build logs. A retry reuses it; once MAG works the player
  may delete it to reclaim space.

Installers before this layout kept the build files inside the MAG folder. The next Build MAG
moves them out: a failed build's files become the build folder, ready for Retry build; a
finished build is packaged again from its finished binaries without rebuilding (`_migrate`).
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
from pathlib import Path
from contextlib import nullcontext

from .model import InstallerError, host_key
from .native_build import FIRMWARE_SOURCE_STEPS
from .native_recipe import x86_level_override
from .pipeline import _exclusive_lock, _write_json
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
                             '. The installer needs the original game plus separately decrypted executables and firmware; it cannot decrypt retail files yet.' +
                             ('\n' + FIRMWARE_SOURCE_STEPS if 'firmware/libsre.prx' in missing else ''))
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


BUILD_SUFFIX = '-build'
# What a build folder may hold. MAG and MAG.app are package stages of a failed build an
# earlier installer made inside its MAG folder (adopted by _adopt_failed_legacy).
BUILD_ENTRIES = {'logs', 'build.json', 'build-kit', 'managed-tools', 'build-settings.json', 'build-work', 'package',
                 'MAG', 'MAG.app', '.build-work.native-lock', 'build.pending', 'build-settings.pending'}
BUILD_STAGES = ('build-kit', 'build-work', 'package', 'MAG', 'MAG.app')
# A finished MAG folder of the earlier layout: the build folder's entries plus the player's own.
LEGACY_ENTRIES = BUILD_ENTRIES | {'home', 'launch-error.log'}
# The deepest build path below the installation location on Windows: <MAG>-build\build-work.
WINDOWS_BUILD_RESERVE = len(BUILD_SUFFIX + '\\build-work')


def build_folder(output):
    """The build folder beside a MAG folder."""
    output = Path(output)
    return output.with_name(output.name + BUILD_SUFFIX)


def app_name(host):
    return 'MAG.app' if host == 'macos-arm64' else 'MAG'


def _read_journal(path):
    if path.is_symlink() or not path.is_file():
        return None
    try:
        status = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return status if isinstance(status, dict) and status.get('schema_version') == 2 else None


def legacy_journal(output):
    """The journal of a MAG folder that an earlier installer built with its build files inside it."""
    output = Path(output)
    if output.is_symlink() or not output.is_dir():
        return None
    status = _read_journal(output / 'build.json')
    return status if status is not None and status.get('layout', 1) == 1 else None


def _retry_status(build, output, binding):
    if build.is_symlink() or not build.is_dir() or (build / 'build.json').is_symlink():
        raise InstallerError('Retry requires an owned incomplete MAG build folder, without symlinks.')
    try:
        status = json.loads((build / 'build.json').read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise InstallerError('Unknown build folder; no matching MAG retry journal. Existing files are preserved.') from exc
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
    if any(p.name not in BUILD_ENTRIES or p.is_symlink() for p in build.iterdir()):
        raise InstallerError('Unknown files or symlinks in the build folder; existing files are preserved.')
    # Stage directories are renamed intact, so their legitimate SDK/tool symlinks
    # are never followed. Log destinations, which we write into, must be ordinary.
    if any(p.is_symlink() for p in (build / 'logs').rglob('*')):
        raise InstallerError('Retry logs contain symlinks; existing files are preserved.')
    return status


def _prepare_retry(build, status, token):
    """Retain diagnostics, and move the failed attempt's journal-owned scratch roots
    beside its journal (keeping its output) instead of deleting them.

    This is replayable after a power failure: the attempt number changes only
    after all cleanup succeeds, and the original attempt journal is never replaced.
    """
    native_lock = build / '.build-work.native-lock'
    with (_exclusive_lock(native_lock, True) if native_lock.exists() else nullcontext()):
        _clean_retry_scratch(build, status, token)


def _clean_retry_scratch(build, status, token):
    attempt = build / 'logs' / 'attempts' / str(status['attempt'])
    attempt.mkdir(parents=True, exist_ok=True)
    prior = attempt / 'build.json'
    if not prior.exists():
        _write_json(prior, status)
    diagnostics = [('build-settings.json', 'build-settings.json'),
                   ('build-work/native-build.json', 'native-build.json')]
    for stage in ('build', 'build-engine'):
        parent = build / 'build-work/workspace' / stage
        if parent.is_symlink() or not parent.resolve().is_relative_to(build.resolve()):
            continue
        for path in parent.glob('*.receipt.json'):
            diagnostics.append((str(path.relative_to(build)), stage + '-' + path.name))
    for source, target in diagnostics:
        path = build / source
        if path.is_file() and not path.is_symlink():
            shutil.copy2(path, attempt / target)
    stages = list(BUILD_STAGES)
    if not status['managed_complete']:
        stages.append('managed-tools')
    # Only the most recent failed attempt keeps its output: older attempts keep
    # their logs and journal, so a series of retries holds at most one extra copy.
    for older in (build / 'logs' / 'attempts').iterdir():
        token.check()
        if older == attempt or older.is_symlink() or not older.is_dir():
            continue
        for name in (*BUILD_STAGES, 'managed-tools'):
            kept = older / name
            if kept.is_symlink() or not kept.is_dir():
                continue
            for entry in kept.iterdir():
                if entry.name == 'logs':
                    continue
                if entry.is_dir() and not entry.is_symlink():
                    shutil.rmtree(entry)
                else:
                    entry.unlink()
    for name in stages:
        token.check()
        stage = build / name
        if not stage.exists():
            continue
        if stage.is_symlink() or not stage.is_dir():
            raise InstallerError('Retry scratch must be an ordinary owned directory.')
        logs = stage / 'logs'
        if logs.is_dir() and not logs.is_symlink() and any(path.is_symlink() for path in logs.rglob('*')):
            raise InstallerError('Stage logs contain symlinks; retry cleanup was stopped.')
        # The failed attempt's output is kept, not deleted: these exact roots are
        # owned scratch recorded by the matching journal, moved intact (their SDK
        # and tool symlinks are never followed) beside that attempt's journal.
        kept = attempt / name
        if not kept.exists():
            stage.rename(kept)
            continue
        # An interrupted earlier cleanup already kept a copy: keep its logs too.
        if logs.is_dir() and not logs.is_symlink():
            shutil.copytree(logs, kept / 'logs', dirs_exist_ok=True)
        shutil.rmtree(stage)
    for name in ('build-settings.json', 'build.pending', 'build-settings.pending'):
        path = build / name
        if path.exists():
            path.unlink()


def _adopt_failed_legacy(output, build, status, events):
    """A failed build an earlier installer made inside its MAG folder becomes the build folder.

    Its managed tools record their old absolute location, so the retry rebuilds them.
    """
    if status.get('state') not in ('incomplete', 'building') or status.get('output') != str(output):
        raise InstallerError(f'{output} holds an unknown or mismatched MAG build journal. Existing files are preserved.')
    unknown = sorted(p.name for p in output.iterdir() if p.name not in BUILD_ENTRIES or p.is_symlink())
    if unknown:
        raise InstallerError(f'{output} holds files this installer does not know ({", ".join(unknown)}). Move them aside, then retry. Existing files are preserved.')
    output.rename(build)
    status.update(layout=2, build=str(build), managed_complete=False)
    _write_json(build / 'build.json', status)
    events({'event': 'layout-migrated', 'build': str(build),
            'message': f'Moved the earlier failed build out of the MAG folder, to {build}. The retry continues from there.'})


def _migrate(root, output, build, status, token, events):
    """Make a MAG folder that an earlier installer finished self-contained, without rebuilding.

    The finished launcher and engine are packaged again, with the game files linked into the MAG
    folder; its saves and launch logs stay in it; everything else becomes the build folder. The old
    app, which read the game from the build files, is removed once the new one is in place.
    Each phase is journaled, so an interrupted migration continues where it stopped.
    """
    host = _manifest(root)['host']
    binding = status.get('binding') if isinstance(status.get('binding'), dict) else {}
    if status.get('state') != 'complete':
        raise InstallerError(f'{output} holds an unfinished build. Use Retry build or build --retry. Existing files are preserved.')
    if binding.get('host') != host:
        raise InstallerError(f'{output} was built for {PLAYER_HOSTS.get(binding.get("host"), "another kind of computer")}. Existing files are preserved.')
    name = app_name(host)
    unknown = sorted(p.name for p in output.iterdir() if p.name not in LEGACY_ENTRIES or p.is_symlink())
    if unknown:
        raise InstallerError(f'{output} holds files this installer does not know ({", ".join(unknown)}). Move them aside, then Build MAG again. Existing files are preserved.')
    stage = output / 'package'
    phase = status.get('migration')
    events({'event': 'stage-start', 'stage': 'migrate-layout',
            'message': f'Moving the build files out of {output} (no rebuild)'})
    if phase in (None, 'staging'):
        old_app, workspace = output / name, output / 'build-work/workspace'
        for path in (old_app, workspace):
            if path.is_symlink() or not path.is_dir():
                raise InstallerError(f'{output} is missing {path.relative_to(output)}, so it cannot be moved to the self-contained layout. Build into a new location. Existing files are preserved.')
        config = json.loads((output / 'build-settings.json').read_text(encoding='utf-8'))
        if stage.exists():
            # Our own interrupted staging, which never holds the player's files yet.
            if phase != 'staging' or stage.is_symlink() or any((stage / n).exists() for n in ('home', 'logs', 'launch-error.log')):
                raise InstallerError(f'{stage} is not this installer\'s staging. Existing files are preserved.')
            shutil.rmtree(stage)
        status['migration'] = 'staging'
        _write_json(output / 'build.json', status)
        from .native_package import package
        stage.mkdir(mode=0o700)
        package(config, workspace, stage / name, token=token, events=events)
        token.check()
        phase = status['migration'] = 'moving'
        _write_json(output / 'build.json', status)
    if phase == 'moving':
        if not (stage / name).is_dir():
            raise InstallerError(f'{stage} is incomplete. Existing files are preserved.')
        # The player's own files: saves, the server overlay and launch logs.
        for entry in ('home', 'launch-error.log'):
            if (output / entry).exists() and not (stage / entry).exists():
                (output / entry).rename(stage / entry)
        (stage / 'logs').mkdir(exist_ok=True)
        for launch in sorted((output / 'logs').glob('launch-*')) if (output / 'logs').is_dir() else ():
            if not launch.is_symlink() and not (stage / 'logs' / launch.name).exists():
                launch.rename(stage / 'logs' / launch.name)
        status.update(layout=2, build=str(build), output=str(output), app=str(output / name), migration='swapping')
        _write_json(output / 'build.json', status)
        output.rename(build)
        phase = 'swapping'
    if phase != 'swapping':
        raise InstallerError(f'Unknown migration journal in {output}. Existing files are preserved.')
    return _finish_migration(output, build, status, name, events)


def _finish_migration(output, build, status, name, events):
    """The last migration step: publish the staged MAG folder, then drop the superseded app."""
    if output.exists() or output.is_symlink():
        raise InstallerError(f'{output} appeared during the move; the new MAG folder is kept in {build / "package"}.')
    (build / 'package').rename(output)
    old_app = build / name
    if old_app.is_dir() and not old_app.is_symlink():
        shutil.rmtree(old_app)
    status.update(migration='complete', migrated_from_layout=1)
    _write_json(build / 'build.json', status)
    message = (f'MAG is now self-contained in {output}: move or copy that folder as a whole. Its build files are in '
               f'{build}; delete that folder to reclaim space. Nothing was rebuilt.')
    events({'event': 'player-build-complete', 'output': str(output), 'app': str(output / name), 'build': str(build),
            'logs': str(build / 'logs'), 'migrated': True, 'message': message})
    return output


def _publish(output, build, status, name):
    """Move the staged MAG folder into place; the journal records each side of the rename."""
    if output.exists() or output.is_symlink():
        raise InstallerError(f'{output} already exists; the new MAG folder is kept in {build / "package"}. Move the existing folder aside and retry.')
    status['state'] = 'publishing'
    _write_json(build / 'build.json', status)
    (build / 'package').rename(output)
    status.update(state='complete', app=str(output / name))
    _write_json(build / 'build.json', status)


def install(root, source, output, cache, token=None, events=None, retry=False, jobs=None):
    """jobs: the compile job count the player chose (a number, or 'auto' for this computer's
    default); None uses the remembered choice or the default (build_jobs.py). Checked first."""
    token = token or Cancellation()
    events = events or (lambda event: None)
    root, source, output, cache = (Path(p).expanduser().absolute() for p in (root, source, output, cache))
    from . import build_jobs
    jobs, jobs_source = build_jobs.resolve(cache, jobs)
    build = build_folder(output)
    state = readiness(root)
    if not state['ready']:
        raise InstallerError(state['reason'])
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise InstallerError('Choose an installation location inside an existing ordinary folder')
    lock = output.with_name('.' + output.name + '.player-lock')
    legacy = legacy_journal(output)
    if legacy is not None and legacy.get('state') == 'complete' and (build.exists() or build.is_symlink()):
        raise InstallerError(f'{output} was built by an earlier installer, and {build} already exists, so its build files '
                             'cannot move there. Move that folder aside, then Build MAG again. Existing files are preserved.')
    if legacy is not None and legacy.get('state') == 'complete':
        # A finished MAG folder of the earlier layout: no inputs, tools or compile needed.
        with _exclusive_lock(lock, True, events):
            return _migrate(root, output, build, legacy_journal(output) or legacy, token, events)
    journal_now = _read_journal(build / 'build.json') if build.is_dir() and not build.is_symlink() else None
    if journal_now is not None and journal_now.get('migration') == 'swapping' and not output.exists() and (build / 'package').is_dir():
        with _exclusive_lock(lock, True, events):
            return _finish_migration(output, build, journal_now, app_name(_manifest(root)['host']), events)
    if journal_now is not None and journal_now.get('state') == 'publishing' and output.is_dir() and not (build / 'package').exists():
        # Interrupted between moving the MAG folder into place and recording it.
        with _exclusive_lock(lock, True, events):
            name = app_name(_manifest(root)['host'])
            journal_now.update(state='complete', app=str(output / name))
            _write_json(build / 'build.json', journal_now)
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(output / name),
                    'build': str(build), 'logs': str(build / 'logs'), 'message': f'MAG was built in {output}.'})
            return output
    if retry and legacy is None and (output.exists() or output.is_symlink()):
        raise InstallerError(f'{output} already exists; a retry creates a new MAG folder there. Move it aside, then retry. Existing files are preserved.')
    if not retry:
        if output.exists() or output.is_symlink():
            raise InstallerError('Output already exists. Use Retry build or build --retry for a matching failed build.')
        if build.exists() or build.is_symlink():
            raise InstallerError(f'Build files from an earlier MAG build are in {build}. Use Retry build for a failed build, '
                                 'or delete that folder (or choose another installation location) to build again. Existing files are preserved.')
    source_real, cache_real = source.resolve(), cache.resolve()
    for writable in (output.resolve(), build.resolve(), cache_real):
        if writable.is_relative_to(source_real) or source_real.is_relative_to(writable):
            raise InstallerError('Output and cache must be separate from the owned input tree')
    for folder in (output.resolve(), build.resolve()):
        if cache_real.is_relative_to(folder) or folder.is_relative_to(cache_real):
            raise InstallerError('Output and cache must be separate directories')
    token.check()
    events({'event': 'stage-start', 'stage': 'validate', 'message': 'Checking your game files'})
    inputs = locate_inputs(source)
    record = _manifest(root)
    host = record.get('host', host_key())
    windows = host == 'windows-x86_64'
    linux = host == 'linux-x86_64'
    name = app_name(host)
    # A troubleshooting override of the x86-64 code generation level; checked before any download.
    x86_level = x86_level_override(os.environ, host)
    # Keyboard and mouse controls (mouse aim) unless MAG_GAME_CONTROLS=0; also checked before downloads.
    from .native_recipe import game_controls_enabled
    controls = game_controls_enabled(os.environ)
    # The per-user build cache (build_cache.py) unless MAG_BUILD_CACHE=0; also checked before downloads.
    from . import build_cache as build_caches
    reuse = build_caches.enabled(os.environ)
    binding = _binding(inputs, record, token)
    # The OS vendor owns the compiler/SDK installation; settle it before downloads.
    from .managed_tools import bootstrap, ensure_apple_sdk, ensure_windows_build_tools
    from .native_build import install as build_native, _verify_identities, check_windows_location
    from .native_package import package
    if windows:
        check_windows_location(output, WINDOWS_BUILD_RESERVE)
    reused = {'tools': 0.0, 'lift': 0.0, 'compile': 0.0}
    reused_unestimated = [0]
    def counted(event):
        if event.get('event') == 'build-cache-hit' and event.get('kind') in reused:
            reused[event['kind']] += event.get('saved_seconds', 0) or 0
        elif event.get('event') == 'compile-cache':
            reused['compile'] += event.get('saved_seconds', 0) or 0
            reused_unestimated[0] += event.get('unestimated_hits', 0) or 0
        events(event)
    cache_store = build_caches.BuildCache.for_cache(cache, token, counted) if reuse else None
    with _exclusive_lock(lock, retry, events), (cache_store.session() if cache_store else nullcontext()):
        if retry:
            if not build.exists() and not build.is_symlink():
                legacy = legacy_journal(output)
                if legacy is None:
                    raise InstallerError('Retry requires a matching failed build; none was found. Existing files are preserved.')
                _adopt_failed_legacy(output, build, legacy, events)
            status = _retry_status(build, output, binding)
            recovering = True
        else:
            recovering = False
            build.mkdir(mode=0o700)
            status = {'schema_version': 2, 'layout': 2, 'state': 'building', 'output': str(output), 'build': str(build),
                      'binding': binding, 'attempt': 1, 'managed_complete': False, 'tools_started': False}
        logs = build / 'logs'; logs.mkdir(exist_ok=True)
        journal = build / 'build.json'
        retry_prepared = False
        checking_inputs = False
        try:
            if recovering:
                _prepare_retry(build, status, token)
                status.update(state='building', attempt=status['attempt'] + 1, binding=binding)
                retry_prepared = True
            _write_json(journal, status)
            checking_inputs = True
            events({'event': 'stage-start', 'stage': 'prepare-source', 'logs': str(logs)})
            kit_root = build / 'build-kit'
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
            events({'event': 'stage-start', 'stage': 'acquire', 'logs': str(build / 'managed-tools/logs')})
            managed = bootstrap(build / 'managed-tools', cache=cache, token=token, events=counted,
                                **({'build_cache': cache_store} if cache_store else {}))
            status['managed_complete'] = True
            _write_json(journal, status)
            toolchain_fields = ({'msvc': {key: toolchain[key] for key in ('vcvars', 'clang_cl', 'installation', 'version', 'product')},
                                 **{key: managed[key] for key in ('ffmpeg', 'sdl2') if managed.get(key)}}
                                if windows else {'ffmpeg': managed['ffmpeg']})
            config = {'schema_version': 1, 'source_tree': str(source_tree), **inputs,
                      **{key: managed[key] for key in ('python', 'cmake', 'ninja', 'git')}, **toolchain_fields,
                      'jobs': jobs, 'native_graphics': False,
                      **({'host': host, 'linux_toolchain': {key: toolchain[key] for key in ('cc', 'cxx')}} if linux else {}),
                      'sdks': {role: {'path': str(contained(kit_root, item['path'])), 'revision': item['revision']}
                               for role, item in kit['sdks'].items()}}
            if managed.get('prefix'):
                config['managed_prefix'] = str(managed['prefix'])
            if controls:
                # The pinned Rust toolchain, beside the other managed tools.
                from .managed_tools import rust_toolchain
                config['game_controls'] = rust_toolchain(managed['root'], cache=cache, token=token, events=events)
            if x86_level:
                config['cmake_args'] = x86_level
            if cache_store:
                # Reused generation and compiled objects; they stay in the per-user cache only.
                from .compile_cache import SUPPORTED_HOSTS, ccache_tool
                config['build_cache'] = {'root': str(cache_store.root), 'base_dir': str(build)}
                if host in SUPPORTED_HOSTS:
                    config['build_cache']['ccache'] = ccache_tool(managed['root'], host, cache=cache, token=token, events=events)
            config_path = build / 'build-settings.json'
            _write_json(config_path, config)
            native_output = build / 'build-work'
            def progress(event):
                if event.get('event') == 'experimental-local-build-complete':
                    return
                counted(event)
            events({'event': 'stage-start', 'stage': 'build', 'logs': str(native_output / 'logs')})
            events({'event': 'build-jobs', 'jobs': jobs, 'source': jobs_source,
                    'message': build_jobs.describe(jobs, jobs_source)})
            build_native(config_path, native_output, token=token, events=progress, **({'retry_lock': True} if recovering else {}))
            events({'event': 'stage-start', 'stage': 'package', 'logs': str(native_output / 'logs')})
            # The MAG folder is assembled in the build folder and moved into place when complete.
            stage = build / 'package'
            stage.mkdir(mode=0o700)
            package(config, native_output / 'workspace', stage / name, token=token, events=progress)
            token.check()
            if cache_store:
                saved = sum(reused.values())
                unestimated = reused_unestimated[0]
                summary = {'event': 'build-cache-summary', **{k + '_saved_seconds': round(v, 1) for k, v in reused.items()},
                           'message': ('Build cache: this build reused earlier work, saving about '
                                       f'{build_caches.duration_text(saved)} (tools {build_caches.duration_text(reused["tools"])}, '
                                       f'generated sources {build_caches.duration_text(reused["lift"])}, compiler time '
                                       f'{build_caches.duration_text(reused["compile"])}'
                                       + (', plus compilations reused without a recorded time' if unestimated else '') + ').'
                                       if saved >= 1 or unestimated else
                                       'Build cache: nothing was reused this time; this build\'s work is kept for later builds on this computer.')}
                events(summary)
                status['build_cache'] = {k: round(v, 1) for k, v in reused.items()}
            status['logs'] = str(native_output / 'logs')
            _publish(output, build, status, name)
            app = output / name
            done = (f'MAG was built. Start {app / "MAG.exe"}.' if windows
                    else (f'MAG was built. Start {app / "MAG"}.' if linux else f'MAG.app was built in {output}.'))
            done += (f' That MAG folder is self-contained: move or copy it as a whole. '
                     f'The build files are in {build}; you may delete that folder once MAG works.')
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(app), 'build': str(build),
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
