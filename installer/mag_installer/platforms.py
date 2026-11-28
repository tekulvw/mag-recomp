"""Small host adapters. OS branches require certification on each target OS."""
from contextlib import contextmanager
import ctypes
import errno
import os
from pathlib import Path
import platform
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tempfile

from .model import InstallerError


# Folders the installer makes for the player: the MAG folder, its app and game folders, the build
# folder and its scratch, staging and the per-user build cache. On macOS and Linux they are made
# owner-only (mode 0o700). On Windows they inherit the permissions of the folder the player chose,
# as a folder made there in Explorer would (the player's profile folders already keep other
# accounts out); that is also what Python did on Windows before the change below. Python's mode=0o700 on Windows (3.12.4 and later, the fix for CVE-2024-4030) instead gives the
# folder a protected DACL granting only SYSTEM, Administrators and the folder's owner (OWNER
# RIGHTS). When the installer runs elevated, the owner is the Administrators group, so the
# player's own account loses access: MAG started without admin rights cannot write its saves or
# logs, or even read its game files, and the player cannot delete the build folder. A folder
# moved into place (the MAG folder is renamed out of the build folder) keeps the permissions it
# was made with, so the folder must be made inheriting from the start.
OWNER_ONLY = 0o700


def private_directory(path, parents=False, exist_ok=False):
    """mkdir for a folder the installer makes for the player: owner-only on macOS and Linux,
    inheriting its parent's permissions on Windows (see OWNER_ONLY above). Returns the Path."""
    path = Path(path)
    if os.name == 'nt':
        path.mkdir(parents=parents, exist_ok=exist_ok)
    else:
        path.mkdir(mode=OWNER_ONLY, parents=parents, exist_ok=exist_ok)
    return path


def private_scratch(prefix, dir):
    """tempfile.mkdtemp in `dir`, made with private_directory's permissions. Returns the Path."""
    if os.name != 'nt':
        return Path(tempfile.mkdtemp(prefix=prefix, dir=dir))
    for _ in range(100):
        path = Path(dir) / (prefix + secrets.token_hex(8))
        try:
            return private_directory(path)
        except FileExistsError:
            continue
    raise FileExistsError(errno.EEXIST, 'No usable temporary folder name found', str(dir))


def _remove_read_only(function, path, _error):
    os.chmod(path, stat.S_IWRITE)
    function(path)


@contextmanager
def private_temporary_directory(prefix, dir):
    """tempfile.TemporaryDirectory in `dir`, made with private_directory's permissions."""
    if os.name != 'nt':
        with tempfile.TemporaryDirectory(prefix=prefix, dir=dir) as path:
            yield path
        return
    path = private_scratch(prefix, dir)
    try:
        yield str(path)
    finally:
        if path.exists():
            shutil.rmtree(path, **({'onexc': _remove_read_only} if sys.version_info >= (3, 12)
                                   else {'onerror': _remove_read_only}))


def publish_directory(source, destination):
    """Same-filesystem atomic publication with no-replace semantics, or fail closed.

    Windows: os.rename refuses existing destinations.
    Darwin: renamex_np(RENAME_EXCL), Linux: renameat2(RENAME_NOREPLACE).
    Unsupported filesystems/libcs fail closed rather than a racy rename fallback.
    """
    source, destination = Path(source), Path(destination)
    try:
        system = platform.system()
        if system == 'Windows':
            os.rename(source, destination)
            return
        libc = ctypes.CDLL(None, use_errno=True)
        if system == 'Darwin' and hasattr(libc, 'renamex_np'):
            function = libc.renamex_np
            function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
            function.restype = ctypes.c_int
            result = function(os.fsencode(source), os.fsencode(destination), 0x00000004)
        elif system == 'Linux' and hasattr(libc, 'renameat2'):
            function = libc.renameat2
            function.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
            function.restype = ctypes.c_int
            result = function(-100, os.fsencode(source), -100, os.fsencode(destination), 1)
        else:
            raise InstallerError('This host lacks a verified atomic no-overwrite publication adapter')
        if result:
            number = ctypes.get_errno()
            raise OSError(number, os.strerror(number))
    except OSError as exc:
        raise InstallerError(f'Cannot publish local result without overwriting ({exc.strerror})') from exc


def stop_process_tree(process, log):
    if os.name == 'nt':
        # Absolute OS utility, never resolved through a player's input/PATH.
        system_root = os.environ.get('SystemRoot')
        if not system_root:
            process.kill()
            raise InstallerError('Windows SystemRoot is unavailable; process-tree cancellation is unverified')
        utility = Path(system_root) / 'System32/taskkill.exe'
        result = subprocess.run([str(utility), '/PID', str(process.pid), '/T', '/F'],
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=10, check=False)
        if result.returncode and process.poll() is None:
            process.kill()
            raise InstallerError('Windows process-tree cancellation failed')
    else:
        try: os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError: pass
        try: process.wait(timeout=2)
        except subprocess.TimeoutExpired: pass
        # Kill the group even if the leader exited first and children ignored TERM.
        try: os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError: pass
    process.wait(timeout=5)
