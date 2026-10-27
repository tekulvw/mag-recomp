"""Pinned downloads and bounded extraction; never execute downloaded content here."""
from __future__ import annotations

import gzip
import hashlib
import http.client
import os
from pathlib import Path
import shutil
import stat
import tarfile
import tempfile
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from .model import InstallerError, relative_path
from .safety import contained, file_digest


def _check_url(url, allow_loopback):
    parsed = urllib.parse.urlsplit(url)
    if parsed.username is not None or parsed.password is not None or parsed.fragment:
        raise InstallerError('Tool URL must not contain credentials or a fragment')
    if parsed.scheme == 'https' and parsed.hostname:
        return
    if allow_loopback and parsed.scheme == 'http' and parsed.hostname in ('127.0.0.1', '::1'):
        return
    raise InstallerError('Pinned tool downloads require HTTPS')


class _RedirectPolicy(urllib.request.HTTPRedirectHandler):
    def __init__(self, allow_loopback):
        self.allow_loopback = allow_loopback

    def redirect_request(self, request, response, code, message, headers, newurl):
        _check_url(newurl, self.allow_loopback)
        return super().redirect_request(request, response, code, message, headers, newurl)


NETWORK_ATTEMPTS = 3
_TRANSIENT = (urllib.error.URLError, http.client.HTTPException, TimeoutError, ConnectionError)


def acquire(tool, cache, token, events, allow_loopback=False):
    """Return a reverified content-addressed blob; partial downloads restart cleanly.

    Network failures (not verification failures) are retried a bounded number
    of times, so one dropped connection does not end an hours-long build.
    allow_loopback exists only for synthetic local tests, never the release CLI.
    The cache is a user-owned local directory, not a hostile shared cache.
    """
    for attempt in range(1, NETWORK_ATTEMPTS + 1):
        try:
            return _acquire_once(tool, cache, token, events, allow_loopback)
        except InstallerError as exc:
            if attempt == NETWORK_ATTEMPTS or not isinstance(exc.__cause__, _TRANSIENT):
                raise
            events({'event': 'download-retry', 'tool': tool['id'], 'attempt': attempt + 1,
                    'message': f'Download of {tool["id"]} was interrupted; retrying ({attempt + 1}/{NETWORK_ATTEMPTS}).'})
            resume = time.monotonic() + 3 * attempt
            while time.monotonic() < resume:
                token.check(); time.sleep(0.2)


def _acquire_once(tool, cache, token, events, allow_loopback):
    token.check()
    _check_url(tool['url'], allow_loopback)
    cache = Path(cache).absolute()
    if cache.is_symlink():
        raise InstallerError('Cache root must not be a symlink')
    cache.mkdir(parents=True, exist_ok=True)
    blob = contained(cache, f'{tool["sha256"]}.blob')
    if blob.exists():
        if not blob.is_file(): raise InstallerError('Cache collision is not a regular file')
        size, digest = file_digest(blob, token, tool['size'])
        if size != tool['size'] or digest != tool['sha256']:
            raise InstallerError('Cached tool failed verification; use a fresh cache directory')
        events({'event': 'cache-hit', 'tool': tool['id']})
        return blob
    handle, partial_name = tempfile.mkstemp(prefix='download-', suffix='.part', dir=cache)
    partial = Path(partial_name)
    try:
        digest = hashlib.sha256()
        count = 0
        opener = urllib.request.build_opener(_RedirectPolicy(allow_loopback))
        request = urllib.request.Request(tool['url'], headers={'User-Agent': 'MAG-Local-Installer/0.1', 'Accept-Encoding': 'identity'})
        with os.fdopen(handle, 'wb') as target, opener.open(request, timeout=30) as response:
            if response.status != 200:
                raise InstallerError('Tool server did not return a complete archive')
            declared = response.headers.get('Content-Length')
            if declared is not None and int(declared) != tool['size']:
                raise InstallerError('Tool server size differs from pinned size')
            while block := response.read(1024 * 256):
                token.check()
                count += len(block)
                if count > tool['size']:
                    raise InstallerError('Tool download exceeds pinned size')
                target.write(block)
                digest.update(block)
                events({'event': 'download', 'tool': tool['id'], 'bytes': count, 'total': tool['size']})
            target.flush()
            os.fsync(target.fileno())
        token.check()
        if count != tool['size'] or digest.hexdigest() != tool['sha256']:
            raise InstallerError('Downloaded tool failed SHA-256 or size verification')
        # An existing winner is independently verified on the next invocation;
        # all writers have the same cryptographic identity and byte size.
        if blob.is_symlink(): raise InstallerError('Cache destination became a symlink')
        os.replace(partial, blob)
        events({'event': 'tool-verified', 'tool': tool['id']})
        return blob
    except (OSError, urllib.error.URLError, http.client.HTTPException, ValueError) as exc:
        raise InstallerError(f'Tool acquisition failed ({type(exc).__name__}); retry restarts safely') from exc
    finally:
        partial.unlink(missing_ok=True)


