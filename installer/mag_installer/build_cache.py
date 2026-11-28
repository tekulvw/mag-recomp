"""The per-user build cache: work one MAG build can reuse from an earlier one on this computer.

It lives in the installer's own cache folder (``build-cache`` beside the verified downloads) and
holds three kinds of reusable work, each keyed by everything that determines its result:

- ``tools``: the managed dependency prefix (SDL2, glslang, SPIRV-Cross and FFmpeg on macOS and
  Linux; Vulkan-Headers, SPIRV-Headers, SPIRV-Tools and glslang on Windows), keyed by the build
  recipe, the pinned source and build tool identities, the compiler and the target.
- ``lift``: the files the lift and splice steps generate, keyed by everything those steps read
  (see lift_cache.py).
- ``objects``: the compile cache (pinned ccache), keyed per translation unit by the compiler
  identity, the command line, and the content of the source and every file it includes.

Tool and lift entries are manifests of content-addressed files in ``blobs`` (named by SHA-256), so
files that two entries share, such as the unchanged generated sources of two MAG versions, are kept
once. A file that names the build folder it was made in is stored with that path replaced by a
placeholder and gets the new build folder's path when restored. Entries appear only once all their
files are stored, and every restore checks each file against its SHA-256.

The total size is bounded (``max_bytes``, 20 GiB by default): the compile cache keeps itself within
its share, and the least recently used tool and lift entries go first when the rest is full.

Privacy: lift entries and compiled objects are derived from the player's own game files. The build
cache stays on this computer, in this per-user folder. Nothing in it is copied into a MAG folder,
an installer or a release, and nothing is uploaded. Clear build cache (in the wizard, or
``build-cache --clear``) removes it all; downloads are kept.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile
import time
import uuid

from .model import InstallerError
from .platforms import private_directory
from .safety import Cancellation

DIRECTORY = 'build-cache'
KINDS = ('tools', 'lift')
GIB = 1024 ** 3
DEFAULT_MAX_BYTES = 20 * GIB
MIN_MAX_BYTES = 2 * GIB
# The compile cache's share of the limit; ccache evicts its own least recently used objects.
OBJECTS_SHARE = 0.3
SWITCH = 'MAG_BUILD_CACHE'
SCHEMA = 1
_KEY = re.compile(r'[0-9a-f]{64}')
# Files larger than this are stored as they are, never searched for build folder paths.
TEMPLATE_LIMIT = 64 * 1024 * 1024
CHUNK = 1024 * 1024
PLACEHOLDER = b'@MAG-BUILD-CACHE-PATH-'


def enabled(environ=None):
    """Whether Build MAG uses the build cache: on unless MAG_BUILD_CACHE=0."""
    value = (os.environ if environ is None else environ).get(SWITCH, '')
    if value not in ('', '0', '1'):
        raise InstallerError(f'{SWITCH} must be 0 or 1, not {value!r}')
    return value != '0'


def root_for(cache):
    return Path(cache).expanduser().absolute() / DIRECTORY


_UNITS = {'': GIB, 'k': 1000, 'kb': 1000, 'm': 1000 ** 2, 'mb': 1000 ** 2, 'g': 1000 ** 3, 'gb': 1000 ** 3,
          't': 1000 ** 4, 'tb': 1000 ** 4, 'kib': 1024, 'mib': 1024 ** 2, 'gib': GIB, 'tib': 1024 ** 4}


def parse_size(text):
    """'20' or '20GiB' (the default unit is GiB, as for ccache), '20G'/'20GB' (decimal), '500MB'."""
    match = re.fullmatch(r'\s*([0-9]+(?:\.[0-9]+)?)\s*([a-zA-Z]*)\s*', str(text))
    unit = _UNITS.get(match[2].lower()) if match else None
    if unit is None:
        raise InstallerError(f'Build cache size must be a number with an optional unit (for example 20GiB or 15GB), not {text!r}')
    value = int(float(match[1]) * unit)
    if value < MIN_MAX_BYTES:
        raise InstallerError(f'The build cache needs at least {MIN_MAX_BYTES // GIB} GiB to be useful')
    return value


def size_text(value):
    value = float(value)
    for unit in ('B', 'KiB', 'MiB', 'GiB', 'TiB'):
        if value < 1024 or unit == 'TiB':
            return f'{value:,.1f} {unit}'
        value /= 1024


def duration_text(seconds):
    seconds = int(round(max(0, seconds)))
    return f'{seconds} s' if seconds < 60 else f'{seconds // 60} min {seconds % 60:02d} s'


def digest(material):
    """The key of a JSON description of everything that determines a result."""
    return hashlib.sha256(json.dumps(material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def file_hash(path, token=None):
    with Path(path).open('rb') as stream:
        hasher = hashlib.sha256()
        while chunk := stream.read(CHUNK):
            if token: token.check()
            hasher.update(chunk)
    return hasher.hexdigest()


def _tree_size(path):
    total = 0
    for base, directories, files in os.walk(path, followlinks=False):
        for name in files:
            try: total += os.lstat(os.path.join(base, name)).st_size
            except OSError: pass
    return total


def _remove(path):
    path = Path(path)
    def writable(function, name, *ignored):
        os.chmod(name, stat.S_IWRITE | stat.S_IREAD)   # Windows refuses to delete read-only files
        function(name)
    if path.is_symlink() or path.is_file():
        try: path.unlink()
        except PermissionError:
            os.chmod(path, stat.S_IWRITE | stat.S_IREAD); path.unlink()
    elif path.exists():
        if sys.version_info >= (3, 12): shutil.rmtree(path, onexc=writable)
        else: shutil.rmtree(path, onerror=writable)


def _write_json(path, data):
    temporary = Path(str(path) + '.pending')
    with temporary.open('w', encoding='utf-8') as output:
        json.dump(data, output, indent=1, sort_keys=True)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


def _relative(name):
    parts = PurePosixPath(name).parts
    if not parts or name.startswith('/') or '\\' in name or ':' in name or any(p in ('', '.', '..') for p in parts):
        raise InstallerError(f'Unsafe build cache path: {name!r}')
    return '/'.join(parts)


class _FileLock:
    """A kernel-held lock on a stable file: try once, or poll with cancellation."""

    def __init__(self, path):
        self.path = Path(path)
        self.handle = None

    def try_acquire(self):
        if self.path.is_symlink():
            raise InstallerError('Build cache lock is a symlink')
        handle = os.open(self.path, os.O_RDWR | os.O_CREAT | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        try:
            if os.name == 'nt':
                import msvcrt
                os.lseek(handle, 0, os.SEEK_SET)
                msvcrt.locking(handle, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(handle)
            return False
        self.handle = handle
        return True

    def acquire(self, token, events=None, waiting=''):
        notified = False
        while not self.try_acquire():
            token.check()
            if events and waiting and not notified:
                events({'event': 'build-cache-wait', 'message': waiting})
                notified = True
            time.sleep(0.25)
        return self

    def release(self):
        if self.handle is None:
            return
        try:
            if os.name == 'nt':
                import msvcrt
                os.lseek(self.handle, 0, os.SEEK_SET)
                msvcrt.locking(self.handle, msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.handle, fcntl.LOCK_UN)
        finally:
            os.close(self.handle)
            self.handle = None


def _forms(replacements):
    """Each (placeholder, path) as the byte forms a file may spell the path in: as written, JSON
    escaped (Windows backslashes doubled) and with forward slashes; longest first."""
    forms = []
    for placeholder, value in sorted(replacements.items()):
        spellings = {value, json.dumps(value)[1:-1], value.replace('\\', '/')}
        for index, spelling in enumerate(sorted(spellings, key=lambda s: (-len(s), s))):
            forms.append((PLACEHOLDER + f'{placeholder}-{index}@'.encode(), spelling, value))
    return forms


class BuildCache:
    """One per-user build cache folder. Every change to it happens under its lock."""

    def __init__(self, root, token=None, events=None):
        self.root = Path(root).absolute()
        self.token = token or Cancellation()
        self.events = events or (lambda event: None)
        self.session_lock = None

    @classmethod
    def for_cache(cls, cache, token=None, events=None):
        return cls(root_for(cache), token, events)

    # -- layout -------------------------------------------------------------------------------
    def _prepare(self):
        for path in (self.root.parent, self.root):
            if path.is_symlink():
                raise InstallerError(f'The build cache folder must not be a symlink: {path}')
        private_directory(self.root, parents=True, exist_ok=True)
        for name in (*KINDS, 'blobs', 'objects', 'sessions'):
            (self.root / name).mkdir(exist_ok=True)
        readme = self.root / 'README.txt'
        if not readme.exists():
            readme.write_text(
                'MAG build cache. It holds tool builds, generated sources and compiled objects made from your own\n'
                'game files, so that later MAG builds on this computer finish sooner. Keep it private: never share\n'
                'or upload it. Clear build cache in the MAG installer (or deleting this folder) reclaims the space\n'
                'at any time; the next build then does all of its work again.\n', encoding='utf-8')

    def objects_directory(self):
        self._prepare()
        return self.root / 'objects'

    @contextmanager
    def locked(self):
        self._prepare()
        lock = _FileLock(self.root / '.lock').acquire(
            self.token, self.events, 'Waiting for another MAG installer that is using the build cache')
        try:
            yield
        finally:
            lock.release()

    # -- settings -----------------------------------------------------------------------------
    def max_bytes(self):
        try:
            data = json.loads((self.root / 'settings.json').read_text(encoding='utf-8'))
            value = data.get('max_bytes') if isinstance(data, dict) else None
            if type(value) is int and value >= MIN_MAX_BYTES:
                return value
        except (OSError, ValueError):
            pass
        return DEFAULT_MAX_BYTES

    def objects_max_bytes(self):
        return max(GIB, int(self.max_bytes() * OBJECTS_SHARE))

    def set_max_bytes(self, value):
        if type(value) is not int or value < MIN_MAX_BYTES:
            raise InstallerError(f'The build cache needs at least {MIN_MAX_BYTES // GIB} GiB')
        with self.locked():
            _write_json(self.root / 'settings.json', {'schema_version': SCHEMA, 'max_bytes': value})
            self._evict()

    # -- sessions: a running build holds one, so the cache is never cleared under it ----------------
    @contextmanager
    def session(self):
        self._prepare()
        lock = _FileLock(self.root / 'sessions' / f'{os.getpid()}-{uuid.uuid4().hex[:12]}.lock')
        if not lock.try_acquire():
            raise InstallerError('Cannot register this build with the build cache')
        self.session_lock = lock
        try:
            yield self
        finally:
            self.session_lock = None
            lock.release()
            try: lock.path.unlink()
            except OSError: pass

    def _other_sessions(self):
        """Running builds other than this one; files of builds that ended are removed."""
        live = 0
        folder = self.root / 'sessions'
        for path in (folder.glob('*.lock') if folder.is_dir() else ()):
            if self.session_lock is not None and path == self.session_lock.path:
                continue
            probe = _FileLock(path)
            if probe.try_acquire():
                probe.release()
                try: path.unlink()
                except OSError: pass
            else:
                live += 1
        return live

    # -- blobs --------------------------------------------------------------------------------
    def _blob(self, sha256):
        return self.root / 'blobs' / sha256[:2] / sha256

    def _put(self, path, forms):
        """Store one file; returns its manifest record. Text files that spell a build folder path
        are stored with placeholders instead (see _forms)."""
        size = path.stat().st_size
        executable = bool(path.stat().st_mode & stat.S_IXUSR)
        data = None
        if forms and size <= TEMPLATE_LIMIT:
            data = path.read_bytes()
            if b'\0' in data or not any(spelling.encode('utf-8') in data for _, spelling, _ in forms):
                data = None
            else:
                if PLACEHOLDER in data:
                    raise InstallerError(f'{path} already holds a build cache placeholder')
                for placeholder, spelling, _ in forms:
                    data = data.replace(spelling.encode('utf-8'), placeholder)
        if data is not None:
            sha256 = hashlib.sha256(data).hexdigest()
        else:
            sha256 = file_hash(path, self.token)
        blob = self._blob(sha256)
        if not blob.is_file():
            blob.parent.mkdir(exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=blob.parent, prefix='.put-', delete=False) as output:
                if data is not None:
                    output.write(data)
                else:
                    with path.open('rb') as incoming:
                        while chunk := incoming.read(CHUNK):
                            self.token.check(); output.write(chunk)
                output.flush(); os.fsync(output.fileno())
            temporary = Path(output.name)
            if file_hash(temporary, self.token) != sha256:
                temporary.unlink()
                raise InstallerError(f'{path} changed while it was stored in the build cache')
            os.chmod(temporary, 0o444)
            temporary.replace(blob)
        record = {'sha256': sha256, 'size': len(data) if data is not None else size, 'executable': executable}
        if data is not None:
            record['template'] = True
        return record

    def _materialize(self, record, target, forms):
        """Create target from its blob, verified; False when the blob is missing or damaged."""
        blob = self._blob(record['sha256'])
        if not blob.is_file() or blob.is_symlink():
            return False
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() or target.is_symlink():
            _remove(target)
        if record.get('template'):
            data = blob.read_bytes()
            if hashlib.sha256(data).hexdigest() != record['sha256']:
                return False
            for placeholder, spelling, value in forms:
                data = data.replace(placeholder, spelling.encode('utf-8'))
            target.write_bytes(data)
        else:
            if file_hash(blob, self.token) != record['sha256']:
                return False
            # Hard links share the stored copy (read-only, so nothing writes into it); executables
            # need their own mode, and Windows keeps read-only per file, so those are copied.
            try:
                if os.name == 'nt' or record.get('executable'):
                    raise OSError
                os.link(blob, target)
                return True
            except OSError:
                shutil.copyfile(blob, target)
        os.chmod(target, 0o755 if record.get('executable') else 0o644)
        return True

    # -- entries ------------------------------------------------------------------------------
    def _manifest(self, kind, key):
        if kind not in KINDS or not _KEY.fullmatch(key):
            raise InstallerError('Invalid build cache entry')
        return self.root / kind / (key + '.json')

    def _record(self, kind, key):
        path = self._manifest(kind, key)
        if path.is_symlink() or not path.is_file():
            return None
        try:
            record = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return None
        if not isinstance(record, dict) or record.get('schema_version') != SCHEMA or record.get('key') != key \
                or record.get('kind') != kind or not isinstance(record.get('files'), dict):
            return None
        return record

    def lookup(self, kind, key):
        """The entry record (without touching any file), or None."""
        if not self.root.is_dir():
            return None
        return self._record(kind, key)

    def restore(self, kind, key, root, replacements=None, check=None):
        """Recreate an entry's files under root. Returns its record, or None on a miss.

        check(record) may refuse an entry (returning False) before any file is written. A damaged
        entry is removed and counts as a miss; files it already wrote are removed again."""
        root = Path(root)
        with self.locked():
            record = self._record(kind, key)
            if record is None or (check is not None and not check(record)):
                return None
            forms = _forms(replacements or {})
            written = []
            try:
                for name, item in sorted(record['files'].items()):
                    self.token.check()
                    target = root / _relative(name)
                    if 'link' in item:
                        if target.exists() or target.is_symlink():
                            _remove(target)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.symlink_to(item['link'])
                    elif not self._materialize(item, target, forms):
                        raise _Damaged(name)
                    written.append(target)
                for name in record.get('deleted', []):
                    target = root / _relative(name)
                    if target.exists() or target.is_symlink():
                        _remove(target)
            except _Damaged as damaged:
                for target in written:
                    _remove(target)
                self._manifest(kind, key).unlink()
                self.events({'event': 'build-cache-damaged', 'kind': kind,
                             'message': f'The build cache {kind} entry was damaged ({damaged}); it was removed and the work runs again.'})
                return None
            record['last_used'] = time.time()
            _write_json(self._manifest(kind, key), record)
            return record

    def store(self, kind, key, root, names, material, seconds, replacements=None, deleted=(), extra=None):
        """Store the files names (relative to root; directories are walked) as a new entry."""
        root = Path(root)
        with self.locked():
            forms = _forms(replacements or {})
            files = {}
            for name in names:
                path = root / _relative(name)
                walk = [path] if not path.is_dir() or path.is_symlink() else sorted(path.rglob('*'))
                for item in walk:
                    self.token.check()
                    relative = item.relative_to(root).as_posix()
                    if item.is_symlink():
                        files[relative] = {'link': os.readlink(item)}
                    elif item.is_file():
                        files[relative] = self._put(item, forms)
                    elif not item.is_dir():
                        raise InstallerError(f'The build cache stores only files, folders and links: {item}')
            record = {'schema_version': SCHEMA, 'kind': kind, 'key': key, 'material': material,
                      'created': time.time(), 'last_used': time.time(), 'seconds': round(seconds, 1),
                      'files': files, 'deleted': sorted(_relative(n) for n in deleted), **(extra or {})}
            _write_json(self._manifest(kind, key), record)
            self._evict()
            return record

    def entries(self):
        found = []
        for kind in KINDS:
            folder = self.root / kind
            for path in (sorted(folder.glob('*.json')) if folder.is_dir() else ()):
                record = self._record(kind, path.stem) if _KEY.fullmatch(path.stem) else None
                if record is not None:
                    found.append(record)
        return found

    def _blob_sizes(self):
        sizes = {}
        folder = self.root / 'blobs'
        for path in (folder.glob('*/*') if folder.is_dir() else ()):
            if _KEY.fullmatch(path.name):
                try: sizes[path.name] = path.stat().st_size
                except OSError: pass
        return sizes

    def usage(self):
        """Sizes for display: total, tool and lift entries, compiled objects, and the limits."""
        objects = _tree_size(self.root / 'objects') if (self.root / 'objects').is_dir() else 0
        blobs = self._blob_sizes()
        by_kind = {kind: set() for kind in KINDS}
        for record in self.entries():
            by_kind[record['kind']].update(item['sha256'] for item in record['files'].values() if 'sha256' in item)
        return {'path': str(self.root), 'bytes': _tree_size(self.root) if self.root.is_dir() else 0,
                'objects_bytes': objects, 'stored_bytes': sum(blobs.values()),
                **{kind + '_entries': sum(1 for r in self.entries() if r['kind'] == kind) for kind in KINDS},
                **{kind + '_bytes': sum(blobs.get(sha, 0) for sha in shas) for kind, shas in by_kind.items()},
                'max_bytes': self.max_bytes(), 'objects_max_bytes': self.objects_max_bytes()}

    def _evict(self):
        """Drop unreferenced files, then least recently used entries until the stored files fit
        beside the compile cache's share. Called with the lock held."""
        budget = self.max_bytes() - self.objects_max_bytes()
        records = sorted(self.entries(), key=lambda record: record.get('last_used', 0))
        if self._other_sessions() == 0:
            for leftover in (self.root / 'blobs').glob('*/.put-*'):
                _remove(leftover)   # interrupted stores of builds that ended
        evicted = []
        while True:
            referenced = {item['sha256'] for record in records for item in record['files'].values() if 'sha256' in item}
            sizes = self._blob_sizes()
            for sha256 in set(sizes) - referenced:
                _remove(self._blob(sha256))
            if not records or sum(size for sha, size in sizes.items() if sha in referenced) <= budget:
                break
            record = records.pop(0)
            self._manifest(record['kind'], record['key']).unlink()
            evicted.append(record)
        if evicted:
            self.events({'event': 'build-cache-evicted', 'count': len(evicted),
                         'message': f'Build cache: removed {len(evicted)} least recently used '
                                    f'entr{"y" if len(evicted) == 1 else "ies"} to stay within {size_text(self.max_bytes())}.'})
        return evicted

    def clear(self):
        """Delete everything in the build cache (downloads stay). Refused while a build uses it."""
        if not self.root.exists():
            return 0
        if self.root.is_symlink():
            raise InstallerError('The build cache folder must not be a symlink')
        with self.locked():
            if self._other_sessions():
                raise InstallerError('A MAG build is using the build cache. Clear it after that build finishes.')
            freed = _tree_size(self.root)
            for path in self.root.iterdir():
                if path.name not in ('.lock', 'sessions', 'settings.json'):
                    _remove(path)
            self._prepare()
        return freed


class _Damaged(Exception):
    pass


def clear(cache):
    """Clear the build cache inside an installer cache folder; returns the bytes freed."""
    return BuildCache.for_cache(cache).clear()


def usage(cache):
    return BuildCache.for_cache(cache).usage()
