"""Read-only input identity checks and shared containment/cancellation rules."""
from __future__ import annotations

import hashlib
from pathlib import Path
import threading

from .model import InstallerError, relative_path


class Cancelled(InstallerError):
    pass


class Cancellation:
    def __init__(self):
        self._event = threading.Event()

    def cancel(self):
        self._event.set()

    def check(self):
        if self._event.is_set():
            raise Cancelled('Installation cancelled; local staging is available for explicit recovery')


def contained(root, name):
    relative_path(name)
    root = Path(root).absolute()
    current = root
    if root.is_symlink():
        raise InstallerError('Symlink roots are unsupported')
    for part in Path(name).parts:
        current = current / part
        if current.is_symlink():
            raise InstallerError('Symlinks are unsupported')
    if not current.resolve().is_relative_to(root.resolve()):
        raise InstallerError('Path escapes its root')
    return current


def file_digest(path, token, expected_size=None):
    digest = hashlib.sha256()
    size = 0
    token.check()
    with Path(path).open('rb') as stream:
        while block := stream.read(1024 * 1024):
            token.check()
            size += len(block)
            if expected_size is not None and size > expected_size:
                raise InstallerError('File exceeds expected size')
            digest.update(block)
    token.check()
    return size, digest.hexdigest()


def validate_inputs(profile, root, token):
    token.check()
    root = Path(root).absolute()
    if not root.is_dir() or root.is_symlink():
        raise InstallerError('Select an existing ordinary game directory')
    result = []
    for item in profile.data['identity']['inputs']:
        path = contained(root, item['path'])
        if not path.is_file():
            raise InstallerError(f'Missing required input: {item["path"]}')
        try:
            size, digest = file_digest(path, token, item['size'])
        except Cancelled:
            raise
        except InstallerError as exc:
            raise InstallerError(f'Unsupported input version: {item["path"]}; expected {profile.data["identity"]["label"]}') from exc
        except OSError as exc:
            raise InstallerError(f'Cannot read required input: {item["path"]}') from exc
        if size != item['size'] or digest != item['sha256']:
            raise InstallerError(f'Unsupported input version: {item["path"]}; expected {profile.data["identity"]["label"]}')
        result.append({'path': item['path'], 'size': size, 'sha256': digest})
    if not result:
        raise InstallerError('Input version identity has not been approved/configured')
    return {'profile': profile.digest, 'files': result}