def extract_archive(blob, destination, tool, token):
    """Extract only bounded ordinary files/directories, then atomically publish.

    Reject links, devices, ambiguous/colliding portable paths, and special modes.
    Cached archives are rehashed here too, before they can supply executables.
    """
    token.check()
    size, digest = file_digest(blob, token, tool['size'])
    if size != tool['size'] or digest != tool['sha256']:
        raise InstallerError('Archive changed before extraction')
    destination = Path(destination).absolute()
    if destination.exists() or destination.is_symlink():
        raise InstallerError('Tool extraction destination already exists')
    destination.parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix='.extract-', dir=destination.parent))
    expanded = 0
    entries = 0
    names = {}
    actual_files = set()

    def validate_name(name, is_directory, length):
        nonlocal expanded, entries
        token.check()
        name = name[:-1] if is_directory and name.endswith('/') else name
        relative_path(name)
        entries += 1
        expanded += length
        if entries > tool['max_entries'] or length < 0 or expanded > tool['max_expanded_size']:
            raise InstallerError('Tool archive exceeds pinned extraction limits')
        parts = name.split('/')
        for end in range(1, len(parts) + 1):
            prefix = '/'.join(parts[:end])
            folded = unicodedata.normalize('NFC', prefix).casefold()
            if folded in names and names[folded] != prefix:
                raise InstallerError('Archive has case or Unicode-colliding paths')
            names[folded] = prefix
            if end < len(parts) and prefix in actual_files:
                raise InstallerError('Archive file collides with a parent directory')
        if not is_directory:
            if name in actual_files or any(entry.startswith(name + '/') for entry in names.values()):
                raise InstallerError('Archive contains duplicate or conflicting files')
            actual_files.add(name)
        return name

    def copy_file(source, name, expected):
        target = contained(scratch, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        count = 0
        with target.open('xb') as output:
            while block := source.read(256 * 1024):
                token.check()
                count += len(block)
                if count > expected:
                    raise InstallerError('Archive entry exceeds declared size')
                output.write(block)
        if count != expected:
            raise InstallerError('Truncated archive entry')
        target.chmod(0o755 if name == tool['executable'] else 0o644)

    try:
        if tool['format'] == 'zip':
            with zipfile.ZipFile(blob) as archive:
                # ZIP directory information is bounded by the compressed archive size.
                if len(archive.infolist()) > tool['max_entries']:
                    raise InstallerError('Tool archive has too many entries')
                for info in archive.infolist():
                    mode = info.external_attr >> 16
                    kind = stat.S_IFMT(mode)
                    if kind not in (0, stat.S_IFREG, stat.S_IFDIR) or info.flag_bits & 1:
                        raise InstallerError('Encrypted or special archive entries are unsupported')
                    directory = info.is_dir()
                    name = validate_name(info.filename, directory, info.file_size)
                    if directory:
                        contained(scratch, name).mkdir(parents=True, exist_ok=True)
                    else:
                        with archive.open(info) as source:
                            copy_file(source, name, info.file_size)
        elif tool['format'] == 'tar.gz':
            # Bound the entire decompressed stream, including PAX/GNU metadata,
            # before tarfile may allocate a declared extended-header payload.
            tar_limit = tool['max_expanded_size'] + tool['max_entries'] * 4096 + 10240
            with tempfile.TemporaryFile(dir=scratch) as tar_blob:
                total = 0
                with gzip.open(blob, 'rb') as compressed:
                    while block := compressed.read(256 * 1024):
                        token.check()
                        total += len(block)
                        if total > tar_limit:
                            raise InstallerError('TAR metadata or padding exceeds extraction limits')
                        tar_blob.write(block)
                tar_blob.seek(0)
                with tarfile.open(fileobj=tar_blob, mode='r|') as archive:
                    for info in archive:
                        if not (info.isfile() or info.isdir()) or info.issparse():
                            raise InstallerError('Archive links, sparse files and special entries are unsupported')
                        name = validate_name(info.name, info.isdir(), info.size)
                        if info.isdir():
                            contained(scratch, name).mkdir(parents=True, exist_ok=True)
                        else:
                            with archive.extractfile(info) as source:
                                copy_file(source, name, info.size)
        else:
            raise InstallerError('Unsupported tool archive format')
        executable = contained(scratch, tool['executable'])
        if not executable.is_file():
            raise InstallerError('Pinned tool executable is absent')
        token.check()
        if destination.exists() or destination.is_symlink():
            raise InstallerError('Tool destination appeared during extraction')
        scratch.rename(destination)
    except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError, EOFError) as exc:
        raise InstallerError(f'Tool extraction failed ({type(exc).__name__})') from exc
    finally:
        if scratch.exists(): shutil.rmtree(scratch)
