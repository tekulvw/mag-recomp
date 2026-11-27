"""The preparation guide and immediate input checks, on synthetic placeholder files and identities."""
import hashlib
import io
import json
import shlex
from contextlib import redirect_stdout
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from mag_installer import input_guide, player_build
from mag_installer.input_guide import (DONE, PROBLEM, TODO, build_guide, check_firmware, check_game, load_reviewed,
                                       render, survey)
from mag_installer.model import InstallerError
from mag_installer.rpcs3_detect import Detection, GameCandidate

from test_rpcs3_detect import sfo, write


def sha(data):
    return hashlib.sha256(data).hexdigest()


EBOOT, EBOOT_ELF = b'SCE\x00 synthetic eboot', b'\x7fELF synthetic eboot'
ENGINE, ENGINE_ELF = b'SCE\x00 synthetic engine', b'\x7fELF synthetic engine'
SETTINGS = b'synthetic settings archive'
FIRMWARE = b'\x7fELF synthetic firmware module'
REVIEWED = {'schema_version': 1,
            'executables': {'EBOOT.BIN': [sha(EBOOT), sha(EBOOT_ELF)], 'bin/mage_g.self': [sha(ENGINE), sha(ENGINE_ELF)]},
            'firmware': [{'release': '4.93', 'sha256': sha(FIRMWARE)}],
            'settings_archive': {'path': 'run/mag/ps3/game.psarc.sdat', 'sha256': [sha(SETTINGS)]}}
RPCS3 = ['/Applications/RPCS3.app/Contents/MacOS/rpcs3']


def prepared(root, decrypted=True, **sfo_fields):
    disc = Path(root) / 'PS3_GAME'
    fields = {'TITLE_ID': 'BCUS98110', 'CATEGORY': 'DG', 'APP_VER': '01.00', **sfo_fields}
    write(disc / 'PARAM.SFO', sfo(**fields))
    write(disc / 'USRDIR/EBOOT.BIN', EBOOT)
    write(disc / 'USRDIR/bin/mage_g.self', ENGINE)
    write(disc / 'USRDIR/run/mag/ps3/game.psarc.sdat', SETTINGS)
    if decrypted:
        write(disc / 'USRDIR/EBOOT.elf', EBOOT_ELF)
        write(disc / 'USRDIR/bin/mage_g.elf', ENGINE_ELF)
    return disc


def codes(check):
    return [f.code for f in check.findings]


class InputCheckTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()

    def tearDown(self):
        self.temp.cleanup()

    def test_complete_v1_folder_passes_from_its_parent_or_ps3_game(self):
        disc = prepared(self.root / 'MAG')
        for chosen in (self.root / 'MAG', disc):
            check = check_game(chosen, REVIEWED, RPCS3, 'macos')
            self.assertTrue(check.ok, check.findings)
            self.assertEqual(check.disc, disc)
            self.assertIn('BCUS98110 version 01.00', check.findings[0].message)

    def test_missing_decrypted_executables_give_the_exact_decrypt_command(self):
        disc = prepared(self.root / 'My Games/MAG', decrypted=False)
        check = check_game(self.root / 'My Games/MAG', REVIEWED, RPCS3, 'macos')
        self.assertEqual(codes(check), ['game-elf-missing'])
        finding = check.findings[0]
        self.assertEqual(finding.level, TODO)
        self.assertEqual(finding.commands, [f"{RPCS3[0]} --decrypt {shlex.quote(str(disc / 'USRDIR/EBOOT.BIN'))} "
                                            f"--decrypt {shlex.quote(str(disc / 'USRDIR/bin/mage_g.self'))}"])
        self.assertIn('beside the originals', finding.message)
        windows = check_game(self.root / 'My Games/MAG', REVIEWED, [r'C:\RPCS3\rpcs3.exe'], 'windows').findings[0].commands[0]
        self.assertTrue(windows.startswith(r'C:\RPCS3\rpcs3.exe --decrypt "'), windows)

    def test_each_executable_failure_has_its_own_fix(self):
        cases = ((b'SCE\x00 renamed original', 'game-elf-encrypted', 'still encrypted'),
                 (b'plain text', 'game-elf-not-elf', 'not a decrypted program'),
                 (b'\x7fELF another release', 'game-elf-other-release', 'another release'))
        for content, code, text in cases:
            with self.subTest(code=code):
                disc = prepared(self.root / code)
                (disc / 'USRDIR/EBOOT.elf').write_bytes(content)
                check = check_game(disc, REVIEWED, RPCS3, 'linux')
                self.assertEqual(codes(check), [code])
                self.assertEqual(check.findings[0].level, PROBLEM)
                self.assertIn(text, check.findings[0].message)
                self.assertIn('--decrypt', check.findings[0].commands[0])

    def test_original_files_are_checked_before_their_decrypted_copies(self):
        disc = prepared(self.root / 'changed')
        (disc / 'USRDIR/EBOOT.BIN').write_bytes(b'SCE\x00 an update')
        self.assertEqual(codes(check_game(disc, REVIEWED, RPCS3, 'linux')), ['game-original-mismatch'])
        (disc / 'USRDIR/EBOOT.BIN').write_bytes(EBOOT_ELF)
        check = check_game(disc, REVIEWED, RPCS3, 'linux')
        self.assertEqual(codes(check), ['game-original-decrypted'])
        self.assertIn('already decrypted', check.findings[0].message)

    def test_param_sfo_names_other_games_updates_and_versions(self):
        for fields, code in (({'TITLE_ID': 'BLES00000'}, 'game-other-title'), ({'CATEGORY': 'GD'}, 'game-not-disc'),
                             ({'APP_VER': '01.04'}, 'game-other-version')):
            with self.subTest(code=code):
                disc = prepared(self.root / code, **fields)
                self.assertIn(code, codes(check_game(disc, REVIEWED, RPCS3, 'linux')))

    def test_wrong_folders_say_which_folder_to_choose(self):
        disc = prepared(self.root / 'MAG')
        self.assertIn('inside the game folder', check_game(disc / 'USRDIR', REVIEWED, RPCS3, 'linux').findings[0].message)
        self.assertIn('is a file', check_game(disc / 'PARAM.SFO', REVIEWED, RPCS3, 'linux').findings[0].message)
        self.assertIn('has no PS3_GAME', check_game(self.root, REVIEWED, RPCS3, 'linux').findings[0].message)
        (disc / 'USRDIR/run/mag/ps3/game.psarc.sdat').unlink()
        self.assertEqual(codes(check_game(disc, REVIEWED, RPCS3, 'linux')), ['game-incomplete'])
        write(disc / 'USRDIR/run/mag/ps3/game.psarc.sdat', b'changed settings')
        self.assertEqual(codes(check_game(disc, REVIEWED, RPCS3, 'linux')), ['game-settings-changed'])

    def test_without_reviewed_identities_only_the_structure_is_checked(self):
        disc = prepared(self.root / 'MAG')
        check = check_game(disc, None, RPCS3, 'linux')
        self.assertTrue(check.ok)
        self.assertIn('fully checked when you click Build MAG', check.findings[0].message)
        (disc / 'USRDIR/bin/mage_g.elf').write_bytes(ENGINE)
        self.assertEqual(codes(check_game(disc, None, RPCS3, 'linux')), ['game-elf-encrypted'])

    def test_firmware_failures_have_their_own_fix(self):
        path = self.root / 'libsre.prx'
        sprx = self.root / 'rpcs3/dev_flash/sys/external/libsre.sprx'
        write(path, FIRMWARE)
        ok = check_firmware(path, REVIEWED, RPCS3, 'macos', sprx=sprx)
        self.assertEqual([f.code for f in ok], ['firmware-ok'])
        self.assertIn('system software 4.93', ok[0].message)
        unreviewed = b'\x7fELF synthetic unreviewed release'
        with patch.dict(input_guide.FIRMWARE_KNOWN_RELEASES, {sha(unreviewed): 'system software 9.99'}):
            for content, code, text in ((b'SCE\x00 container', 'firmware-encrypted', 'still encrypted'),
                                        (b'not a module', 'firmware-not-elf', 'not a decrypted firmware module'),
                                        (unreviewed, 'firmware-unreviewed-release', 'system software 9.99, decrypted correctly'),
                                        (b'\x7fELF other', 'firmware-other-release', 'Sony hosts only the latest release')):
                with self.subTest(code=code):
                    write(path, content)
                    findings = check_firmware(path, REVIEWED, RPCS3, 'macos', sprx=sprx)
                    self.assertEqual([f.code for f in findings], [code])
                    self.assertIn(text, findings[0].message)
                    self.assertTrue(any(str(sprx) in command for command in findings[0].commands))
        self.assertEqual(check_firmware(self.root, REVIEWED, RPCS3, 'macos')[0].code, 'firmware-folder')
        missing = check_firmware(self.root / 'firmware/libsre.prx', REVIEWED, RPCS3, 'macos', sprx=sprx)
        self.assertEqual((missing[0].code, missing[0].level), ('firmware-missing', TODO))

    def test_manifest_copy_is_preferred_and_the_repository_is_the_fallback(self):
        root = self.root / 'installer'
        write(root / 'release/player-build.json', json.dumps({'schema_version': 1, 'reviewed_inputs': REVIEWED}))
        self.assertEqual(load_reviewed(root), REVIEWED)
        (root / 'release/player-build.json').write_text(json.dumps({'schema_version': 1, 'kit': None}))
        self.assertIsNone(load_reviewed(root))
        checkout = load_reviewed(Path(__file__).resolve().parents[1])
        self.assertEqual(set(checkout['executables']), {'EBOOT.BIN', 'bin/mage_g.self'})
        self.assertTrue(all(len(image['sha256']) == 64 for image in checkout['firmware']))


class GuideTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()

    def tearDown(self):
        self.temp.cleanup()

    def test_nothing_found_links_to_official_pages_and_lists_every_step(self):
        for host, shell in (('macos', 'Terminal'), ('linux', 'a terminal'), ('windows', 'Command Prompt')):
            with self.subTest(host=host):
                steps = build_guide(Detection(host))
                self.assertEqual([s.state for s in steps], [TODO] * 7)
                self.assertEqual(steps[0].link, 'https://rpcs3.net/download')
                self.assertEqual(steps[1].link, 'https://www.playstation.com/en-us/get-help/ps3-system-software/')
                self.assertIn('--installfw', steps[2].commands[0])
                self.assertIn('--decrypt', steps[3].commands[0])
                text = render(steps)
                self.assertIn('0 of 6 steps done', text)
                self.assertIn(f'Run in {shell}', text)
                self.assertIn('PS3_GAME folder from your own BCUS98110', text)

    def test_found_firmware_container_asks_for_its_decrypt_command(self):
        sprx = write(self.root / 'Application Support/rpcs3/dev_flash/sys/external/libsre.sprx')
        found = Detection('macos', rpcs3=[(Path(RPCS3[0]),)], firmware_encrypted=[sprx])
        steps, *_ = survey(found, REVIEWED)
        self.assertEqual([s.state for s in steps[:4]], [DONE, DONE, DONE, TODO])
        self.assertEqual(steps[3].commands, [f"{RPCS3[0]} --decrypt {shlex.quote(str(sprx))}"])
        self.assertIn('writes libsre.prx beside libsre.sprx', render(steps))

    def test_found_and_checked_inputs_make_every_step_done(self):
        disc = prepared(self.root / 'MAG')
        firmware = write(self.root / 'rpcs3/dev_flash/sys/external/libsre.prx', FIRMWARE)
        found = Detection('linux', rpcs3=[('flatpak', 'run', 'net.rpcs3.RPCS3')], firmware_decrypted=[firmware],
                          firmware_encrypted=[firmware.with_suffix('.sprx')],
                          games=[GameCandidate(disc, 'RPCS3 games list', 'BCUS98110', '01.00', 'DG')])
        steps, game, checked, game_path, firmware_path = survey(found, REVIEWED)
        self.assertEqual((game_path, firmware_path), (disc, firmware))
        self.assertTrue(all(s.state == DONE for s in steps), render(steps))
        text = render(steps)
        self.assertIn('6 of 6 steps done', text)
        self.assertIn('found in RPCS3 games list', text)
        self.assertIn('click Build MAG', text)
        self.assertIn('--firmware', input_guide.suggested_build(found, game_path, firmware_path))

    def test_chosen_inputs_replace_found_ones_and_problems_show(self):
        disc = prepared(self.root / 'MAG', decrypted=False)
        found = Detection('windows', rpcs3=[(Path(r'C:\RPCS3\rpcs3.exe'),)])
        bad = write(self.root / 'libsre.prx', b'SCE\x00 still a container')
        steps, *_ = survey(found, REVIEWED, disc.parent, bad)
        self.assertEqual(steps[3].state, PROBLEM)
        self.assertIn('still encrypted', steps[3].lines[0])
        self.assertEqual(steps[5].state, TODO)
        self.assertIn('--decrypt', steps[5].commands[0])
        unusable = Detection('linux', games=[GameCandidate(self.root / 'game/BCUS98110', 'RPCS3 dev_hdd0/game',
                                                           'BCUS98110', '01.04', 'GD')])
        self.assertIn('update or game data', render(build_guide(unusable)))

    def test_incomplete_search_is_reported(self):
        text = render(build_guide(Detection('linux', incomplete=True, seconds=3.2)), Detection('linux', incomplete=True, seconds=3.2))
        self.assertIn('stopped after 3 seconds', text)

    def test_guide_command_prints_the_same_guide_and_json(self):
        from mag_installer.__main__ import main
        disc = prepared(self.root / 'MAG')
        found = Detection('linux', games=[GameCandidate(disc, 'RPCS3 games list', 'BCUS98110', '01.00', 'DG')])
        with patch('mag_installer.rpcs3_detect.detect', return_value=found), \
                patch('mag_installer.input_guide.load_reviewed', return_value=REVIEWED):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(main(['guide', '--json']), 0)
            event = json.loads(out.getvalue())
            self.assertEqual(event['event'], 'guide')
            self.assertEqual(event['source'], str(disc))
            self.assertTrue(event['reviewed_identities'])
            self.assertFalse(event['ready'])
            self.assertEqual(event['steps'][4]['state'], DONE)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(main(['guide', '--human', '--source', str(self.root)]), 0)
            self.assertIn('has no PS3_GAME folder', out.getvalue())
            self.assertIn('build --source', out.getvalue())


