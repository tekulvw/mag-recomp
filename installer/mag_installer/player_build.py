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

An existing MAG folder is updated in place (`update`; the swap is in update.py): the new app is
built and packaged in the build folder as for a first build, then only the app folder is swapped,
through a journal. After a successful update the build folder keeps only its logs, the previous
app (``MAG-build/previous``) and the journal.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import time
from pathlib import Path
from contextlib import contextmanager, nullcontext

from .model import InstallerError, host_key
from .native_build import FIRMWARE_SOURCE_STEPS
from .native_recipe import x86_level_override
from .pipeline import _exclusive_lock, _write_json
from .platforms import private_directory
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


def locate_inputs(source, firmware=None):
    """Accept a prepared input folder or its PS3_GAME child, without searching the disk.

    firmware: the decrypted firmware module the player chose (for example the libsre.prx RPCS3's
    --decrypt wrote in its own dev_flash); without it, firmware/libsre.prx next to PS3_GAME."""
    source = Path(source).expanduser().resolve()
    if not source.is_dir():
        raise InstallerError('Choose the folder containing your MAG game files.')
    disc = source / 'PS3_GAME' if (source / 'PS3_GAME').is_dir() else source
    base = disc.parent if disc.name == 'PS3_GAME' else source
    selected = {
        'disc_root': disc,
        'eboot_elf': disc / 'USRDIR/EBOOT.elf',
        'engine_elf': disc / 'USRDIR/bin/mage_g.elf',
        # Its folder is resolved like the game folder's; the file itself must not be a link.
        'firmware': (Path(firmware).expanduser().absolute().parent.resolve() / Path(firmware).name if firmware
                     else base / 'firmware/libsre.prx'),
    }
    if firmware and selected['firmware'].is_dir():
        raise InstallerError(f'The firmware file you chose is a folder: {selected["firmware"]}. Choose the decrypted libsre.prx.')
    required = [disc / 'PARAM.SFO', disc / 'USRDIR/EBOOT.BIN', disc / 'USRDIR/bin/mage_g.self',
                selected['eboot_elf'], selected['engine_elf'], selected['firmware']]
    absent = [path for path in required if not path.is_file() or path.is_symlink()]
    missing = [path.relative_to(base).as_posix() if path.is_relative_to(base) else str(path) for path in absent]
    if missing:
        raise InstallerError('Your game folder is missing required prepared files: ' + ', '.join(missing) +
                             '. The installer needs the original game plus the executables and firmware you decrypted with '
                             'your own RPCS3; it never decrypts files itself. The guide command (or the installer window) '
                             'finds RPCS3\'s files and shows the exact commands.' +
                             ('\n' + FIRMWARE_SOURCE_STEPS if selected['firmware'] in absent else ''))
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
                 'MAG', 'MAG.app', '.build-work.native-lock', 'build.pending', 'build-settings.pending',
                 'previous', 'previous.old'}  # the app an update replaced (update.py)
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
        private_directory(stage)
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


