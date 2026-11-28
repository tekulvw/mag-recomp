"""Updating an existing MAG folder in place: the app swap, its journal, rollback and recovery.

Specified in docs/superpowers/specs/2026-11-28-launcher-updates-design.md ("In-place update in the
installer"). An update builds a new app in the build folder exactly like a first build, then
replaces only the app folder (``MAG.app`` on macOS, ``MAG`` holding ``MAG.exe`` or ``MAG`` on
Windows and Linux). The MAG folder itself, ``home/``, ``logs/`` and ``game/`` stay where they are,
so the executables keep their paths and the player's saves, logs and settings are not touched.

The swap is journaled in the build folder's ``build.json`` (``swap.phase``):

1. ``staging``: the new app moves from ``MAG-build/package`` to ``<MAG folder>/.MAG-new``.
2. ``swapping``: the app folder becomes ``.MAG-previous`` and ``.MAG-new`` takes its name.
3. ``verifying``: the new app is checked (receipts, supervisor, game path) and given the previous
   app folder's permissions.
4. ``keeping``: the previous app moves to ``MAG-build/previous`` with ``previous.json``.
5. ``cleaning``: the build workspace is removed; ``MAG-build/logs``, ``MAG-build/previous`` and the
   journal stay.

A failure before ``keeping`` reverses every rename that happened, so the previous app is back where
it was. An interrupted swap (power loss, a kill) is finished by the next run for that MAG folder
before anything else: the renames are undone and redone from the staged app, without rebuilding.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import secrets
import stat
import time

from .model import InstallerError
from .platforms import private_directory, publish_directory
from .safety import Cancellation, contained, file_digest

APP_NEW, APP_PREVIOUS, GAME_PREVIOUS = '.MAG-new', '.MAG-previous', '.game-previous'
GAME = 'game'
PREVIOUS = 'previous'
PREVIOUS_RECORD = 'previous.json'
# The swap phases a later run must finish or reverse before it does anything else.
OPEN_PHASES = ('staging', 'swapping', 'verifying', 'keeping', 'cleaning')
# What a MAG folder may hold besides its app and game files: what MAG writes, and what the
# desktop adds by itself.
PLAYER_ENTRIES = {'home', 'logs', 'launch-error.log'}
DESKTOP_ENTRIES = {'.DS_Store', 'desktop.ini', 'Thumbs.db'}
# The launcher holds this lock while it runs (shared); the installer never creates it.
SESSION_LOCK = 'home/.session-lock'
GIB = 1024 ** 3
# The build folder's peak size when no earlier build on this computer recorded one: the
# installer guide's "roughly 40 GB" (installer/docs/testing.md). The build cache usually makes a
# rebuild smaller; the measured peak replaces this after the first build that records it.
DEFAULT_PEAK_BYTES = 40 * GIB
MARGIN_BYTES = 2 * GIB
# The parts of the build folder that the update removes after it succeeds (decision 13).
WORKSPACE_STAGES = ('build-kit', 'build-work', 'managed-tools', 'package', 'MAG', 'MAG.app')
WORKSPACE_FILES = ('build-settings.json', 'build.pending', 'build-settings.pending', '.build-work.native-lock')
# What workspace_bytes leaves out: what the build folder keeps between updates.
KEPT_ENTRIES = ('logs', 'previous', 'previous.old', 'build.json')


def workspace_bytes(build):
    """The bytes a build folder's workspace takes, without its logs and the previous app."""
    return tree_bytes(build, skip=KEPT_ENTRIES) if Path(build).is_dir() else 0


def app_name(host):
    return 'MAG.app' if host == 'macos-arm64' else 'MAG'


def runtime_path(app, host):
    return Path(app) / ('Contents/Resources/runtime.json' if host == 'macos-arm64' else 'resources/runtime.json')


def receipts_folder(app, host):
    return Path(app) / ('Contents/Resources/package-receipts' if host == 'macos-arm64' else 'resources/package-receipts')


def supervisor_path(app, host):
    return Path(app) / {'macos-arm64': 'Contents/MacOS/MAG', 'windows-x86_64': 'MAG.exe'}.get(host, 'MAG')


def read_runtime(app, host):
    """The app's runtime.json, or None when it is missing or not a MAG runtime record."""
    path = runtime_path(app, host)
    try:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
            return None
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get('schema_version') == 2 else None


def app_version(app, host):
    """The release version an app was built from, or None when it does not say."""
    version = (read_runtime(app, host) or {}).get('version')
    return version if isinstance(version, str) and version and version != 'unknown' else None


