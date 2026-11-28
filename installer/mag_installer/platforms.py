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


# --- Updating a MAG folder in place (update.py) -------------------------------------------------

def running_executables():
    """The executable paths of the processes this account can see, as resolved strings.

    Used to refuse an update while MAG runs from the folder being updated. Raises OSError when
    the host's process list cannot be read, so the caller can say that the check did not run."""
    system = platform.system()
    if system == 'Linux':
        found = []
        for entry in os.listdir('/proc'):
            if not entry.isdigit():
                continue
            try:
                found.append(os.path.realpath(os.readlink(f'/proc/{entry}/exe')))
            except OSError:
                continue  # exited, or another account's process
        return found
    if system == 'Darwin':
        libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
        libproc.proc_listpids.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
        libproc.proc_listpids.restype = ctypes.c_int
        libproc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        libproc.proc_pidpath.restype = ctypes.c_int
        needed = libproc.proc_listpids(1, 0, None, 0)  # PROC_ALL_PIDS
        if needed <= 0:
            raise OSError(ctypes.get_errno(), 'Cannot list processes')
        pids = (ctypes.c_int * (needed // ctypes.sizeof(ctypes.c_int) + 256))()
        count = libproc.proc_listpids(1, 0, pids, ctypes.sizeof(pids)) // ctypes.sizeof(ctypes.c_int)
        buffer = ctypes.create_string_buffer(4096)
        found = []
        for pid in pids[:max(count, 0)]:
            if pid > 0 and libproc.proc_pidpath(pid, buffer, ctypes.sizeof(buffer)) > 0:
                found.append(os.path.realpath(os.fsdecode(buffer.value)))
        return found
    if system == 'Windows':
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)

        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                        ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD),
                        ('cntThreads', wintypes.DWORD), ('th32ParentProcessID', wintypes.DWORD),
                        ('pcPriClassBase', wintypes.LONG), ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]
        kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
        kernel.Process32FirstW.restype = kernel.Process32NextW.restype = wintypes.BOOL
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                      ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        snapshot = kernel.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
        if not snapshot or snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        found = []
        try:
            entry = Entry(); entry.dwSize = ctypes.sizeof(Entry)
            more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while more:
                process = kernel.OpenProcess(0x1000, False, entry.th32ProcessID)  # QUERY_LIMITED_INFORMATION
                if process:
                    try:
                        buffer = ctypes.create_unicode_buffer(32768); size = wintypes.DWORD(len(buffer))
                        if kernel.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(size)):
                            found.append(os.path.realpath(buffer.value))
                    finally:
                        kernel.CloseHandle(process)
                more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            kernel.CloseHandle(snapshot)
        return found
    raise OSError(errno.ENOSYS, f'No process list adapter for {system}')


def lock_is_free(path):
    """Whether no process holds a lock on an existing lock file; the file is never created.

    MAG's launcher holds such a lock while it runs, so an exclusive attempt fails then. True when
    the file does not exist."""
    flags = os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    try:
        handle = os.open(path, flags)
    except FileNotFoundError:
        return True
    try:
        if os.name == 'nt':
            import msvcrt
            try:
                msvcrt.locking(handle, msvcrt.LK_NBLCK, 1)
            except OSError:
                return False
            os.lseek(handle, 0, os.SEEK_SET)
            msvcrt.locking(handle, msvcrt.LK_UNLCK, 1)
            return True
        import fcntl
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return False
        fcntl.flock(handle, fcntl.LOCK_UN)
        return True
    finally:
        os.close(handle)


def _windows_security():
    from ctypes import wintypes
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    advapi.GetNamedSecurityInfoW.argtypes = [wintypes.LPCWSTR, ctypes.c_int, wintypes.DWORD, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p,
                                             ctypes.POINTER(ctypes.c_void_p)]
    advapi.GetNamedSecurityInfoW.restype = wintypes.DWORD
    advapi.SetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, ctypes.c_int, wintypes.DWORD, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    advapi.SetNamedSecurityInfoW.restype = wintypes.DWORD
    advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.ULONG)]
    advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW.restype = wintypes.BOOL
    advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.ULONG)]
    advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = wintypes.BOOL
    advapi.GetSecurityDescriptorDacl.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.BOOL),
                                                 ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.BOOL)]
    advapi.GetSecurityDescriptorDacl.restype = wintypes.BOOL
    advapi.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    advapi.OpenProcessToken.restype = wintypes.BOOL
    advapi.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                           ctypes.POINTER(wintypes.DWORD)]
    advapi.GetTokenInformation.restype = wintypes.BOOL
    advapi.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    advapi.ConvertSidToStringSidW.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    return advapi, kernel