def install(root, source, output, cache, token=None, events=None, retry=False, jobs=None, voice=None, firmware=None,
            _update=None):
    """firmware: the decrypted firmware module the player chose, else firmware/libsre.prx next to PS3_GAME.
    jobs: the compile job count the player chose (a number, or 'auto' for this computer's
    default); None uses the remembered choice or the default (build_jobs.py). Checked first.
    voice: True or False to build with or without voice chat; None uses MAG_VOICE, else the
    remembered choice, else off (voice_option.py). Also checked first.
    _update: set only by update(), which holds the player lock, prepares the build folder and
    swaps the new app into the existing MAG folder instead of publishing a new one."""
    token = token or Cancellation()
    reported = events or (lambda event: None)
    stages, started = [], time.monotonic()
    def events(event):
        # When each stage starts, for the durations installs.json records.
        if event.get('event') == 'stage-start' and event.get('stage'):
            stages.append((event['stage'], time.monotonic()))
        reported(event)
    root, source, output, cache = (Path(p).expanduser().absolute() for p in (root, source, output, cache))
    from . import build_jobs, voice_option
    jobs, jobs_source = build_jobs.resolve(cache, jobs)
    environ = _update['environ'] if _update else os.environ
    if _update and voice is None and voice_option.from_environment(environ) is None and _update.get('voice') is not None:
        voice, voice_source = _update['voice'], 'kept'  # the option the app being updated was built with
    else:
        voice, voice_source = voice_option.resolve(cache, environ, voice)
    build = build_folder(output)
    state = readiness(root)
    if not state['ready']:
        raise InstallerError(state['reason'])
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise InstallerError('Choose an installation location inside an existing ordinary folder')
    lock = output.with_name('.' + output.name + '.player-lock')
    # An update (update()) has already finished any earlier move and checked the MAG folder.
    legacy = legacy_journal(output) if _update is None else None
    if legacy is not None and legacy.get('state') == 'complete' and (build.exists() or build.is_symlink()):
        raise InstallerError(f'{output} was built by an earlier installer, and {build} already exists, so its build files '
                             'cannot move there. Move that folder aside, then Build MAG again. Existing files are preserved.')
    if legacy is not None and legacy.get('state') == 'complete':
        if voice:
            # Moving it packages its finished binaries again, so it could not gain voice chat.
            raise InstallerError(f'{output} was built by an earlier installer without voice chat, and moving it to the new layout '
                                 'does not rebuild it. Move that MAG folder aside (keep its home/gamedata), then Build MAG again '
                                 'to build with voice chat. Existing files are preserved.')
        # A finished MAG folder of the earlier layout: no inputs, tools or compile needed.
        with _exclusive_lock(lock, True, events, reclaim=True):
            return _migrate(root, output, build, legacy_journal(output) or legacy, token, events)
    journal_now = (_read_journal(build / 'build.json') if _update is None and build.is_dir() and not build.is_symlink()
                   else None)
    if journal_now is not None and journal_now.get('migration') == 'swapping' and not output.exists() and (build / 'package').is_dir():
        with _exclusive_lock(lock, True, events, reclaim=True):
            return _finish_migration(output, build, journal_now, app_name(_manifest(root)['host']), events)
    if journal_now is not None and journal_now.get('state') == 'publishing' and output.is_dir() and not (build / 'package').exists():
        # Interrupted between moving the MAG folder into place and recording it.
        with _exclusive_lock(lock, True, events, reclaim=True):
            name = app_name(_manifest(root)['host'])
            journal_now.update(state='complete', app=str(output / name))
            _write_json(build / 'build.json', journal_now)
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(output / name),
                    'build': str(build), 'logs': str(build / 'logs'), 'message': f'MAG was built in {output}.'})
            return output
    if retry and legacy is None and (output.exists() or output.is_symlink()):
        raise InstallerError(f'{output} already exists; a retry creates a new MAG folder there. Move it aside, then retry. Existing files are preserved.')
    if not retry and _update is None:
        if output.exists() or output.is_symlink():
            raise InstallerError('Output already exists. Use Retry build or build --retry for a matching failed build.')
        if build.exists() or build.is_symlink():
            raise InstallerError(f'Build files from an earlier MAG build are in {build}. Use Retry build for a failed build, '
                                 'or delete that folder (or choose another installation location) to build again. Existing files are preserved.')
    source_real, cache_real = source.resolve(), cache.resolve()
    for writable in (output.resolve(), build.resolve(), cache_real):
        if writable.is_relative_to(source_real) or source_real.is_relative_to(writable):
            if _update is not None and writable == output.resolve() and source_real == (output / 'game').resolve():
                continue  # an update may read the MAG folder's own game files, which it never changes
            raise InstallerError('Output and cache must be separate from the owned input tree')
    for folder in (output.resolve(), build.resolve()):
        if cache_real.is_relative_to(folder) or folder.is_relative_to(cache_real):
            raise InstallerError('Output and cache must be separate directories')
    token.check()
    events({'event': 'stage-start', 'stage': 'validate', 'message': 'Checking your game files'})
    inputs = locate_inputs(source, firmware)
    record = _manifest(root)
    host = record.get('host', host_key())
    windows = host == 'windows-x86_64'
    linux = host == 'linux-x86_64'
    name = app_name(host)
    # A troubleshooting override of the x86-64 code generation level; checked before any download.
    x86_level = x86_level_override(environ, host)
    # Keyboard and mouse controls (mouse aim) unless MAG_GAME_CONTROLS=0; also checked before downloads.
    from .native_recipe import game_controls_enabled
    controls = game_controls_enabled(environ)
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
    # An update holds the player lock already (update()).
    with (_exclusive_lock(lock, retry, events, reclaim=True) if _update is None else nullcontext()), \
            (cache_store.session() if cache_store else nullcontext()):
        if _update is not None:
            status, recovering = _update['prepare'](binding)
        elif retry:
            if not build.exists() and not build.is_symlink():
                legacy = legacy_journal(output)
                if legacy is None:
                    raise InstallerError('Retry requires a matching failed build; none was found. Existing files are preserved.')
                _adopt_failed_legacy(output, build, legacy, events)
            status = _retry_status(build, output, binding)
            recovering = True
        else:
            recovering = False
            private_directory(build)
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
            if voice:
                status['voice'] = True   # this attempt builds with voice chat (absent without it)
            else:
                status.pop('voice', None)
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
            if _update is not None:
                _update['after_tools']()  # the disk-space check again, now the tools are here
            toolchain_fields = ({'msvc': {key: toolchain[key] for key in ('vcvars', 'clang_cl', 'installation', 'version', 'product')},
                                 **{key: managed[key] for key in ('ffmpeg', 'sdl2') if managed.get(key)}}
                                if windows else {'ffmpeg': managed['ffmpeg']})
            config = {'schema_version': 1, 'source_tree': str(source_tree), **inputs,
                      **{key: managed[key] for key in ('python', 'cmake', 'ninja', 'git')}, **toolchain_fields,
                      'jobs': jobs, 'native_graphics': False,
                      # Recorded in the app (no paths), so an update keeps the options it was built with.
                      'build_options': {'voice': bool(voice), 'game_controls': bool(controls),
                                        'x86_level': environ.get('MAG_X86_LEVEL') or None},
                      **({'host': host, 'linux_toolchain': {key: toolchain[key] for key in ('cc', 'cxx')}} if linux else {}),
                      'sdks': {role: {'path': str(contained(kit_root, item['path'])), 'revision': item['revision']}
                               for role, item in kit['sdks'].items()}}
            if managed.get('prefix'):
                config['managed_prefix'] = str(managed['prefix'])
            if controls:
                # The pinned Rust toolchain, beside the other managed tools.
                from .managed_tools import rust_toolchain
                config['game_controls'] = rust_toolchain(managed['root'], cache=cache, token=token, events=events)
            if voice:
                # The pinned static Opus, beside the other managed tools; -DMAG_VOICE=ON for both images
                # (the launcher image's settings windows show the Voice rows; only the engine links Opus).
                from .managed_tools import opus_prefix
                config['opus'] = {'root': opus_prefix(managed, cache=cache, token=token, events=counted,
                                                      **({'build_cache': cache_store} if cache_store else {}),
                                                      toolchain=(toolchain if windows or linux else None), host=host)}
            if x86_level or voice:
                config['cmake_args'] = [*x86_level, *(['-DMAG_VOICE=ON'] if voice else [])]
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
            if voice_option.describe(voice, voice_source):
                events({'event': 'build-voice', 'voice': voice, 'source': voice_source,
                        'message': voice_option.describe(voice, voice_source)})
            build_native(config_path, native_output, token=token, events=progress, **({'retry_lock': True} if recovering else {}))
            events({'event': 'stage-start', 'stage': 'package', 'logs': str(native_output / 'logs')})
            # The MAG folder is assembled in the build folder and moved into place when complete.
            stage = build / 'package'
            private_directory(stage)
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
            from .update import workspace_bytes
            peak = workspace_bytes(build)  # the build folder at its fullest: the workspace and the packaged app
            app = output / name
            if _update is not None:
                _update['swap'](status)
                start = app / 'MAG.exe' if windows else (app / 'MAG' if linux else app)
                swap = status['swap']
                done = (f'MAG was updated{" to " + swap["to_version"] if swap.get("to_version") else ""} in place, in {output}. '
                        f'Start it as before ({start}). Your saves, settings and logs were kept. The previous version is kept in '
                        f'{build / "previous"}; the rest of the build files were removed.')
            else:
                _publish(output, build, status, name)
                done = (f'MAG was built. Start {app / "MAG.exe"}.' if windows
                        else (f'MAG was built. Start {app / "MAG"}.' if linux else f'MAG.app was built in {output}.'))
                done += (f' That MAG folder is self-contained: move or copy it as a whole. '
                         f'The build files are in {build}; you may delete that folder once MAG works.')
            _remember(cache, source, output, host, inputs, binding, app, stages, started, peak, token, events)
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(app), 'build': str(build),
                    'logs': status['logs'], 'message': done,
                    **({'updated': True, 'from_version': status['swap'].get('from_version'),
                        'to_version': status['swap'].get('to_version')} if _update is not None else {})})
            return output
        except BaseException as exc:
            if (status.get('swap') or {}).get('phase') in ('keeping', 'cleaning', 'done'):
                raise  # the new app is in place and verified; the next run finishes (update.run_swap)
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