def app_options(app, host):
    """The build options recorded in an app (voice, game_controls, x86_level), or None."""
    options = (read_runtime(app, host) or {}).get('build_options')
    return options if isinstance(options, dict) else None


def is_mag_folder(output, host):
    """Whether output is a complete MAG folder of the self-contained layout."""
    try:
        validate_mag_folder(output, host)
        return True
    except InstallerError:
        return False


def validate_mag_folder(output, host):
    """A complete self-contained MAG folder: its app, its game files and what MAG wrote, nothing else."""
    output = Path(output)
    name = app_name(host)
    if output.is_symlink() or not output.is_dir():
        raise InstallerError(f'{output} is not a MAG folder. Choose the folder that holds {name}.')
    entries = {path.name: path for path in output.iterdir()}
    unknown = sorted(entry for entry in entries
                     if entry not in {name, GAME} | PLAYER_ENTRIES | DESKTOP_ENTRIES)
    links = sorted(entry for entry, path in entries.items() if path.is_symlink())
    if unknown or links:
        raise InstallerError(f'{output} holds files this installer does not know ({", ".join(unknown + links)}). '
                             'Move them aside, then update again. Nothing was changed.')
    for entry in (name, GAME):
        if entry not in entries or not entries[entry].is_dir():
            raise InstallerError(f'{output} has no {entry} folder, so it is not a complete MAG folder. Nothing was changed.')
    if read_runtime(output / name, host) is None:
        raise InstallerError(f'{output / name} is not a MAG app this installer can update (its runtime.json is missing '
                             'or unreadable). Nothing was changed.')
    return output / name


def running_reason(output, host, events=None, check_lock=True):
    """Why the update must wait for MAG to close, or None.

    MAG holds a lock on home/.session-lock while it runs; MAG built before that lock is found by
    its processes' executable paths inside the app folder. When the process list cannot be read,
    that is said, and the update continues: Windows then refuses to move a running app anyway.
    check_lock=False skips the lock, for a caller that holds it itself."""
    from . import platforms
    events = events or (lambda event: None)
    output = Path(output)
    message = (f'MAG is running from {output}. Close MAG and its settings window, then update again. '
               'Nothing was changed.')
    if check_lock and not platforms.lock_is_free(output / SESSION_LOCK):
        return message
    try:
        executables = platforms.running_executables()
    except Exception as exc:  # advisory: an unreadable process list must not stop the update
        events({'event': 'update-notice', 'message': f'Could not check whether MAG is running ({exc}). Make sure MAG is closed.'})
        return None
    app = os.path.normcase(os.path.realpath(output / app_name(host)))
    if any(os.path.normcase(path) == app or os.path.normcase(path).startswith(app + os.sep) for path in executables):
        return message
    return None


def tree_bytes(path, skip=()):
    """The bytes a folder's files take, counting a hard-linked file once; links are not followed.
    skip: names of top-level entries to leave out."""
    seen, total = set(), 0
    for folder, folders, files in os.walk(path):
        if folder == str(path):
            folders[:] = [name for name in folders if name not in skip]
            files = [name for name in files if name not in skip]
        for name in files:
            try:
                info = os.lstat(os.path.join(folder, name))
            except OSError:
                continue
            if stat.S_ISREG(info.st_mode) and (info.st_dev, info.st_ino) not in seen:
                seen.add((info.st_dev, info.st_ino))
                total += info.st_size
    return total


def size_text(value):
    return f'{value / GIB:.1f} GB' if value >= GIB else f'{value / 1024 ** 2:.0f} MB'


def check_space(output, peak_bytes, app_bytes, used_bytes=0):
    """Refuse an update the MAG folder's volume has no room for: the build folder's peak, one more
    app (the previous one is kept) and a margin, less what the build folder already holds."""
    output = Path(output)
    need = max(0, (peak_bytes or DEFAULT_PEAK_BYTES) - used_bytes) + app_bytes + MARGIN_BYTES
    free = shutil.disk_usage(output.parent).free
    if free < need:
        raise InstallerError(f'Updating MAG needs about {size_text(need)} free on the drive that holds {output.parent} '
                             f'({size_text(free)} free now). Free some space, then update again. MAG was not changed.')
    return {'needed_bytes': need, 'free_bytes': free}


