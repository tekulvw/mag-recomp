"""Permissions of the folders the installer makes for the player.

On Windows the MAG folder, its app and game folders and the build folder must inherit their parent
folder's permissions, exactly like a folder made in place, even though the MAG folder is assembled
in the build folder and renamed into place. On macOS and Linux they stay owner-only.

Synthetic only: tiny generated files stand in for the app, the game files and the tools.
"""
import ctypes
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import unittest

from mag_installer import platforms, player_build
from mag_installer.native_build import link_tree
from mag_installer.safety import Cancellation

INSTALLER = Path(__file__).resolve().parents[1] / 'mag_installer'
# Modules whose folders a Windows player build makes; each must go through platforms' helpers.
WINDOWS_BUILD_MODULES = ('player_build.py', 'native_build.py', 'native_package_windows.py', 'pipeline.py',
                         'managed_tools.py', 'build_cache.py', 'tools.py')


def dacl(path):
    """The DACL of `path` as (flags, sorted ACE strings), read from its SDDL."""
    from ctypes import wintypes
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    get = advapi.GetNamedSecurityInfoW
    get.argtypes = [wintypes.LPCWSTR, ctypes.c_int, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p,
                    ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    get.restype = wintypes.DWORD
    convert = advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW
    convert.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p),
                        ctypes.POINTER(wintypes.ULONG)]
    convert.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, SDDL_REVISION_1 = 1, 4, 1
    descriptor = ctypes.c_void_p()
    error = get(str(path), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, None, None, None, None, ctypes.byref(descriptor))
    if error:
        raise ctypes.WinError(error)
    try:
        text, length = ctypes.c_void_p(), wintypes.ULONG()
        if not convert(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, ctypes.byref(text), ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            sddl = ctypes.wstring_at(text.value)
        finally:
            kernel.LocalFree(text)
    finally:
        kernel.LocalFree(descriptor)
    if not sddl.startswith('D:'):
        raise AssertionError(f'Unexpected SDDL for {path}: {sddl}')
    body = sddl[2:]
    flags = body.split('(', 1)[0]
    return flags, sorted(re.findall(r'\(([^)]*)\)', body))


def current_user_sid():
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'whoami.exe'
    line = subprocess.run([str(system), '/user', '/fo', 'csv', '/nh'], capture_output=True, text=True,
                          check=True).stdout.strip()
    return line.rsplit(',', 1)[1].strip('"')


def grants_full_control(aces, sid):
    """An allow ACE for `sid` with full access that applies to the object itself (not inherit-only)."""
    names = {sid} | ({'LA'} if sid.endswith('-500') else set())
    for ace in aces:
        kind, flags, rights, _object, _inherited, trustee = ace.split(';')[:6]
        if kind == 'A' and trustee in names and 'IO' not in flags and rights.upper() in ('FA', 'GA', '0X1F01FF'):
            return True
    return False


@unittest.skipUnless(os.name == 'nt', 'Windows folder permissions')
class WindowsFolderPermissionTests(unittest.TestCase):
    def setUp(self):
        # A parent made with the default permissions, like the player's own profile folder: under the
        # user's temporary folder, which inherits from the profile. tempfile.mkdtemp would itself be
        # protected (mode 0o700), so it cannot stand in for the player's chosen parent folder.
        self.parent = (Path(tempfile.gettempdir()) / ('mag-acl-' + secrets.token_hex(6))).resolve()
        self.parent.mkdir()
        self.addCleanup(shutil.rmtree, self.parent, ignore_errors=True)
        # The reference: what a folder and a file made in place in the parent get.
        self.control = self.parent / 'made-in-place'; self.control.mkdir()
        (self.control / 'file.bin').write_bytes(b'control')
        self.sid = current_user_sid()

    def assertInheritsLikeInPlace(self, path, reference=None):
        reference = reference or self.control
        flags, aces = dacl(path)
        expected_flags, expected = dacl(reference)
        self.assertNotIn('P', flags, f'{path} has a protected DACL: D:{flags}{"".join(f"({a})" for a in aces)}')
        self.assertTrue(all('ID' in ace.split(';')[1] for ace in aces), f'{path} has explicit entries: {aces}')
        self.assertEqual(aces, expected, f'{path} differs from a folder made in place')
        if grants_full_control(expected, self.sid):
            self.assertTrue(grants_full_control(aces, self.sid), f'{path} lost the player\'s full control: {aces}')

    def test_python_owner_only_mode_would_protect_the_folder(self):
        # The cause this guards against: mode=0o700 replaces inheritance with SYSTEM/Administrators/owner.
        if sys.version_info < (3, 12, 4):
            self.skipTest('This Python ignores mkdir modes on Windows')
        strict = self.parent / 'strict'; strict.mkdir(mode=0o700)
        flags, aces = dacl(strict)
        self.assertIn('P', flags)
        self.assertTrue(any(ace.endswith(';OW') for ace in aces), aces)

    def test_published_mag_folder_inherits_from_its_parent(self):
        token = Cancellation()
        output, build = self.parent / 'MAG', player_build.build_folder(self.parent / 'MAG')
        platforms.private_directory(build)
        work = platforms.private_directory(build / 'build-work')
        game = work / 'workspace' / 'game'; (game / 'PS3_GAME' / 'USRDIR').mkdir(parents=True)
        (game / 'PS3_GAME' / 'USRDIR' / 'data.bin').write_bytes(b'synthetic game data')
        stage = platforms.private_directory(build / 'package')
        app = platforms.private_directory(stage / 'MAG')      # as native_package_windows.package makes it
        (app / 'bin').mkdir(); (app / 'MAG.exe').write_bytes(b'MZ synthetic')
        link_tree(game, stage / 'game', token)                 # as native_package.stage_game links them
        player_build._publish(output, build, {'state': 'building'}, 'MAG')
        self.assertEqual(sorted(p.name for p in output.iterdir()), ['MAG', 'game'])
        for path in (output, output / 'MAG', output / 'MAG' / 'bin', output / 'game', build, build / 'build-work'):
            self.assertInheritsLikeInPlace(path)
        reference = self.control / 'file.bin'
        for path in (output / 'MAG' / 'MAG.exe', output / 'game' / 'PS3_GAME' / 'USRDIR' / 'data.bin'):
            self.assertInheritsLikeInPlace(path, reference)
        # The launcher makes home/ and logs/ in place at first start; they inherit the same way.
        (output / 'home').mkdir(); (output / 'logs').mkdir()
        self.assertInheritsLikeInPlace(output / 'home'); self.assertInheritsLikeInPlace(output / 'logs')

    def test_scratch_moved_into_the_build_folder_inherits_from_it(self):
        build = platforms.private_directory(self.parent / 'MAG-build')
        with platforms.private_temporary_directory('.managed-', build) as scratch:
            self.assertInheritsLikeInPlace(scratch)
            payload = Path(scratch) / 'payload'; payload.mkdir(); (payload / 'tool.exe').write_bytes(b'MZ')
            payload.rename(build / 'tool')
            read_only = Path(scratch) / 'left.txt'; read_only.write_text('x'); read_only.chmod(0o444)
        self.assertFalse(Path(scratch).exists())
        self.assertInheritsLikeInPlace(build / 'tool')
        extract = platforms.private_scratch('.extract-', build)
        extract.rename(build / 'extracted')
        self.assertInheritsLikeInPlace(build / 'extracted')


@unittest.skipIf(os.name == 'nt', 'POSIX modes')
class PosixFolderPermissionTests(unittest.TestCase):
    def test_player_folders_stay_owner_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual(platforms.private_directory(root / 'MAG-build').stat().st_mode & 0o777, 0o700)
            platforms.private_directory(root / 'cache' / 'build', parents=True)
            self.assertEqual((root / 'cache' / 'build').stat().st_mode & 0o777, 0o700)
            self.assertEqual(platforms.private_scratch('.extract-', root).stat().st_mode & 0o777, 0o700)
            with platforms.private_temporary_directory('.managed-', root) as scratch:
                self.assertEqual(Path(scratch).stat().st_mode & 0o777, 0o700)
            self.assertFalse(Path(scratch).exists())


class FolderCreationRouteTests(unittest.TestCase):
    def test_windows_build_folders_are_made_through_the_permission_helpers(self):
        # mode=0o700, mkdtemp and TemporaryDirectory protect the folder on Windows (see platforms.py).
        direct = re.compile(r'mode\s*=\s*0o700|mkdtemp\(|TemporaryDirectory\(')
        for name in WINDOWS_BUILD_MODULES:
            source = (INSTALLER / name).read_text(encoding='utf-8')
            self.assertEqual([line.strip() for line in source.splitlines() if direct.search(line)], [], name)


if __name__ == '__main__':
    unittest.main()