def _remember(cache, source, output, host, inputs, binding, app, stages, started, peak, token, events):
    """installs.json and the private firmware copy, after a successful build or update. A failure
    here never fails the build: it is reported, and the next update asks for what it lacks."""
    from . import installs
    from .update import app_version
    now = time.monotonic()
    durations = {}
    for index, (stage, began) in enumerate(stages):
        ended = stages[index + 1][1] if index + 1 < len(stages) else now
        durations[stage] = round(durations.get(stage, 0) + ended - began, 1)
    try:
        kept = installs.keep_firmware(cache, inputs['firmware'], binding['inputs']['firmware']['sha256'], token)
        installs.record(cache, output, host=host, game_folder=str(source), firmware=str(inputs['firmware']),
                        firmware_copy=kept, version=app_version(app, host), build_seconds=round(now - started, 1),
                        stage_seconds=durations, peak_build_bytes=peak)
    except (OSError, InstallerError, KeyError, TypeError) as exc:
        events({'event': 'install-record-failed',
                'message': f'Could not remember this build\'s inputs for later updates ({exc}). The next update asks for them.'})


def installer_version(root):
    """This installer's release version ("0.16.0"), or None when it does not know it."""
    candidates = []
    if getattr(sys, 'frozen', False):
        executable = Path(sys.executable).resolve()
        # PROVENANCE.json sits beside mag-installer(.exe), or beside MAG Installer.app on macOS.
        candidates += [executable.parent / 'PROVENANCE.json', *[parent / 'PROVENANCE.json' for parent in executable.parents[3:4]]]
    candidates.append(Path(root).parent / '.release-please-manifest.json')
    for path in candidates:
        try:
            data = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        version = data.get('version', data.get('.')) if isinstance(data, dict) else None
        if isinstance(version, str) and version:
            return version.removeprefix('v')
    return None


