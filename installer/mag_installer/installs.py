"""What the installer remembers about each MAG folder it built, for updating it later.

``installs.json`` sits beside ``preferences.json`` in the installer's per-user cache folder
(``ux.default_cache``), never in a MAG folder, which stays self-contained and movable. It holds one
entry per MAG folder, keyed by its resolved path: the host, the game folder and firmware file the
build used, the version built, the duration of each build stage and the peak size of the build
folder. It stores no hashes: every build checks its inputs against the kit's reviewed identities.

The firmware module (libsre.prx) is the player's own file. After a successful build the installer
keeps a private copy of it, ``firmware/libsre.prx`` beside ``installs.json``, so updating never asks
for it again. The copy is checked against the reviewed identities whenever it is used, and is
deleted when it does not match. It never leaves this computer and never enters a MAG folder.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import time

from .model import InstallerError
from .platforms import private_directory
from .safety import Cancellation, file_digest

INSTALLS = 'installs.json'
SCHEMA = 1
FIRMWARE_COPY = 'firmware/libsre.prx'
# A firmware module is a few megabytes; anything far larger is not one.
MAX_FIRMWARE_BYTES = 64 * 1024 * 1024


def _key(output):
    return os.path.normcase(str(Path(output).expanduser().absolute().resolve()))


def _path(cache):
    return Path(cache).expanduser().absolute() / INSTALLS


def read(cache):
    """All entries, by MAG folder key; {} when the file is missing or damaged."""
    path = _path(cache)
    try:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 4 * 1024 * 1024:
            return {}
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict) or data.get('schema_version') != SCHEMA or not isinstance(data.get('installs'), dict):
        return {}
    return {key: value for key, value in data['installs'].items() if isinstance(value, dict)}


def entry(cache, output):
    """The entry for one MAG folder, or None."""
    return read(cache).get(_key(output))


def record(cache, output, **fields):
    """Add or replace the entry for a MAG folder after a successful build or update."""
    path = _path(cache)
    if path.parent.is_symlink():
        raise InstallerError(f'The installer cache folder must not be a symlink: {path.parent}')
    private_directory(path.parent, parents=True, exist_ok=True)
    installs = read(cache)
    installs[_key(output)] = {'output': str(Path(output).absolute()), 'recorded': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                              **{key: value for key, value in fields.items() if value is not None}}
    from .pipeline import _write_json
    _write_json(path, {'schema_version': SCHEMA, 'installs': installs})
    if os.name != 'nt':
        path.chmod(0o600)


def reviewed_firmware(reviewed):
    """The reviewed firmware SHA-256 identities from input_guide.load_reviewed, or an empty set."""
    images = (reviewed or {}).get('firmware') or []
    return {image['sha256'] for image in images if isinstance(image, dict) and isinstance(image.get('sha256'), str)}


def firmware_copy(cache, reviewed, token=None):
    """The private firmware copy when it matches a reviewed identity, else None (a bad copy is deleted).

    Without reviewed identities (a source checkout without them) the copy is offered as it is; the
    build checks it against the kit's identities before using it."""
    token = token or Cancellation()
    path = Path(cache).expanduser().absolute() / FIRMWARE_COPY
    if path.is_symlink() or not path.is_file():
        return None
    allowed = reviewed_firmware(reviewed)
    if not allowed:
        return path
    try:
        _, digest = file_digest(path, token, MAX_FIRMWARE_BYTES)
    except (OSError, InstallerError):
        digest = None
    if digest in allowed:
        return path
    try:
        path.unlink()
    except OSError:
        pass
    return None


def keep_firmware(cache, firmware, sha256, token=None):
    """Keep a private copy of the firmware module a successful build used; True when one is kept.

    The copy is written beside the target and renamed into place once its size and SHA-256 equal
    the build's record of the file (sha256), so a damaged copy never replaces a good one."""
    token = token or Cancellation()
    source = Path(firmware)
    folder = Path(cache).expanduser().absolute() / Path(FIRMWARE_COPY).parent
    target = folder / Path(FIRMWARE_COPY).name
    if folder.is_symlink() or target.is_symlink() or not source.is_file():
        return False
    try:
        if source.resolve() == target.resolve():
            return True
    except OSError:
        return False
    if target.is_file():
        try:
            if file_digest(target, token, MAX_FIRMWARE_BYTES)[1] == sha256:
                return True
        except (OSError, InstallerError):
            pass
    private_directory(folder, parents=True, exist_ok=True)
    temporary = folder / ('.libsre.prx.' + str(os.getpid()) + '.part')
    try:
        shutil.copyfile(source, temporary)
        if os.name != 'nt':
            temporary.chmod(0o600)
        if file_digest(temporary, token, MAX_FIRMWARE_BYTES)[1] != sha256:
            return False
        os.replace(temporary, target)
        return True
    finally:
        if temporary.exists():
            temporary.unlink()
