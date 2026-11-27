"""Find the player's own RPCS3 files: read-only, bounded in depth, entries and time.

The player installs RPCS3, installs PS3 system software in it and decrypts files with it; the
installer never runs RPCS3, never downloads it or firmware, and never decrypts anything. This
module only looks where RPCS3 keeps its data, so the wizard and the guide command can suggest
paths and fill them into the commands the player runs. Every suggested file is still checked by
its SHA-256 identity before a build uses it, and nothing here copies, moves or writes a file.

Where RPCS3 keeps its data (its fs::get_config_dir):
- a ``portable`` folder next to its executable, on every platform, when one exists;
- Windows: the folder holding rpcs3.exe, or RPCS3_CONFIG_DIR; its YAML settings are in ``config/``;
- macOS: ~/Library/Application Support/rpcs3;
- Linux: $XDG_CONFIG_HOME/rpcs3, else ~/.config/rpcs3; Flatpak keeps its own under
  ~/.var/app/net.rpcs3.RPCS3/config/rpcs3, and an AppImage run with --appimage-portable-config
  under <AppImage>.config/rpcs3.
``vfs.yml`` may move dev_flash, dev_hdd0 and the games folder ($(EmulatorDir) is the data folder);
``games.yml`` maps each title ID to its game folder. ``rpcs3 --decrypt X.sprx`` writes ``X.prx``
beside it, and any other file ``X.elf``.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import struct
import sys
import threading
import time


TITLE_ID = 'BCUS98110'
APP_VERSION = '01.00'
FLATPAK_ID = 'net.rpcs3.RPCS3'
TIME_LIMIT = 3.0
MAX_ENTRIES = 20000
# Folders the bounded Windows search never enters (system and package folders, other users' data).
SKIPPED = {'appdata', 'windows', 'programdata', 'node_modules', 'system volume information', 'recovery'}
FIRMWARE_NAMES = ('libsre.prx', 'libsre.elf')


@dataclass
class Rpcs3Data:
    """One RPCS3 data folder and the folders its vfs.yml resolves."""
    data: Path
    kind: str
    dev_flash: Path
    dev_hdd0: Path
    games: Path


@dataclass
class GameCandidate:
    """A folder RPCS3 knows that holds MAG's PARAM.SFO; ``disc`` is its PS3_GAME-level folder."""
    disc: Path
    where: str
    title_id: str = ''
    version: str = ''
    category: str = ''

    @property
    def usable(self):
        """The BCUS98110 v1.0 disc's files, as far as PARAM.SFO tells. Builds still check hashes."""
        return (self.title_id == TITLE_ID and self.category in ('DG', '') and self.version in (APP_VERSION, '')
                and not self.disc.is_file())

    @property
    def note(self):
        if self.disc.is_file():
            return 'RPCS3 lists MAG as a disc image file; extract its PS3_GAME folder first.'
        if self.category and self.category != 'DG':
            return ('This is MAG data RPCS3 installed on its virtual hard disk (an update or game data), '
                    'not the disc\'s PS3_GAME folder.')
        if self.version and self.version != APP_VERSION:
            return f'This is version {self.version}; only the version {APP_VERSION} disc is supported.'
        return ''


