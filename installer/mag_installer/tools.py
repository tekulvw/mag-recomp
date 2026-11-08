"""Pinned downloads and bounded extraction; never execute downloaded content here."""
from __future__ import annotations

import gzip
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
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

# The operating system's CA bundle, by the paths its vendors use: macOS (and
# Alpine), Debian/Ubuntu, Fedora/RHEL.
SYSTEM_CA_BUNDLES = ('/etc/ssl/cert.pem', '/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt')


def tls_context():
    """A verifying TLS context that trusts the operating system's CAs.

    A frozen installer's interpreter looks for CAs where its OpenSSL was built,
    which on a player's Mac does not exist, so every HTTPS download failed
    certificate verification. When the default context loaded no CA, the system
    bundle is added; verification itself is never relaxed."""
    context = ssl.create_default_context()
    if not context.cert_store_stats().get('x509_ca'):
        for bundle in SYSTEM_CA_BUNDLES:
            if os.path.isfile(bundle):
                context.load_verify_locations(cafile=bundle)
                break
    return context


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
_USER_AGENT = 'MAG-Local-Installer/0.1'
# Optional content-addressed mirrors, tried after the pinned URL (and GitHub's API): each
# base URL serves <base>/<sha256>.blob, the same name as a --cache entry. The pins make
# any mirror as safe as the original. Space-separated HTTPS base URLs.
MIRRORS_VARIABLE = 'MAG_INSTALLER_MIRRORS'


class _SourceFailed(Exception):
    """One download source failed in a way another source might not."""
    def __init__(self, detail, cause):
        super().__init__(detail)
        self.detail, self.cause = detail, cause


def _host(url):
    return urllib.parse.urlsplit(url).hostname or url


def _certificate_error(exc):
    reason = getattr(exc, 'reason', exc)
    return isinstance(reason, ssl.SSLCertVerificationError) or isinstance(exc, ssl.SSLCertVerificationError)


def _certificate_failure(url, exc):
    # Not a dropped connection: retrying the same trust store cannot help.
    return InstallerError(f'Tool download failed: the server certificate for {_host(url)} could not be verified '
                          "with this computer's certificate authorities. On Linux, install the ca-certificates package.")


def _describe(exc, url):
    if isinstance(exc, urllib.error.HTTPError):
        return f'HTTP {exc.code} from {_host(exc.geturl() or url)}'
    reason = getattr(exc, 'reason', exc)
    if isinstance(reason, (TimeoutError, socket.timeout)) or isinstance(exc, (TimeoutError, socket.timeout)):
        return f'timed out contacting {_host(url)}'
    if isinstance(exc, urllib.error.URLError):
        return f'could not reach {_host(url)} ({reason})'
    return f'connection to {_host(url)} failed ({type(exc).__name__})'


def _github_release_asset(url):
    """(owner, repository, tag, name) for a github.com release download URL, else None."""
    parsed = urllib.parse.urlsplit(url)
    parts = parsed.path.split('/')
    if parsed.hostname != 'github.com' or parsed.query or len(parts) != 7 or parts[3:5] != ['releases', 'download'] \
            or not all(parts[1:]):
        return None
    return tuple(urllib.parse.unquote(part) for part in (parts[1], parts[2], parts[5], parts[6]))


def _github_api_source(opener, asset, size):
    """The REST API download URL for a release asset.

    GitHub's release CDN can fail while its API keeps serving the same asset. The
    tag lookup is one unauthenticated request (60 per hour per address)."""
    owner, repository, tag, name = asset
    quote = lambda value: urllib.parse.quote(value, safe='')
    base = f'https://api.github.com/repos/{quote(owner)}/{quote(repository)}/releases'
    request = urllib.request.Request(f'{base}/tags/{quote(tag)}',
                                     headers={'User-Agent': _USER_AGENT, 'Accept': 'application/vnd.github+json'})
    try:
        with opener.open(request, timeout=30) as response:
            release = json.loads(response.read(8 * 1024 * 1024))
    except (urllib.error.URLError, http.client.HTTPException, OSError) as exc:
        if _certificate_error(exc): raise _certificate_failure(request.full_url, exc) from getattr(exc, 'reason', exc)
        raise _SourceFailed('GitHub API ' + _describe(exc, request.full_url), exc) from exc
    except ValueError as exc:
        raise _SourceFailed('GitHub API returned an unreadable release', exc) from exc
    for entry in release.get('assets', []) if isinstance(release, dict) else []:
        if isinstance(entry, dict) and entry.get('name') == name and type(entry.get('id')) is int:
            if entry.get('size') not in (None, size):
                raise InstallerError('GitHub lists a different size for this pinned tool')
            return f'{base}/assets/{entry["id"]}', {'Accept': 'application/octet-stream'}
    raise _SourceFailed(f'GitHub API lists no asset {name} for {tag}', None)


