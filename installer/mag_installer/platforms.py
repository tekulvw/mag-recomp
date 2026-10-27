"""Small host adapters. OS branches require certification on each target OS."""
import ctypes
import errno
import os
from pathlib import Path
import platform
import signal
import subprocess

from .model import InstallerError


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