def same_tree(first, second, token=None):
    """Whether two folders hold the same names and file contents (hard links compare by identity)."""
    token = token or Cancellation()
    def listing(root):
        found = {}
        for folder, folders, files in os.walk(root):
            for name in folders + files:
                path = Path(folder) / name
                if path.is_symlink():
                    return None
                found[path.relative_to(root).as_posix()] = path
        return found
    left, right = listing(first), listing(second)
    if left is None or right is None or left.keys() != right.keys():
        return False
    for relative, path in left.items():
        other = right[relative]
        if path.is_dir() != other.is_dir():
            return False
        if path.is_dir():
            continue
        if os.path.samefile(path, other):
            continue
        if path.stat().st_size != other.stat().st_size or file_digest(path, token)[1] != file_digest(other, token)[1]:
            return False
    return True


def verify_app(app, host, output, token=None):
    """The installer's self-check of an app in the MAG folder: its runtime record, its game path,
    its supervisor and the receipts of both images. Starts nothing."""
    token = token or Cancellation()
    app, output = Path(app), Path(output)
    runtime = read_runtime(app, host)
    if runtime is None:
        raise InstallerError(f'The new app has no readable runtime record ({runtime_path(app, host)}).')
    game = runtime.get('game')
    if (not isinstance(game, str) or not game or os.path.isabs(game)
            or os.path.normpath(output / app.name / game) != os.path.normpath(output / GAME)
            or not (output / GAME / 'PS3_GAME').is_dir()):
        raise InstallerError('The new app does not find the game files in the MAG folder.')
    supervisor = supervisor_path(app, host)
    if supervisor.is_symlink() or not supervisor.is_file() or (os.name != 'nt' and not os.access(supervisor, os.X_OK)):
        raise InstallerError(f'The new app has no usable launcher: {supervisor}')
    for role in ('launcher', 'engine'):
        relative = runtime.get(role)
        receipt_path = receipts_folder(app, host) / (role + '.json')
        try:
            binary = contained(app, relative)
            receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        except (OSError, ValueError, TypeError, InstallerError) as exc:
            raise InstallerError(f'The new app\'s {role} or its receipt is missing.') from exc
        if binary.is_symlink() or not binary.is_file() or not isinstance(receipt, dict) \
                or receipt.get('binary_sha256') != file_digest(binary, token)[1]:
            raise InstallerError(f'The new app\'s {role} does not match its receipt.')
        # The game executable that image was built from, as MAG checks it at every start.
        elf = output / GAME / 'PS3_GAME/USRDIR' / ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        if 'elf_sha256' in receipt and (not elf.is_file() or file_digest(elf, token)[1] != receipt['elf_sha256']):
            raise InstallerError(f'The game files in the MAG folder are not the ones the new app\'s {role} was built from.')


def _step(name):
    """One named step of the swap. Tests replace it to fail or stop at that step."""


def _rename(source, destination):
    """Move a folder within the MAG folder's volume, never replacing anything."""
    attempts = 5 if os.name == 'nt' else 1
    for attempt in range(attempts):
        try:
            publish_directory(source, destination)
            return
        except InstallerError as exc:
            # Windows: a virus scanner or the search indexer may hold a file in it for a moment.
            if attempt + 1 == attempts:
                raise InstallerError(f'Cannot move {source} to {destination}: {exc.__cause__ or exc}') from exc
            time.sleep(0.5)


def _remove(path):
    path = Path(path)
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        def writable(function, name, _error):
            os.chmod(name, stat.S_IWRITE)
            function(name)
        import sys
        shutil.rmtree(path, **({'onexc': writable} if sys.version_info >= (3, 12) else {'onerror': writable}))


def _now():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def _capture_permissions(app, output, host):
    """What the previous app folder's permissions are, so the new one gets the same."""
    if os.name == 'nt':
        from . import platforms
        return {'app': platforms.folder_dacl(app), 'folder': platforms.folder_dacl(output)}
    return {'mode': stat.S_IMODE(os.stat(app).st_mode)}