@dataclass
class Detection:
    host: str
    rpcs3: list = field(default_factory=list)          # [argv prefix tuple, ...]: how to run the player's RPCS3
    data: list = field(default_factory=list)           # [Rpcs3Data]
    firmware_encrypted: list = field(default_factory=list)   # libsre.sprx files
    firmware_decrypted: list = field(default_factory=list)   # libsre.prx / libsre.elf files
    firmware_release: str = ''                         # dev_flash/vsh/etc/version.txt, when readable
    pup_files: list = field(default_factory=list)      # PS3UPDAT.PUP files
    games: list = field(default_factory=list)          # [GameCandidate]
    incomplete: bool = False                           # the time or entry limit stopped the search
    seconds: float = 0.0

    def best_game(self):
        usable = [game for game in self.games if game.usable]
        return usable[0] if usable else None

    def rpcs3_found(self):
        return bool(self.rpcs3 or self.data)

    def to_json(self):
        def text(value):
            return [str(item) for item in value]
        return {'host': self.host, 'rpcs3': [list(map(str, argv)) for argv in self.rpcs3],
                'data': [{'folder': str(d.data), 'kind': d.kind, 'dev_flash': str(d.dev_flash),
                          'dev_hdd0': str(d.dev_hdd0), 'games': str(d.games)} for d in self.data],
                'firmware_encrypted': text(self.firmware_encrypted), 'firmware_decrypted': text(self.firmware_decrypted),
                'firmware_release': self.firmware_release, 'pup_files': text(self.pup_files),
                'games': [{'folder': str(g.disc), 'where': g.where, 'title_id': g.title_id, 'version': g.version,
                           'category': g.category, 'usable': g.usable, 'note': g.note} for g in self.games],
                'incomplete': self.incomplete, 'seconds': round(self.seconds, 2)}


class _Budget:
    def __init__(self, time_limit, max_entries, clock):
        self.clock, self.deadline, self.left = clock, clock() + time_limit, max_entries
        self.spent = False

    def take(self, count=1):
        self.left -= count
        if self.left < 0 or self.clock() > self.deadline:
            self.spent = True
        return not self.spent


def host_name(platform=None):
    platform = platform or sys.platform
    return 'windows' if platform.startswith('win') else ('macos' if platform == 'darwin' else 'linux')


def read_sfo(path, limit=65536):
    """The string fields of a PARAM.SFO file (the documented PSF key/value table), or {}."""
    try:
        with Path(path).open('rb') as stream:
            data = stream.read(limit)
    except OSError:
        return {}
    if len(data) < 20 or data[:4] != b'\0PSF':
        return {}
    _, keys, values, count = struct.unpack_from('<IIII', data, 4)
    fields = {}
    for index in range(min(count, 256)):
        offset = 20 + index * 16
        if offset + 16 > len(data):
            break
        key_offset, kind, used, _, value_offset = struct.unpack_from('<HHIII', data, offset)
        start = keys + key_offset
        end = data.find(b'\0', start)
        if end < 0:
            break
        key = data[start:end].decode('ascii', 'replace')
        raw = data[values + value_offset:values + value_offset + used]
        if kind in (0x0204, 0x0004):
            fields[key] = raw.split(b'\0', 1)[0].decode('utf-8', 'replace')
    return fields


def _yaml_map(path):
    """The top-level and nested ``key: value`` scalars of a small YAML file (RPCS3's vfs.yml, games.yml)."""
    result = {}
    try:
        if not path.is_file() or path.stat().st_size > 4 * 1024 * 1024:
            return result
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return result
    for line in lines:
        match = re.match(r'\s*(?:-\s+)?("(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\'|[^:#]+?)\s*:\s+(.+?)\s*$', line)
        if not match:
            continue
        key, value = (_unquote(part) for part in match.groups())
        if value and value not in ('|', '>', '{}', '[]', '~', 'null'):
            result.setdefault(key, value)
    return result


def _unquote(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), text[1:-1])
    if len(text) >= 2 and text[0] == text[-1] == "'":
        return text[1:-1].replace("''", "'")
    return text


def _ini_paths(path):
    """Folders named by ``lastExplorePath...=`` lines in an RPCS3 GuiConfigs .ini (its file dialogs' last folders)."""
    found = []
    try:
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            return found
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            key, _, value = line.partition('=')
            if key.strip().lower().startswith('lastexplorepath') and value.strip():
                found.append(Path(_unquote(value.strip())))
    except OSError:
        pass
    return found


def _is_dir(path):
    try:
        return path.is_dir()
    except OSError:
        return False


def _is_file(path):
    try:
        return path.is_file()
    except OSError:
        return False