def update_plan(root, output, cache=None):
    """What updating an existing MAG folder in place would do, for the wizard; None when output is
    not one (it does not exist, or it is not a complete self-contained MAG folder)."""
    from . import installs
    from . import update as updating
    try:
        host = _manifest(root)['host']
    except InstallerError:
        return None
    output = Path(output).expanduser().absolute()
    if not updating.is_mag_folder(output, host):
        return None
    app = output / app_name(host)
    remembered = installs.entry(cache, output) if cache is not None else None
    return {'output': output, 'app': app, 'host': host,
            'from_version': updating.app_version(app, host) or (remembered or {}).get('version'),
            'to_version': installer_version(root), 'remembered': remembered or {},
            'options': updating.app_options(app, host)}


@contextmanager
def _session_guard(output, host, events):
    """Refuse while MAG runs from output, and hold its session lock (when MAG made one) so MAG does
    not start from it until the update ends. The lock file is never created here."""
    from . import update as updating
    reason = updating.running_reason(output, host, events)
    if reason:
        raise InstallerError(reason)
    path = Path(output) / updating.SESSION_LOCK
    flags = os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    try:
        handle = os.open(path, flags)
    except FileNotFoundError:
        handle = None
    if handle is None:
        yield
        return
    try:
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(handle, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise InstallerError(reason or f'MAG is running from {output}. Close MAG, then update again. Nothing was changed.') from exc
        yield
    finally:
        os.close(handle)  # closing releases the lock


def _resolve_update_inputs(root, output, cache, source, firmware, host, token, events):
    """The game folder and firmware file for an update: the player's choice, else what was
    remembered, else the MAG folder's own game files and the private firmware copy, else RPCS3's.
    Each candidate must pass the reviewed-identity checks; nothing is guessed."""
    from . import input_guide, installs
    reviewed = input_guide.load_reviewed(root)
    remembered = installs.entry(cache, output) or {}
    def accepted(findings):
        return bool(findings) and all(finding.level == input_guide.DONE for finding in findings)
    def game_ok(path):
        try:
            check = input_guide.check_game(path, reviewed, ['rpcs3'], host, token)
        except (OSError, InstallerError, ValueError):
            return False
        return check.disc is not None and accepted(check.findings)
    def firmware_ok(path):
        try:
            return accepted(input_guide.check_firmware(path, reviewed, ['rpcs3'], host, token))
        except (OSError, InstallerError, ValueError):
            return False
    detection = []
    def detected():
        if not detection:
            from . import rpcs3_detect
            try:
                found = input_guide.survey(rpcs3_detect.detect(), reviewed, None, None, token)
                detection.append((found[3], found[4]))
            except Exception:  # detection is a convenience; a missing input is reported below
                detection.append((None, None))
        return detection[0]
    if source is None:
        candidates = [(remembered.get('game_folder'), 'the game folder used last time'),
                      (output / 'game', 'the game files in the MAG folder'),
                      (lambda: detected()[0], 'RPCS3')]
        for candidate, where in candidates:
            path = candidate() if callable(candidate) else candidate
            if path and game_ok(Path(path)):
                source = Path(path)
                events({'event': 'update-input', 'input': 'game', 'path': str(source),
                        'message': f'Game files: {where} ({source})'})
                break
        else:
            raise InstallerError('Choose your MAG game folder to continue the update: the one used last time has moved '
                                 'or changed, and the MAG folder\'s own game files are missing or changed. Nothing was changed.')
    if firmware is None:
        copy = installs.firmware_copy(cache, reviewed, token)
        default = input_guide.default_firmware(source)
        candidates = [(copy, 'the copy the installer keeps'), (remembered.get('firmware'), 'the file used last time'),
                      (default if default and default.is_file() else None, 'beside the game folder'),
                      (lambda: detected()[1], 'RPCS3')]
        for candidate, where in candidates:
            path = candidate() if callable(candidate) else candidate
            if path and firmware_ok(Path(path)):
                firmware = Path(path)
                events({'event': 'update-input', 'input': 'firmware', 'path': str(firmware),
                        'message': f'Firmware module: {where} ({firmware})'})
                break
        else:
            raise InstallerError('Choose your firmware file (libsre.prx) again to continue the update. Nothing was changed.\n'
                                 + FIRMWARE_SOURCE_STEPS)
    return Path(source), Path(firmware)


def _archive_build(build, status, token):
    """Set an earlier build's work aside before a new one: keep its journal and logs under
    logs/attempts/<n>, remove its workspace. Returns the next attempt number."""
    from . import update as updating
    attempt = status.get('attempt') if type(status.get('attempt')) is int and status['attempt'] >= 1 else 1
    folder = build / 'logs' / 'attempts' / str(attempt)
    folder.mkdir(parents=True, exist_ok=True)
    if not (folder / 'build.json').exists():
        _write_json(folder / 'build.json', status)
    for stage in ('build-work', 'managed-tools'):
        logs = build / stage / 'logs'
        if logs.is_dir() and not logs.is_symlink() and not (folder / stage).exists():
            logs.rename(folder / stage)
    for name in ('build-settings.json',):
        if (build / name).is_file() and not (folder / name).exists():
            shutil.copy2(build / name, folder / name)
    for name in updating.WORKSPACE_STAGES:
        token.check()
        if (build / name).exists():
            updating._remove(build / name)
    for name in updating.WORKSPACE_FILES:
        if (build / name).exists():
            (build / name).unlink()
    return attempt + 1


def _prepare_update_folder(build, output, binding, token, events):
    """The build folder an update builds in, and its journal: new, a retry of this update's own
    failed attempt, or a fresh attempt after an earlier finished (or other) build. Returns
    (status, retrying)."""
    fresh = {'schema_version': 2, 'layout': 2, 'state': 'building', 'output': str(output), 'build': str(build),
             'binding': binding, 'attempt': 1, 'managed_complete': False, 'tools_started': False}
    if build.is_symlink():
        raise InstallerError(f'{build} is a symlink. Move it aside, then update again. Nothing was changed.')
    if not build.exists():
        private_directory(build)
        return fresh, False
    if build.is_dir() and not any(build.iterdir()):
        return fresh, False  # made by an update that stopped before writing its journal
    if not build.is_dir():
        raise InstallerError(f'{build} is not a folder. Move it aside, then update again. Nothing was changed.')
    unknown = sorted(p.name for p in build.iterdir() if p.name not in BUILD_ENTRIES or p.is_symlink())
    if unknown:
        raise InstallerError(f'{build} holds files this installer does not know ({", ".join(unknown)}). Move them aside, '
                             'then update again. Nothing was changed.')
    status = _read_journal(build / 'build.json')
    if status is None:
        raise InstallerError(f'{build} has no MAG build journal this installer knows. Move it aside, then update again. '
                             'Nothing was changed.')
    if status.get('output') != str(output):
        raise InstallerError(f'{build} belongs to another MAG folder ({status.get("output")}). Move it aside, then update '
                             'again. Nothing was changed.')
    if status.get('state') in ('incomplete', 'building') and status.get('binding') == binding:
        events({'event': 'update-retry', 'message': 'Continuing the earlier update attempt, reusing its work'})
        return _retry_status(build, output, binding), True
    if status.get('state') not in ('complete', 'incomplete', 'building'):
        raise InstallerError(f'{build} holds a build in an unknown state. Move it aside, then update again. Nothing was changed.')
    fresh['attempt'] = _archive_build(build, status, token)
    return fresh, False


def update(root, output, cache, source=None, firmware=None, token=None, events=None, jobs=None, voice=None,
           restore_previous=False):
    """Update the MAG folder output in place, or (restore_previous) put back the previous app kept
    by the last update. Only the app folder is replaced; game/, home/ and logs/ stay as they are.

    source and firmware: the player's choice, or None for the remembered inputs and their fallbacks
    (_resolve_update_inputs). The build options are the ones the app was built with, unless the
    player chooses otherwise (voice) or sets MAG_GAME_CONTROLS or MAG_X86_LEVEL."""
    from . import installs
    from . import update as updating
    token = token or Cancellation()
    events = events or (lambda event: None)
    root, output, cache = (Path(p).expanduser().absolute() for p in (root, output, cache))
    record = _manifest(root)
    host = record['host']
    if host_key() != host:
        raise InstallerError(f'This build of the MAG installer supports {PLAYER_HOSTS[host]}. Download the installer for this computer.')
    if not restore_previous:
        state = readiness(root)
        if not state['ready']:
            raise InstallerError(state['reason'])
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise InstallerError('Choose an installation location inside an existing ordinary folder')
    if output.is_symlink() or not output.is_dir():
        raise InstallerError(f'There is no MAG folder to update at {output}. Choose the folder that holds MAG, or build a new one.')
    build = build_folder(output)
    journal = build / 'build.json'
    def write(status):
        _write_json(journal, status)
    lock = output.with_name('.' + output.name + '.player-lock')
    with _exclusive_lock(lock, True, events, reclaim=True):
        # An interrupted swap is finished before anything else, whichever installer started it.
        status = _read_journal(journal) if build.is_dir() and not build.is_symlink() else None
        if status is not None and updating.recover(output, build, status, host, write, token, events) is not None:
            swap = status['swap']
            message = (f'Finished the interrupted update: MAG{" " + swap["to_version"] if swap.get("to_version") else ""} '
                       f'is in {output}.' if swap.get('kind') == 'update' else f'Finished putting back the previous MAG in {output}.')
            events({'event': 'player-build-complete', 'output': str(output), 'app': str(output / app_name(host)),
                    'build': str(build), 'logs': str(build / 'logs'), 'updated': True, 'message': message})
            return output
        legacy = legacy_journal(output)
        if legacy is not None and legacy.get('state') == 'complete' and not restore_previous:
            if build.exists() or build.is_symlink():
                raise InstallerError(f'{output} was built by an earlier installer, and {build} already exists, so its build '
                                     'files cannot move there. Move that folder aside, then update again. Nothing was changed.')
            _migrate(root, output, build, legacy, token, events)  # the earlier layout first, then the update
        updating.validate_mag_folder(output, host)
        if restore_previous:
            return _restore_previous(output, build, host, cache, write, token, events)
        plan = update_plan(root, output, cache)
        options = plan['options'] or {}
        environ = dict(os.environ)
        if not environ.get('MAG_GAME_CONTROLS') and options.get('game_controls') is False:
            environ['MAG_GAME_CONTROLS'] = '0'
        if not environ.get('MAG_X86_LEVEL') and options.get('x86_level'):
            environ['MAG_X86_LEVEL'] = options['x86_level']
        kept_voice = options.get('voice') if isinstance(options.get('voice'), bool) else None
        if kept_voice is None and status is not None and isinstance(status.get('binding'), dict):
            kept_voice = status.get('voice') is True  # an app made before build_options: its build's journal
        with _session_guard(output, host, events):
            source, firmware = _resolve_update_inputs(root, output, cache, None if source is None else Path(source),
                                                      None if firmware is None else Path(firmware), host, token, events)
            app = output / app_name(host)
            app_bytes = updating.tree_bytes(app)
            peak = (plan['remembered'] or {}).get('peak_build_bytes')
            peak = peak if type(peak) is int and peak > 0 else None
            def prepare(binding):
                # Before anything is written: a shortfall changes nothing, not even the build folder.
                updating.check_space(output, peak, app_bytes, updating.workspace_bytes(build))
                status, retrying = _prepare_update_folder(build, output, binding, token, events)
                status['update'] = {'from_version': plan['from_version'], 'to_version': plan['to_version'],
                                    'requested': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
                return status, retrying
            def swap(status):
                # MAG built before the session lock is found by its processes; check again just before.
                reason = updating.running_reason(output, host, events, check_lock=False)
                if reason:
                    raise InstallerError(reason)
                events({'event': 'stage-start', 'stage': 'update-swap'})
                updating.start_swap(output, build, status, host, 'update')
                return updating.run_swap(output, build, status, host, write, token, events)
            events({'event': 'update-start', 'output': str(output), 'from_version': plan['from_version'],
                    'to_version': plan['to_version'],
                    'message': f'Updating MAG in {output} ({plan["from_version"] or "an earlier MAG"} → '
                               f'{plan["to_version"] or "this version"}). It stays as it is until the update finishes.'})
            return install(root, source, output, cache, token=token, events=events, jobs=jobs, voice=voice, firmware=firmware,
                           _update={'environ': environ, 'voice': kept_voice, 'prepare': prepare, 'swap': swap,
                                    'after_tools': lambda: updating.check_space(output, peak, app_bytes,
                                                                                updating.workspace_bytes(build))})


def _restore_previous(output, build, host, cache, write, token, events):
    """Put back the app the last update kept in MAG-build/previous; the newer one is kept there instead."""
    from . import installs
    from . import update as updating
    kept, record = updating.kept_previous(build, host)
    status = _read_journal(build / 'build.json') if build.is_dir() and not build.is_symlink() else None
    if kept is None or status is None:
        raise InstallerError(f'No previous version of MAG is kept in {build / updating.PREVIOUS}. Nothing was changed.')
    with _session_guard(output, host, events):
        updating.verify_app(kept, host, output, token)
        events({'event': 'stage-start', 'stage': 'update-swap'})
        record = updating.start_swap(output, build, status, host, 'restore')
        app = updating.run_swap(output, build, status, host, write, token, events)
    version = updating.app_version(app, host)
    try:
        entry = installs.entry(cache, output)
        if entry is not None:
            installs.record(cache, output, **{**{k: v for k, v in entry.items() if k not in ('output', 'recorded')},
                                              'version': version})
    except (OSError, InstallerError) as exc:
        events({'event': 'install-record-failed', 'message': f'Could not update the record of this MAG folder ({exc}).'})
    message = (f'MAG{" " + version if version else ""} is back in {output}, as before the last update. The newer version '
               f'{record.get("from_version") or ""} is kept in {build / updating.PREVIOUS}; going back again puts it back. '
               'Settings the newer version wrote stay as they are; this version may ignore or reset options it does not know.')
    events({'event': 'player-build-complete', 'output': str(output), 'app': str(app), 'build': str(build),
            'logs': str(build / 'logs'), 'updated': True, 'restored': True, 'message': message.replace('  ', ' ')})
    return output