def _keep_permissions(app, output, record):
    """Give the new app folder the previous one's permissions, and check them; a difference fails."""
    permissions = record.get('permissions') or {}
    if os.name != 'nt':
        if 'mode' in permissions:
            os.chmod(app, permissions['mode'])
            if stat.S_IMODE(os.stat(app).st_mode) != permissions['mode']:
                raise InstallerError('The new app folder\'s permissions differ from the previous one\'s.')
        return
    from . import platforms
    previous = permissions['app']
    platforms.apply_explicit_dacl(app, previous)
    # The entries the folder was given and whether it inherits; inherited entries come from the MAG
    # folder, which is checked to be unchanged.
    now = platforms.folder_dacl(app)
    if platforms.explicit_dacl(now) != platforms.explicit_dacl(previous):
        raise InstallerError('The new app folder\'s permissions differ from the previous one\'s; the update was undone. '
                             f'Previous: {previous} New: {now}')
    folder = platforms.folder_dacl(output)
    if platforms.dacl_parts(folder) != platforms.dacl_parts(permissions['folder']):
        raise InstallerError('The MAG folder\'s permissions changed during the update; the update was undone. '
                             f'Before: {permissions["folder"]} Now: {folder}')
    sid = platforms.current_user_sid()
    if platforms.grants_full_control(platforms.dacl_parts(previous)[1], sid):
        for path in [app, *app.rglob('*')]:
            if not platforms.grants_full_control(platforms.dacl_parts(platforms.folder_dacl(path))[1], sid):
                raise InstallerError(f'Your account lost full access to {path} in the new app; the update was undone.')


def clean_workspace(build, status):
    """After a successful update: keep only the build folder's logs, its previous app and the journal.

    The build's and the tools' own logs move into MAG-build/logs first."""
    build = Path(build)
    logs = build / 'logs'
    private_directory(logs, exist_ok=True)
    for stage in ('build-work', 'managed-tools'):
        source = build / stage / 'logs'
        if source.is_dir() and not source.is_symlink():
            target = logs / stage
            if target.exists() or target.is_symlink():
                _remove(target)
            source.rename(target)
    for name in WORKSPACE_STAGES:
        path = build / name
        if path.exists() or path.is_symlink():
            _remove(path)
    for name in WORKSPACE_FILES:
        path = build / name
        if path.exists() or path.is_symlink():
            path.unlink()
    status['logs'] = str(logs / 'build-work')


def start_swap(output, build, status, host, kind='update'):
    """The journal record of a new swap. kind: 'update' (the app staged in MAG-build/package) or
    'restore' (the previous app kept in MAG-build/previous)."""
    name = app_name(host)
    staged = ('package' if kind == 'update' else PREVIOUS)
    record = {'kind': kind, 'id': secrets.token_hex(8), 'phase': 'staging', 'started': _now(),
              'staged': staged + '/' + name, 'game_staged': staged + '/' + GAME,
              'from_version': app_version(Path(output) / name, host),
              'to_version': app_version(Path(build) / staged / name, host)}
    status['swap'] = record
    return record


def run_swap(output, build, status, host, write, token=None, events=None):
    """Carry the journaled swap in status['swap'] from its phase to the end; on a failure before the
    previous app is kept, undo every rename, record it and raise. Returns the app folder."""
    token = token or Cancellation()
    events = events or (lambda event: None)
    record = status['swap']
    try:
        return _advance(Path(output), Path(build), status, record, host, write, token, events)
    except BaseException as exc:
        if record.get('phase') not in ('staging', 'swapping', 'verifying'):
            # The new app is in place and verified: the next run finishes keeping or cleaning.
            record['error'] = str(exc) or type(exc).__name__
            write(status)
            raise
        try:
            reverse(Path(output), Path(build), record, host)
        except BaseException as again:
            record['rollback_error'] = str(again) or type(again).__name__
            try:
                write(status)
            except OSError:
                pass
            raise InstallerError(f'The update failed ({exc}) and could not be undone ({again}). Run the installer '
                                 f'again for {output}: it finishes undoing it first.') from exc
        record.update(phase='rolled-back', error=str(exc) or type(exc).__name__, ended=_now())
        if record['kind'] == 'update':
            status['state'] = 'incomplete'
            status['error'] = record['error']
        try:
            write(status)
        except OSError:
            pass
        raise


def reverse(output, build, record, host):
    """Undo the renames of an unfinished swap: the previous app (and game files) back in place, the
    new ones back where they were staged. Safe to repeat after an interruption."""
    name = app_name(host)
    app, new, previous = output / name, output / APP_NEW, output / APP_PREVIOUS
    staged, game_staged = build / record['staged'], build / record['game_staged']
    if previous.is_dir():
        if app.exists():
            if new.exists():
                raise InstallerError(f'{output} holds both {APP_NEW} and a new app; move one aside, then run the installer again.')
            _rename(app, new)
        _rename(previous, app)
    if (output / GAME_PREVIOUS).is_dir():
        if (output / GAME).exists():
            private_directory(game_staged.parent, exist_ok=True)
            _rename(output / GAME, game_staged)
        _rename(output / GAME_PREVIOUS, output / GAME)
    if new.is_dir():
        private_directory(staged.parent, exist_ok=True)
        _rename(new, staged)