def _children(folder, budget):
    """A folder's ordinary subfolders, without following links or junctions."""
    try:
        entries = list(os.scandir(folder))
    except OSError:
        return []
    budget.take(len(entries))
    result = []
    for entry in entries:
        try:
            if entry.is_symlink() or getattr(entry, 'is_junction', lambda: False)() or not entry.is_dir(follow_symlinks=False):
                continue
        except OSError:
            continue
        if entry.name.startswith(('.', '$')) or entry.name.lower() in SKIPPED:
            continue
        result.append(Path(entry.path))
    return sorted(result)


def _files_named(folder, predicate, budget):
    try:
        entries = list(os.scandir(folder))
    except OSError:
        return []
    budget.take(len(entries))
    result = []
    for entry in entries:
        try:
            if predicate(entry.name) and entry.is_file(follow_symlinks=False):
                result.append(Path(entry.path))
        except OSError:
            continue
    return sorted(result)


def _walk(root, depth, budget, found):
    """Breadth-first over ordinary folders to ``depth`` below root, calling found(folder) on each."""
    level = [root] if _is_dir(root) else []
    for remaining in range(depth, -1, -1):
        following = []
        for folder in level:
            if not budget.take():
                return
            found(folder)
            if remaining:
                following.extend(_children(folder, budget))
        level = following


def _windows_drives():
    """Fixed local drives only: probing a removable or network drive can wait for a long time."""
    try:
        import ctypes
        kernel = ctypes.windll.kernel32
        mask = kernel.GetLogicalDrives()
        drives = []
        for index in range(26):
            if mask & (1 << index):
                root = f'{chr(65 + index)}:\\'
                if kernel.GetDriveTypeW(root) == 3:   # DRIVE_FIXED
                    drives.append(Path(root))
        return drives
    except (AttributeError, OSError):
        return [Path(os.environ.get('SystemDrive', 'C:') + '\\')]


def _resolve(value, emulator):
    value = value.replace('$(EmulatorDir)', str(emulator).rstrip('/\\') + '/')
    path = Path(value)
    return path if path.is_absolute() or PureWindowsPath(value).is_absolute() else None


def _data(folder, kind):
    """An RPCS3 data folder with the device folders its vfs.yml names (config/ on Windows)."""
    settings = {}
    for name in ('config/vfs.yml', 'vfs.yml', 'config.yml', 'config/config.yml'):
        for key, value in _yaml_map(folder / name).items():
            settings.setdefault(key, value)
    emulator = _resolve(settings['$(EmulatorDir)'], folder) if settings.get('$(EmulatorDir)') else folder
    emulator = emulator or folder
    def device(name, default):
        value = settings.get(name)
        return (_resolve(value, emulator) if value else None) or emulator / default
    return Rpcs3Data(folder, kind, device('/dev_flash/', 'dev_flash'), device('/dev_hdd0/', 'dev_hdd0'),
                     device('/games/', 'games'))


def detect(platform=None, environ=None, home=None, time_limit=TIME_LIMIT, max_entries=MAX_ENTRIES,
           drives=None, system_root=None, clock=time.monotonic):
    """Look for RPCS3, its firmware module and MAG's game folder. Never raises for unreadable places.

    platform, environ, home, drives (Windows) and system_root (/ on macOS and Linux) are injectable
    for synthetic tests. The search stops at time_limit seconds or max_entries folder entries; the
    result says when it stopped early, and a search stuck in one slow file system call is abandoned
    (its partial result is returned) shortly after the time limit."""
    platform = platform or sys.platform
    result = Detection(host_name(platform))
    started = clock()
    budget = _Budget(time_limit, max_entries, clock)
    worker = threading.Thread(target=_detect, name='mag-rpcs3-detect', daemon=True,
                              args=(result, budget, platform, dict(os.environ if environ is None else environ),
                                    Path(home) if home else Path.home(), drives, Path(system_root or '/')))
    worker.start()
    worker.join(time_limit + 0.5)
    if worker.is_alive() or budget.spent:
        result.incomplete = True
    result.seconds = clock() - started
    return Detection(**{**result.__dict__, **{key: list(getattr(result, key)) for key in
                                               ('rpcs3', 'data', 'firmware_encrypted', 'firmware_decrypted',
                                                'pup_files', 'games')}})