class FirmwareChoiceTests(unittest.TestCase):
    def test_chosen_firmware_replaces_the_default_location(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            prepared(root / 'MAG')
            elsewhere = write(root / 'rpcs3/dev_flash/sys/external/libsre.prx', FIRMWARE)
            with self.assertRaisesRegex(InstallerError, 'firmware/libsre.prx'):
                player_build.locate_inputs(root / 'MAG')
            self.assertEqual(player_build.locate_inputs(root / 'MAG', elsewhere)['firmware'], str(elsewhere))
            with self.assertRaisesRegex(InstallerError, 'is a folder'):
                player_build.locate_inputs(root / 'MAG', elsewhere.parent)
            with self.assertRaises(InstallerError) as raised:
                player_build.locate_inputs(root / 'MAG', root / 'missing.prx')
            self.assertIn('rpcs3 --installfw', str(raised.exception))
            self.assertIn('never decrypts files itself', str(raised.exception))

    def test_build_command_passes_the_chosen_firmware(self):
        from mag_installer.__main__ import main
        with patch('mag_installer.player_build.install') as build, redirect_stdout(io.StringIO()):
            self.assertEqual(main(['build', '--source', '/input', '--parent', '/destination', '--firmware', '/fw/libsre.prx']), 0)
            self.assertEqual(build.call_args.kwargs['firmware'], Path('/fw/libsre.prx'))
            self.assertEqual(main(['build', '--source', '/input', '--parent', '/destination']), 0)
            self.assertNotIn('firmware', build.call_args.kwargs)


class KitManifestTests(unittest.TestCase):
    def test_player_kit_manifest_carries_the_reviewed_identities(self):
        import importlib.util
        script = Path(__file__).resolve().parents[1] / 'scripts/prepare_player_kit.py'
        spec = importlib.util.spec_from_file_location('prepare_player_kit_under_test', script)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            root, source = Path(temp) / 'installer', Path(temp) / 'source'
            (root / 'release').mkdir(parents=True)
            retail = {'EBOOT.BIN': ('a' * 64, 'b' * 64), 'bin/mage_g.self': ('c' * 64, 'd' * 64)}
            write(source / 'tools/prepare_inputs.py', f"RETAIL = {retail!r}\nRETAIL_SETTINGS_ARCHIVE = 'run/s.sdat'\n"
                                                      f"RETAIL_SETTINGS_ARCHIVE_SHA256 = ({'e' * 64!r},)\n")
            write(source / 'profiles/retail-v1.0-firmware.json', json.dumps({'images': [{'release': '4.93', 'sha256': 'f' * 64}]}))
            descriptor = {'sha256': '0' * 64, 'size': 1, 'format': 'tar.gz', 'max_expanded_size': 1, 'max_entries': 1}
            with patch.object(module, 'prepare', return_value=descriptor), patch.object(module, 'pinned_sdks', return_value={}), \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(['--root', str(root), '--source', str(source), '--host', 'linux-x86_64']), 0)
            manifest = json.loads((root / 'release/player-build.json').read_text())
            reviewed = manifest['reviewed_inputs']
            self.assertEqual(reviewed['executables']['EBOOT.BIN'], ['a' * 64, 'b' * 64])
            self.assertEqual(reviewed['firmware'], [{'release': '4.93', 'sha256': 'f' * 64}])
            self.assertEqual(reviewed['settings_archive'], {'path': 'run/s.sdat', 'sha256': ['e' * 64]})
            self.assertEqual(load_reviewed(root), reviewed)


if __name__ == '__main__':
    unittest.main()