def _mirror_sources(tool):
    for base in os.environ.get(MIRRORS_VARIABLE, '').split():
        yield base.rstrip('/') + f'/{tool["sha256"]}.blob', {}


def acquire(tool, cache, token, events, allow_loopback=False):
    """Return a reverified content-addressed blob; partial downloads restart cleanly.

    Network failures (not verification failures) are retried a bounded number
    of times, so one dropped connection does not end an hours-long build. Each
    attempt tries the pinned URL, then (for a GitHub release asset) the same
    asset through GitHub's API, then any configured mirror; every byte is still
    checked against the pinned size and SHA-256.
    allow_loopback exists only for synthetic local tests, never the release CLI.
    The cache is a user-owned local directory, not a hostile shared cache.
    """
    for attempt in range(1, NETWORK_ATTEMPTS + 1):
        try:
            return _acquire_once(tool, cache, token, events, allow_loopback)
        except InstallerError as exc:
            if attempt == NETWORK_ATTEMPTS or not isinstance(exc.__cause__, _TRANSIENT):
                raise
            detail = getattr(exc, 'detail', 'interrupted')
            events({'event': 'download-retry', 'tool': tool['id'], 'attempt': attempt + 1, 'detail': detail,
                    'message': f'Download of {tool["id"]} failed ({detail}); retrying ({attempt + 1}/{NETWORK_ATTEMPTS}).'})
            resume = time.monotonic() + 3 * attempt
            while time.monotonic() < resume:
                token.check(); time.sleep(0.2)


def _fetch(opener, url, headers, tool, partial, token, events):
    """Download url into partial with the pinned checks; returns its SHA-256."""
    digest = hashlib.sha256()
    count = 0
    request = urllib.request.Request(url, headers={'User-Agent': _USER_AGENT, 'Accept-Encoding': 'identity', **headers})
    with partial.open('wb') as target, opener.open(request, timeout=30) as response:
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
    if count != tool['size']:
        # A short body is a dropped connection, which another source may complete.
        raise _SourceFailed(f'{_host(url)} ended the download early', ConnectionError('short download'))
    return digest.hexdigest()


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
    os.close(handle)
    partial = Path(partial_name)
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=tls_context()),
                                         _RedirectPolicy(allow_loopback))
    asset = _github_release_asset(tool['url'])

    def sources():
        yield tool['url'], {}
        if asset:
            yield _github_api_source(opener, asset, tool['size'])
        yield from _mirror_sources(tool)

    failures, last = [], None
    try:
        candidates = sources()
        while True:
            try:
                url, headers = next(candidates)
            except StopIteration:
                break
            except _SourceFailed as exc:
                failures.append(exc.detail); last = exc.cause or last
                continue
            if url != tool['url']:
                try:
                    _check_url(url, allow_loopback)
                except InstallerError:
                    failures.append(f'skipped {_host(url)}: mirrors must use HTTPS')
                    continue
            try:
                digest = _fetch(opener, url, headers, tool, partial, token, events)
            except _SourceFailed as exc:
                failures.append(exc.detail); last = exc.cause
            except (urllib.error.URLError, http.client.HTTPException, OSError) as exc:
                if _certificate_error(exc):
                    raise _certificate_failure(url, exc) from getattr(exc, 'reason', exc)
                if isinstance(exc, urllib.error.HTTPError) and 400 <= exc.code < 500 and exc.code not in (403, 429):
                    # The pinned asset is not there; a mirror might still hold it.
                    failures.append(_describe(exc, url)); last = last or exc
                    continue
                failures.append(_describe(exc, url)); last = exc
            else:
                token.check()
                if digest != tool['sha256']:
                    raise InstallerError('Downloaded tool failed SHA-256 or size verification')
                # An existing winner is independently verified on the next invocation;
                # all writers have the same cryptographic identity and byte size.
                if blob.is_symlink(): raise InstallerError('Cache destination became a symlink')
                os.replace(partial, blob)
                if url != tool['url']:
                    events({'event': 'download-source', 'tool': tool['id'], 'host': _host(url)})
                events({'event': 'tool-verified', 'tool': tool['id']})
                return blob
            if len(failures) == 1 and (asset or os.environ.get(MIRRORS_VARIABLE)):
                events({'event': 'download-fallback', 'tool': tool['id'], 'detail': failures[0],
                        'message': f'Download of {tool["id"]} failed ({failures[0]}); trying another source.'})
        error = InstallerError(f'Tool acquisition failed: {"; ".join(failures) or "no source"}; retry restarts safely')
        error.detail = '; '.join(failures)
        raise error from (last if isinstance(last, _TRANSIENT) else None)
    except ValueError as exc:
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
