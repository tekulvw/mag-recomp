"""Actual Win32 dialog transactions in a synthetic process, without game files."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'installer'))


@unittest.skipUnless(os.name == 'nt', 'Native Win32 fixture')
class WindowsSettingsTests(unittest.TestCase):
    def test_native_dialogs_keep_frames_alive_and_preserve_transactions(self):
        from mag_installer.managed_tools import windows_toolchain_probe, msvc_environment
        toolchain, detail = windows_toolchain_probe()
        if not toolchain:
            self.skipTest(detail)
        environment = msvc_environment(toolchain['vcvars'])
        with tempfile.TemporaryDirectory(prefix='mag-win-settings-') as temp:
            binary = Path(temp) / 'settings.exe'
            subprocess.run([
                toolchain['clang_cl'], '/nologo', '/std:c++20', '/EHsc', '/MT',
                '/W4', '/WX', '/D_CRT_SECURE_NO_WARNINGS', '/Isrc',
                'src/player_settings_portable.cpp', 'src/player_settings_portable_runtime.cpp',
                'src/player_settings_windows.cpp', 'tests/player_settings_windows_fixture.cpp',
                '/Fo' + temp + '\\', '/Fe' + str(binary), '/link',
                'user32.lib', 'gdi32.lib', 'xinput.lib', 'bcrypt.lib', 'ws2_32.lib',
            ], cwd=ROOT, env=environment, check=True, timeout=120)
            # Keep synthetic dialogs off the user's input desktop. Every UI
            # thread inherits this desktop, so a human Enter/Keep cannot race
            # the real 15-second confirmation test.
            import ctypes
            from ctypes import wintypes
            user32 = ctypes.WinDLL('user32', use_last_error=True)
            user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR,
                                              ctypes.c_void_p, wintypes.DWORD,
                                              wintypes.DWORD, ctypes.c_void_p]
            user32.CreateDesktopW.restype = wintypes.HANDLE
            user32.CloseDesktop.argtypes = [wintypes.HANDLE]
            user32.CloseDesktop.restype = wintypes.BOOL
            desktop_name = Path(temp).name
            desktop = user32.CreateDesktopW(desktop_name, None, None, 0, 0x10000000, None)
            if not desktop:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                startup = subprocess.STARTUPINFO()
                startup.lpDesktop = desktop_name
                subprocess.run([str(binary), str(Path(temp) / 'private')],
                               env=environment, startupinfo=startup, check=True, timeout=75)
            finally:
                user32.CloseDesktop(desktop)


if __name__ == '__main__':
    unittest.main()