SE_FILE_OBJECT, DACL_SECURITY_INFORMATION = 1, 4
PROTECTED_DACL, UNPROTECTED_DACL = 0x80000000, 0x20000000


def folder_dacl(path):
    """Windows: the DACL of `path` (a folder or file) as its SDDL string, "D:<flags>(ace)(ace)..."."""
    from ctypes import wintypes
    advapi, kernel = _windows_security()
    descriptor = ctypes.c_void_p()
    error = advapi.GetNamedSecurityInfoW(str(path), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, None, None, None, None,
                                         ctypes.byref(descriptor))
    if error:
        raise ctypes.WinError(error)
    try:
        text, length = ctypes.c_void_p(), wintypes.ULONG()
        if not advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, 1, DACL_SECURITY_INFORMATION,
                                                                          ctypes.byref(text), ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return ctypes.wstring_at(text.value)
        finally:
            kernel.LocalFree(text)
    finally:
        kernel.LocalFree(descriptor)


def dacl_parts(sddl):
    """(flags, sorted ACE strings) of a DACL SDDL string from folder_dacl."""
    import re
    if not sddl.startswith('D:'):
        raise InstallerError(f'Unexpected folder permissions: {sddl}')
    body = sddl[2:]
    return body.split('(', 1)[0], sorted(re.findall(r'\(([^)]*)\)', body))


def apply_explicit_dacl(path, sddl):
    """Windows: give `path` the explicit (not inherited) entries and the protection of `sddl`.

    Its inherited entries then come from the folder `path` is in now, and its children inherit
    again from it."""
    from ctypes import wintypes
    advapi, kernel = _windows_security()
    flags, aces = dacl_parts(sddl)
    explicit = [ace for ace in aces if 'ID' not in ace.split(';')[1]]
    descriptor, size = ctypes.c_void_p(), wintypes.ULONG()
    if not advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            'D:' + flags + ''.join(f'({ace})' for ace in explicit), 1, ctypes.byref(descriptor), ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        present, defaulted, dacl = wintypes.BOOL(), wintypes.BOOL(), ctypes.c_void_p()
        if not advapi.GetSecurityDescriptorDacl(descriptor, ctypes.byref(present), ctypes.byref(dacl), ctypes.byref(defaulted)):
            raise ctypes.WinError(ctypes.get_last_error())
        protected = 'P' in flags.replace('AI', '').replace('AR', '')
        error = advapi.SetNamedSecurityInfoW(str(path), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION | (PROTECTED_DACL if protected else UNPROTECTED_DACL),
                                             None, None, dacl, None)
        if error:
            raise ctypes.WinError(error)
    finally:
        kernel.LocalFree(descriptor)


def current_user_sid():
    """Windows: the string SID of the account running the installer."""
    from ctypes import wintypes
    advapi, kernel = _windows_security()
    token = wintypes.HANDLE()
    if not advapi.OpenProcessToken(kernel.GetCurrentProcess(), 0x0008, ctypes.byref(token)):  # TOKEN_QUERY
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        size = wintypes.DWORD()
        advapi.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))  # TokenUser
        buffer = ctypes.create_string_buffer(size.value)
        if not advapi.GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        sid = ctypes.c_void_p.from_buffer(buffer).value  # TOKEN_USER.User.Sid comes first
        text = ctypes.c_void_p()
        if not advapi.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return ctypes.wstring_at(text.value)
        finally:
            kernel.LocalFree(text)
    finally:
        kernel.CloseHandle(token)


def grants_full_control(aces, sid):
    """An allow entry for `sid` with full access that applies to the object itself (not inherit-only)."""
    names = {sid} | ({'LA'} if sid.endswith('-500') else set())
    for ace in aces:
        parts = ace.split(';')
        if len(parts) < 6:
            continue
        kind, flags, rights, trustee = parts[0], parts[1], parts[2], parts[5]
        if kind == 'A' and trustee in names and 'IO' not in flags and rights.upper() in ('FA', 'GA', '0X1F01FF'):
            return True
    return False
