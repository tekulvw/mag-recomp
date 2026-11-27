"""RPCS3 detection on synthetic folder layouts: every file is a placeholder written by the test."""
import hashlib
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

from mag_installer import rpcs3_detect
from mag_installer.rpcs3_detect import detect, read_sfo


def sfo(**fields):
    """A synthetic PARAM.SFO holding the given string fields (the documented PSF table layout)."""
    keys = b''
    values = b''
    index = b''
    for key, value in fields.items():
        data = value.encode() + b'\0'
        size = (len(data) + 3) // 4 * 4
        index += struct.pack('<HHIII', len(keys), 0x0204, len(data), size, len(values))
        keys += key.encode() + b'\0'
        values += data.ljust(size, b'\0')
    keys = keys.ljust((len(keys) + 3) // 4 * 4, b'\0')
    key_start = 20 + len(index)
    return b'\0PSF' + struct.pack('<IIII', 0x101, key_start, key_start + len(keys), len(fields)) + index + keys + values


def write(path, data=b'synthetic placeholder'):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    return path


def game(folder, title='BCUS98110', category='DG', version='01.00'):
    write(Path(folder) / 'PS3_GAME/PARAM.SFO', sfo(TITLE_ID=title, CATEGORY=category, APP_VER=version, TITLE='Synthetic'))
    return Path(folder) / 'PS3_GAME'


def snapshot(root):
    return {str(p): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else '')
            for p in Path(root).rglob('*')}