def _add(items, value):
    if value not in items:
        items.append(value)


def _detect(result, budget, platform, environ, home, drives, system_root):
    host = result.host
    data_folders = []   # (folder, kind)
    executables = []    # executable path files

    def data(folder, kind):
        if _is_dir(folder) and all(existing != folder for existing, _ in data_folders):
            data_folders.append((folder, kind))

    def executable_folder(folder, executable, kind):
        _add(result.rpcs3, (executable,))
        executables.append(executable)
        if _is_dir(folder / 'portable'):
            data(folder / 'portable', 'portable')
        elif kind:
            data(folder, kind)

    if host == 'windows':
        if environ.get('RPCS3_CONFIG_DIR'):
            data(Path(environ['RPCS3_CONFIG_DIR']), 'RPCS3_CONFIG_DIR')
        profile = Path(environ.get('USERPROFILE') or home)
        local = Path(environ.get('LOCALAPPDATA') or profile / 'AppData/Local')
        roots = [(profile / 'scoop/apps/rpcs3/current', 0), (profile / 'scoop/persist/rpcs3', 0),
                 (local / 'Programs', 2), (local / 'Microsoft/WinGet/Packages', 2),
                 (profile / 'Downloads', 2), (profile / 'Desktop', 2), (profile / 'Documents', 2), (profile, 2)]
        for name in ('ProgramFiles', 'ProgramFiles(x86)'):
            if environ.get(name):
                roots.append((Path(environ[name]), 2))
        for drive in (drives if drives is not None else _windows_drives()):
            roots.extend([(Path(drive) / 'RPCS3', 1), (Path(drive), 2)])
        seen = set()
        def windows_folder(folder):
            key = str(folder).lower()
            if key in seen:
                return
            seen.add(key)
            exe = folder / 'rpcs3.exe'
            if _is_file(exe):
                executable_folder(folder, exe, 'next to rpcs3.exe' if not environ.get('RPCS3_CONFIG_DIR') else '')
        for root, depth in roots:
            if budget.spent:
                break
            _walk(root, depth, budget, windows_folder)
        downloads = [profile / 'Downloads', profile / 'Desktop']
    elif host == 'macos':
        data(home / 'Library/Application Support/rpcs3', 'standard')
        for applications in (system_root / 'Applications', home / 'Applications'):
            app = applications / 'RPCS3.app/Contents/MacOS'
            if _is_file(app / 'rpcs3'):
                executable_folder(app, app / 'rpcs3', '')
        # Downloads, Desktop and Documents are privacy-protected on macOS: reading them would ask the player
        # for permission at startup, so the macOS search stays in RPCS3's own folders.
        downloads = []
    else:
        config = Path(environ['XDG_CONFIG_HOME']) if environ.get('XDG_CONFIG_HOME') else home / '.config'
        data(config / 'rpcs3', 'standard')
        if config != home / '.config':
            data(home / '.config/rpcs3', 'standard')
        data(home / '.var/app' / FLATPAK_ID / 'config/rpcs3', 'Flatpak')
        if any(_is_dir(base / FLATPAK_ID) for base in (home / '.local/share/flatpak/app', system_root / 'var/lib/flatpak/app')):
            _add(result.rpcs3, ('flatpak', 'run', FLATPAK_ID))
        on_path = shutil.which('rpcs3', path=environ.get('PATH', ''))
        if on_path:
            _add(result.rpcs3, (Path(on_path),))
            data(Path(on_path).parent / 'portable', 'portable')
        for folder in (home, home / 'Applications', home / 'Downloads', home / 'Desktop', home / '.local/bin',
                       home / 'bin', system_root / 'opt', system_root / 'opt/rpcs3'):
            for image in _files_named(folder, lambda name: name.lower().startswith('rpcs3') and name.lower().endswith('.appimage'), budget):
                _add(result.rpcs3, (image,))
                data(Path(str(image) + '.config') / 'rpcs3', 'AppImage portable')
                data(Path(str(image) + '.home') / '.config/rpcs3', 'AppImage portable')
                data(folder / 'portable', 'portable')
        downloads = [home / 'Downloads', home / 'Desktop']

    hints = []
    for folder, kind in data_folders:
        if budget.spent:
            break
        record = _data(folder, kind)
        result.data.append(record)
        external = record.dev_flash / 'sys/external'
        if _is_file(external / 'libsre.sprx'):
            _add(result.firmware_encrypted, external / 'libsre.sprx')
        for name in FIRMWARE_NAMES:
            for place in (external, folder):
                if _is_file(place / name):
                    _add(result.firmware_decrypted, place / name)
        if not result.firmware_release:
            result.firmware_release = _firmware_release(record.dev_flash / 'vsh/etc/version.txt')
        games = {}
        for name in ('config/games.yml', 'games.yml'):
            for key, value in _yaml_map(folder / name).items():
                games.setdefault(key, value)
        if TITLE_ID in games:
            _game(result, Path(games[TITLE_ID]), 'RPCS3 games list')
        for parent, where in ((record.dev_hdd0 / 'disc', 'RPCS3 dev_hdd0/disc'), (record.games, 'RPCS3 games folder')):
            for child in _children(parent, budget):
                _game(result, child, where)
        for child in _children(record.dev_hdd0 / 'game', budget):
            if child.name.upper().startswith(TITLE_ID):
                _game(result, child, 'RPCS3 dev_hdd0/game')
        for ini in ('GuiConfigs/CurrentSettings.ini',):
            hints.extend(_ini_paths(folder / ini))
    for folder in [*hints, *downloads]:
        if budget.spent or not budget.take():
            break
        for name in ('PS3UPDAT.PUP',):
            if _is_file(folder / name):
                _add(result.pup_files, folder / name)
        if _is_file(folder / 'libsre.sprx'):
            _add(result.firmware_encrypted, folder / 'libsre.sprx')
        for name in FIRMWARE_NAMES:
            if _is_file(folder / name):
                _add(result.firmware_decrypted, folder / name)
        if folder in hints:
            _game(result, folder, 'RPCS3\'s last opened folder', quiet=True)
    result.games.sort(key=lambda game: not game.usable)


