"""Run the actual Windows supervisor with synthetic launcher/engine images."""
import hashlib
from contextlib import contextmanager
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from mag_installer import native_package_windows as package
from mag_installer.managed_tools import msvc_environment, windows_toolchain_probe


@contextmanager
def private_desktop():
    # Even a failing supervisor must not put a test error dialog on the user's
    # desktop. Use the same native desktop mechanism as the settings fixture.
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR,
                                      ctypes.c_void_p, wintypes.DWORD,
                                      wintypes.DWORD, ctypes.c_void_p]
    user32.CreateDesktopW.restype = wintypes.HANDLE
    user32.CloseDesktop.argtypes = [wintypes.HANDLE]
    name = 'mag-package-' + os.urandom(8).hex()
    desktop = user32.CreateDesktopW(name, None, None, 0, 0x10000000, None)
    if not desktop:
        raise ctypes.WinError(ctypes.get_last_error())
    startup = subprocess.STARTUPINFO(); startup.lpDesktop = name
    try:
        yield startup
    finally:
        user32.CloseDesktop(desktop)


@unittest.skipUnless(os.name == 'nt', 'Native Windows supervisor')
class WindowsPackageTests(unittest.TestCase):
    def test_writable_game_data_survives_handoff_overlay_cleanup_and_relaunch(self):
        toolchain, detail = windows_toolchain_probe()
        if not toolchain:
            self.skipTest(detail)
        env = msvc_environment(toolchain['vcvars'])
        with tempfile.TemporaryDirectory(prefix='mag-package-') as temp:
            root = Path(temp)
            install = root / 'install'; install.mkdir()
            workspace = install / 'workspace'
            profiles = workspace / 'profiles'; profiles.mkdir(parents=True)
            disc = workspace / 'local/retail-v1.0/PS3_GAME/USRDIR'
            (disc / 'bin').mkdir(parents=True)
            elf = bytearray(136); elf[:7] = b'\x7fELF\x02\x02\x01'
            struct.pack_into('>Q', elf, 32, 64)
            struct.pack_into('>HH', elf, 54, 56, 1)
            struct.pack_into('>I', elf, 64, 1)
            struct.pack_into('>QQ', elf, 72, 120, 0x32A660)
            struct.pack_into('>Q', elf, 96, 16)
            elf[120:136] = bytes(range(16))
            (disc / 'EBOOT.elf').write_bytes(elf)
            (disc / 'bin/mage_g.elf').write_bytes(b'synthetic engine ELF')
            (disc / 'bin/mage_g.self').write_bytes(b'synthetic engine SELF')
            source = root / 'fixture.c'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
int main(void) {
 const char *hdd0=getenv("PS3_HDD0_ROOT"), *hdd1=getenv("PS3_HDD1_ROOT");
 if(!hdd0||!hdd1||!strcmp(hdd0,"inherited")||!strcmp(hdd1,"inherited"))return 41;
 /* The pinned cellGame implementation has 128-byte host path buffers. */
 char path[128];int length=snprintf(path,sizeof path,"%s/game/BCUS98110/USRDIR",hdd0);
 if(length<0||length>=(int)sizeof path)return 42;
 char parent[128];snprintf(parent,sizeof parent,"%s/game",hdd0);_mkdir(parent);
 snprintf(parent,sizeof parent,"%s/game/BCUS98110",hdd0);_mkdir(parent);_mkdir(path);
 char marker[256];snprintf(marker,sizeof marker,"%s/persist.txt",path);
 const char *request=getenv("MAG_HANDOFF_REQUEST");
 FILE *f=fopen(marker,request?"a":"r");if(!f)return 43;
 if(request)fputs("launcher\n",f);else if(fgetc(f)!='l')return 44;fclose(f);
 snprintf(marker,sizeof marker,"%s/cache-probe.txt",hdd1);f=fopen(marker,"w");if(!f)return 45;fclose(f);
 f=fopen(request?"launcher-storage.txt":"engine-storage.txt","w");if(!f)return 46;
 fprintf(f,"%s\n%s\n%s\n",hdd0,hdd1,getenv("MAG_PLAYER_OVERLAY_ROOT"));fclose(f);
 if(request){f=fopen(request,"wbx");if(!f)return 47;
 fprintf(f,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n",getenv("MAG_HANDOFF_TOKEN"));fclose(f);return 90;}
 return 23;
}
''')
            fixture = root / 'fixture.exe'
            subprocess.run([toolchain['clang_cl'], '/nologo', '/MT',
                            '/D_CRT_SECURE_NO_WARNINGS', str(source),
                            '/Fo' + str(root / 'fixture.obj'), '/Fe' + str(fixture)],
                           env=env, check=True, capture_output=True, timeout=60)
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            images = {}
            for folder, image, path in [('build', 'EBOOT', disc / 'EBOOT.elf'),
                                         ('build-engine', 'mage_g', disc / 'bin/mage_g.elf')]:
                binary = workspace / folder / 'MagRecomp.exe'
                binary.parent.mkdir(); binary.write_bytes(fixture.read_bytes())
                record = dict(image_name=image, binary_sha256=digest(binary),
                              elf_sha256=digest(path), sdk_revision='a' * 40)
                binary.with_suffix('.receipt.json').write_text(json.dumps(record))
                images[image] = dict(elf_sha256=digest(path), source_sha256=digest(disc / 'bin/mage_g.self'))
            (profiles / 'retail-v1.0.json').write_text(json.dumps(dict(images=images)))
            (profiles / 'framework.json').write_text(json.dumps(dict(
                strict_runtime_revisions=['a' * 40], strict_hle_runtime_revisions=['a' * 40])))
            app = package.package({'msvc': toolchain}, workspace, install / 'MAG', environment=env)
            launch_env = dict(env, APPDATA=str(root / 'preferences'),
                              PS3_HDD0_ROOT='inherited', PS3_HDD1_ROOT='inherited')
            original = digest(disc / 'bin/mage_g.self')
            with private_desktop() as startup:
                for _ in range(2):
                    result = subprocess.run([str(app / 'MAG.exe')], env=launch_env,
                                            startupinfo=startup, timeout=15)
                    self.assertEqual(result.returncode, 23)
            logs = sorted((install / 'logs').glob('launch-*'))
            self.assertEqual(len(logs), 2)
            for log in logs:
                launcher = (log / 'launcher-storage.txt').read_text().splitlines()
                engine = (log / 'engine-storage.txt').read_text().splitlines()
                self.assertEqual(launcher, engine)
                self.assertTrue(Path(launcher[0]).is_relative_to(install / 'home'))
                self.assertTrue(Path(launcher[1]).is_relative_to(install / 'home'))
                self.assertGreater(len(str(log / 'player-overlay/vfs/game/BCUS98110/USRDIR')), 128)
                self.assertFalse(Path(launcher[2]).exists())
            marker = Path(launcher[0]) / 'game/BCUS98110/USRDIR/persist.txt'
            self.assertEqual(marker.read_text().splitlines(), ['launcher', 'launcher'])
            self.assertEqual(digest(disc / 'bin/mage_g.self'), original)


if __name__ == '__main__':
    unittest.main()