class Rpcs3DetectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.home = self.root / 'home'
        self.home.mkdir()
        self.system = self.root / 'system'
        self.system.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def test_param_sfo_fields_are_read(self):
        path = write(self.root / 'PARAM.SFO', sfo(TITLE_ID='BCUS98110', APP_VER='01.00', CATEGORY='DG'))
        self.assertEqual(read_sfo(path), {'TITLE_ID': 'BCUS98110', 'APP_VER': '01.00', 'CATEGORY': 'DG'})
        self.assertEqual(read_sfo(write(self.root / 'other', b'not an sfo')), {})
        self.assertEqual(read_sfo(self.root / 'missing'), {})

    def test_macos_finds_app_support_firmware_and_games_list_but_never_reads_downloads(self):
        data = self.home / 'Library/Application Support/rpcs3'
        write(data / 'dev_flash/sys/external/libsre.sprx')
        write(data / 'dev_flash/vsh/etc/version.txt', 'release:04.9300:\nbuild:synthetic\n')
        disc = game(self.root / 'Games/MAG')
        write(data / 'games.yml', f'BCUS98110: {self.root / "Games/MAG"}/\nBLUS00000: /nowhere/\n')
        write(self.system / 'Applications/RPCS3.app/Contents/MacOS/rpcs3')
        write(self.home / 'Downloads/PS3UPDAT.PUP')
        before = snapshot(self.root)
        found = detect('darwin', {}, self.home, system_root=self.system)
        self.assertEqual(snapshot(self.root), before)  # read-only
        self.assertFalse(found.incomplete)
        self.assertEqual(found.rpcs3, [(self.system / 'Applications/RPCS3.app/Contents/MacOS/rpcs3',)])
        self.assertEqual([d.data for d in found.data], [data])
        self.assertEqual(found.firmware_encrypted, [data / 'dev_flash/sys/external/libsre.sprx'])
        self.assertEqual(found.firmware_decrypted, [])
        self.assertEqual(found.firmware_release, '4.93')
        self.assertEqual(found.best_game().disc, disc)
        self.assertEqual(found.best_game().where, 'RPCS3 games list')
        self.assertEqual(found.pup_files, [])  # Downloads is privacy-protected on macOS

    def test_linux_xdg_flatpak_appimage_and_relocated_folders(self):
        xdg = self.root / 'xdg'
        standard = xdg / 'rpcs3'
        flash = self.root / 'elsewhere/flash'
        write(standard / 'vfs.yml', "$(EmulatorDir): ''\n/dev_flash/: " + str(flash) + "/\n/dev_hdd0/: $(EmulatorDir)dev_hdd0/\n")
        write(flash / 'sys/external/libsre.sprx')
        write(flash / 'sys/external/libsre.prx', b'\x7fELF placeholder')
        write(flash / 'vsh/etc/version.txt', 'release:04.9200:\n')
        disc = game(standard / 'dev_hdd0/disc/MAG')
        update = standard / 'dev_hdd0/game/BCUS98110'
        write(update / 'PARAM.SFO', sfo(TITLE_ID='BCUS98110', CATEGORY='GD', APP_VER='01.04'))
        game(standard / 'dev_hdd0/disc/Other', title='BLUS99999')
        flatpak = self.home / '.var/app/net.rpcs3.RPCS3/config/rpcs3'
        write(flatpak / 'games.yml', '')
        (self.home / '.local/share/flatpak/app/net.rpcs3.RPCS3').mkdir(parents=True)
        image = write(self.home / 'Applications/rpcs3-v0.0.43-synthetic_linux64.AppImage')
        portable = Path(str(image) + '.config') / 'rpcs3'
        write(portable / 'dev_flash/sys/external/libsre.sprx')
        write(self.home / 'Downloads/PS3UPDAT.PUP')
        before = snapshot(self.root)
        found = detect('linux', {'XDG_CONFIG_HOME': str(xdg), 'PATH': ''}, self.home, system_root=self.system)
        self.assertEqual(snapshot(self.root), before)
        kinds = {d.data: d.kind for d in found.data}
        self.assertEqual(kinds, {standard: 'standard', flatpak: 'Flatpak', portable: 'AppImage portable'})
        self.assertIn(('flatpak', 'run', 'net.rpcs3.RPCS3'), found.rpcs3)
        self.assertIn((image,), found.rpcs3)
        self.assertEqual(found.firmware_decrypted, [flash / 'sys/external/libsre.prx'])
        self.assertEqual(found.firmware_encrypted, [flash / 'sys/external/libsre.sprx', portable / 'dev_flash/sys/external/libsre.sprx'])
        self.assertEqual(found.firmware_release, '4.92')
        self.assertEqual(found.pup_files, [self.home / 'Downloads/PS3UPDAT.PUP'])
        self.assertEqual(found.best_game().disc, disc)
        installed = [g for g in found.games if g.disc == update]
        self.assertEqual(len(installed), 1)
        self.assertFalse(installed[0].usable)
        self.assertIn('update or game data', installed[0].note)
        self.assertEqual(len(found.games), 2)  # the other title is not listed

    @unittest.skipIf(os.name == 'nt', 'a PATH lookup on Windows needs a PATHEXT extension')
    def test_linux_default_config_and_rpcs3_on_path(self):
        write(self.home / '.config/rpcs3/dev_flash/sys/external/libsre.sprx')
        binary = write(self.root / 'bin/rpcs3')
        binary.chmod(0o755)
        found = detect('linux', {'PATH': str(self.root / 'bin')}, self.home, system_root=self.system)
        self.assertIn((binary,), found.rpcs3)
        self.assertEqual([d.data for d in found.data], [self.home / '.config/rpcs3'])

    def windows(self, **environ):
        profile = self.root / 'Users/player'
        profile.mkdir(parents=True, exist_ok=True)
        env = {'USERPROFILE': str(profile), 'LOCALAPPDATA': str(profile / 'AppData/Local'),
               'ProgramFiles': str(self.root / 'C/Program Files'), **environ}
        return profile, env

    def test_windows_portable_folders_config_subfolder_and_gui_hints(self):
        profile, env = self.windows()
        drive = self.root / 'C'
        rpcs3 = drive / 'RPCS3'
        write(rpcs3 / 'rpcs3.exe')
        write(rpcs3 / 'dev_flash/sys/external/libsre.sprx')
        write(rpcs3 / 'dev_flash/sys/external/libsre.prx', b'\x7fELF')
        disc = game(drive / 'Games/MAG')
        write(rpcs3 / 'config/games.yml', f'"BCUS98110": "{(drive / "Games/MAG").as_posix()}/"\n')
        write(rpcs3 / 'config/vfs.yml', '$(EmulatorDir): ""\n/dev_hdd0/: $(EmulatorDir)dev_hdd0/\n')
        pups = write(profile / 'Saved/Sony/PS3UPDAT.PUP').parent
        write(rpcs3 / 'GuiConfigs/CurrentSettings.ini', f'[main_window]\nlastExplorePathPUP={pups.as_posix()}\n')
        downloaded = profile / 'Downloads/rpcs3-v0.0.43'
        write(downloaded / 'rpcs3.exe')
        write(downloaded / 'portable/dev_flash/sys/external/libsre.sprx')
        write(profile / 'one/two/three/rpcs3.exe')  # deeper than the bounded search
        before = snapshot(self.root)
        found = detect('win32', env, profile, drives=[drive])
        self.assertEqual(snapshot(self.root), before)
        self.assertIn((rpcs3 / 'rpcs3.exe',), found.rpcs3)
        self.assertIn((downloaded / 'rpcs3.exe',), found.rpcs3)
        self.assertNotIn((profile / 'one/two/three/rpcs3.exe',), found.rpcs3)
        self.assertEqual({d.data: d.kind for d in found.data},
                         {rpcs3: 'next to rpcs3.exe', downloaded / 'portable': 'portable'})
        self.assertEqual(found.firmware_decrypted, [rpcs3 / 'dev_flash/sys/external/libsre.prx'])
        self.assertEqual(found.best_game().disc, disc)
        self.assertEqual(found.pup_files, [pups / 'PS3UPDAT.PUP'])

    def test_windows_config_dir_variable_and_program_files(self):
        data = self.root / 'rpcs3-data'
        write(data / 'dev_flash/sys/external/libsre.sprx')
        profile, env = self.windows(RPCS3_CONFIG_DIR=str(data))
        exe = write(self.root / 'C/Program Files/RPCS3/rpcs3.exe')
        found = detect('win32', env, profile, drives=[])
        self.assertEqual(found.rpcs3, [(exe,)])
        self.assertEqual([d.data for d in found.data], [data])
        self.assertEqual(found.firmware_encrypted, [data / 'dev_flash/sys/external/libsre.sprx'])

    def test_windows_search_does_not_follow_links_or_enter_system_folders(self):
        profile, env = self.windows()
        target = self.root / 'outside'
        write(target / 'rpcs3.exe')
        try:
            (profile / 'linked').symlink_to(target, target_is_directory=True)
        except OSError:
            self.skipTest('symbolic links are unavailable')
        write(profile / 'AppData/rpcs3.exe')
        found = detect('win32', env, profile, drives=[])
        self.assertEqual(found.rpcs3, [])

    def test_games_list_disc_image_is_reported_not_suggested(self):
        data = self.home / '.config/rpcs3'
        image = write(self.root / 'MAG.iso')
        write(data / 'games.yml', f'BCUS98110: {image}\n')
        found = detect('linux', {'PATH': ''}, self.home, system_root=self.system)
        self.assertIsNone(found.best_game())
        self.assertIn('disc image', found.games[0].note)

    def test_entry_limit_stops_the_search_and_says_so(self):
        profile, env = self.windows()
        for index in range(40):
            (profile / f'folder{index}').mkdir()
        found = detect('win32', env, profile, drives=[], max_entries=10)
        self.assertTrue(found.incomplete)
        self.assertTrue(found.to_json()['incomplete'])

    def test_time_limit_stops_the_search_and_says_so(self):
        profile, env = self.windows()
        for index in range(5):
            (profile / f'folder{index}').mkdir()
        ticks = iter(range(1000))
        found = detect('win32', env, profile, drives=[], time_limit=3, clock=lambda: next(ticks))
        self.assertTrue(found.incomplete)

    def test_a_stuck_file_system_call_is_abandoned_after_the_time_limit(self):
        def stuck(result, *args):
            result.data.append('partial')
            time.sleep(3)
        with patch.object(rpcs3_detect, '_detect', stuck):
            started = time.monotonic()
            found = detect('linux', {}, self.home, time_limit=0.2)
            self.assertLess(time.monotonic() - started, 2)
        self.assertTrue(found.incomplete)
        self.assertEqual(found.data, ['partial'])

    def test_nothing_found_on_an_empty_home(self):
        for platform in ('darwin', 'linux', 'win32'):
            with self.subTest(platform=platform):
                found = detect(platform, {'PATH': '', 'USERPROFILE': str(self.home)}, self.home, drives=[],
                               system_root=self.system)
                self.assertFalse(found.rpcs3_found())
                self.assertEqual((found.games, found.firmware_encrypted, found.firmware_decrypted), ([], [], []))
                self.assertFalse(found.incomplete)


if __name__ == '__main__':
    unittest.main()