def _firmware_release(path):
    """The system software release RPCS3 installed, from dev_flash/vsh/etc/version.txt (release:04.9300:)."""
    try:
        if not path.is_file() or path.stat().st_size > 4096:
            return ''
        match = re.search(r'release:0*(\d+)\.(\d\d)', path.read_text(encoding='ascii', errors='replace'))
    except OSError:
        return ''
    return f'{int(match.group(1))}.{match.group(2)}' if match else ''


def _game(result, folder, where, quiet=False):
    """Record folder (one holding PS3_GAME, PS3_GAME itself, or a dev_hdd0/game title) when its PARAM.SFO is MAG's."""
    if _is_file(folder) and not quiet:
        if all(existing.disc != folder for existing in result.games):
            result.games.append(GameCandidate(folder, where, TITLE_ID))
        return
    for disc in (folder / 'PS3_GAME', folder):
        if _is_file(disc / 'PARAM.SFO'):
            fields = read_sfo(disc / 'PARAM.SFO')
            if fields.get('TITLE_ID', '').upper() != TITLE_ID:
                return
            if any(existing.disc == disc for existing in result.games):
                return
            result.games.append(GameCandidate(disc, where, fields.get('TITLE_ID', ''),
                                              fields.get('APP_VER') or fields.get('VERSION', ''),
                                              fields.get('CATEGORY', '')))
            return