def recover(output, build, status, host, write, token=None, events=None):
    """Finish a swap an earlier run left open; returns the app folder, or None when none was open.

    Before the previous app was kept, the renames are undone and the swap runs again from the staged
    app, so nothing is rebuilt. After it, keeping and cleaning finish."""
    record = (status or {}).get('swap')
    if not isinstance(record, dict) or record.get('phase') not in OPEN_PHASES:
        return None
    events = events or (lambda event: None)
    events({'event': 'update-recovering', 'phase': record['phase'],
            'message': f'Finishing the {"update" if record.get("kind") == "update" else "change"} of MAG that was interrupted'})
    if record['phase'] in ('staging', 'swapping', 'verifying'):
        reverse(Path(output), Path(build), record, host)
        record['phase'] = 'staging'
        write(status)
    return run_swap(output, build, status, host, write, token, events)


def _advance(output, build, status, record, host, write, token, events):
    name = app_name(host)
    app, new, previous = output / name, output / APP_NEW, output / APP_PREVIOUS
    staged, game_staged = build / record['staged'], build / record['game_staged']
    if record['phase'] == 'staging':
        _step('stage')
        token.check()
        if not app.is_dir() or not staged.is_dir():
            raise InstallerError(f'The {"new" if record["kind"] == "update" else "previous"} app or the current one is '
                                 'missing, so MAG was not changed.')
        if 'game' not in record:
            if record['kind'] == 'update':
                kept = not game_staged.is_dir() or same_tree(game_staged, output / GAME, token)
            else:
                kept = not game_staged.is_dir()
            record['game'] = 'kept' if kept else 'replaced'
            write(status)
        if record['game'] == 'kept' and record['kind'] == 'update' and game_staged.exists():
            _remove(game_staged)  # the same files as the MAG folder's game/, which stays
        record['permissions'] = _capture_permissions(app, output, host)
        _rename(staged, new)
        record['phase'] = 'swapping'
        write(status)
    if record['phase'] == 'swapping':
        _step('set-aside')
        if app.exists() and not previous.exists():
            _rename(app, previous)
        _step('move-in')
        if new.exists() and not app.exists():
            _rename(new, app)
        if record['game'] == 'replaced':
            _step('game')
            if not (output / GAME_PREVIOUS).exists():
                _rename(output / GAME, output / GAME_PREVIOUS)
            if game_staged.exists() and not (output / GAME).exists():
                _rename(game_staged, output / GAME)
        record['phase'] = 'verifying'
        write(status)
    if record['phase'] == 'verifying':
        _step('verify')
        verify_app(app, host, output, token)
        _step('permissions')
        _keep_permissions(app, output, record)
        record['phase'] = 'keeping'
        status.update(state='complete', app=str(app))
        status.pop('error', None)
        write(status)
        events({'event': 'update-verified', 'message': 'The new app passed its checks'})
    if record['phase'] == 'keeping':
        _step('keep')
        kept, older = build / PREVIOUS, build / (PREVIOUS + '.old')
        if record['kind'] == 'update':
            marker = _read_previous(kept)
            if kept.exists() and not older.exists() and (marker or {}).get('swap') != record['id']:
                kept.rename(older)  # the app an earlier update kept; replaced by this one
        private_directory(kept, exist_ok=True)
        if previous.exists():
            _rename(previous, kept / name)
        if (output / GAME_PREVIOUS).exists():
            _rename(output / GAME_PREVIOUS, kept / GAME)
        from .pipeline import _write_json
        _write_json(kept / PREVIOUS_RECORD, {'schema_version': 1, 'version': record.get('from_version'),
                                             'kept': _now(), 'swap': record['id'], 'game': (kept / GAME).is_dir()})
        if older.exists():
            _remove(older)
        record['phase'] = 'cleaning'
        write(status)
    if record['phase'] == 'cleaning':
        _step('clean')
        if record['kind'] == 'update':
            clean_workspace(build, status)
        record.update(phase='done', ended=_now())
        write(status)
    return app


def _read_previous(kept):
    try:
        data = json.loads((Path(kept) / PREVIOUS_RECORD).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def kept_previous(build, host):
    """The previous app kept in MAG-build/previous and its record, or (None, None)."""
    kept = Path(build) / PREVIOUS
    app = kept / app_name(host)
    if kept.is_symlink() or not app.is_dir() or app.is_symlink():
        return None, None
    return app, _read_previous(kept) or {}
