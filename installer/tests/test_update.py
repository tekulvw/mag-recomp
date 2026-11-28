"""Updating an existing MAG folder in place (update.py, player_build.update).

Synthetic only: generated files stand in for the game files, the tools and the app; the build and
packaging steps are faked as the layout tests fake them. No game file is read and nothing is run
from the app.
"""
import contextlib
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from mag_installer import installs, platforms, player_build
from mag_installer import update as updating
from mag_installer.gui import PlayerBuildController
from mag_installer.model import InstallerError
from mag_installer.native_build import link_tree
from mag_installer.safety import Cancellation

from test_player_layout import LayoutFixture

HOSTS = (('macos-arm64', 'MAG.app'), ('windows-x86_64', 'MAG'), ('linux-x86_64', 'MAG'))


class Killed(BaseException):
    """Stands in for the process dying: nothing after it runs, not even the rollback."""


def digest_tree(root):
    """Every file and folder under root, with each file's SHA-256."""
    root = Path(root)
    if not root.exists():
        return None
    return {path.relative_to(root).as_posix(): (hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else 'dir')
            for path in sorted(root.rglob('*'))}


def make_app(destination, host, version, options=None):
    """A minimal app updating accepts: runtime record, both images with receipts, a supervisor."""
    destination.mkdir()
    if host == 'macos-arm64':
        images = {'launcher': 'Contents/MacOS/MagRecomp', 'engine': 'Contents/MacOS/MagEngine'}
    else:
        suffix = '.exe' if host == 'windows-x86_64' else ''
        images = {'launcher': 'bin/MagRecomp' + suffix, 'engine': 'bin/MagEngine' + suffix}
    receipts = updating.receipts_folder(destination, host)
    receipts.mkdir(parents=True)
    for role, relative in images.items():
        binary = destination / relative
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(f'synthetic {role} {version}'.encode())
        receipt = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
        elf = destination.parent / 'game/PS3_GAME/USRDIR' / ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        if elf.is_file():
            receipt['elf_sha256'] = hashlib.sha256(elf.read_bytes()).hexdigest()
        (receipts / (role + '.json')).write_text(json.dumps(receipt))
    supervisor = updating.supervisor_path(destination, host)
    supervisor.parent.mkdir(parents=True, exist_ok=True)
    supervisor.write_bytes(b'synthetic supervisor')
    supervisor.chmod(0o755)
    runtime = {'schema_version': 2, 'game': '../game', **images, 'version': version}
    if options is not None:
        runtime['build_options'] = options
    updating.runtime_path(destination, host).write_text(json.dumps(runtime))
    return destination


def config_host(config):
    return config.get('host') or ('windows-x86_64' if 'msvc' in config else 'macos-arm64')


class UpdateFixture(LayoutFixture):
    def inputs(self, root):
        """A game folder the wizard's checks accept without reviewed identities."""
        files = {'PS3_GAME/PARAM.SFO': b'synthetic param', 'PS3_GAME/USRDIR/EBOOT.BIN': b'SCE\x00synthetic',
                 'PS3_GAME/USRDIR/EBOOT.elf': b'\x7fELFsynthetic launcher', 'PS3_GAME/USRDIR/bin/mage_g.self': b'SCE\x00synthetic',
                 'PS3_GAME/USRDIR/bin/mage_g.elf': b'\x7fELFsynthetic engine',
                 'PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat': b'synthetic settings',
                 'firmware/libsre.prx': b'\x7fELFsynthetic firmware'}
        for name, data in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def packager(self, version, calls=None, game=True):
        def package(config, workspace, destination, token=None, events=None, **kwargs):
            if game:
                if config.get('disc_root'):
                    link_tree(Path(config['disc_root']), destination.parent / 'game' / 'PS3_GAME', Cancellation())
                else:
                    (destination.parent / 'game/PS3_GAME').mkdir(parents=True)
            make_app(destination, config_host(config), version, config.get('build_options'))
            if calls is not None:
                calls.append(config)
            return destination
        return package

    def setup_host(self, stack, host, version, calls=None, native=None):
        tools, _ = self.patches(stack, package=self.packager(version, calls), native=native, host=host)
        stack.enter_context(patch.object(player_build, 'host_key', return_value=host))
        stack.enter_context(patch('mag_installer.managed_tools.opus_prefix', return_value='/managed/opus'))
        return tools

    def installed(self, temp, host='macos-arm64', version='0.15.0', voice=None):
        """A MAG folder built (synthetically) at version, played once: saves and launch logs."""
        root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
        output, cache = root / 'MAG', root / 'cache'
        with ExitStack() as stack:
            self.setup_host(stack, host, version)
            player_build.install(root, source, output, cache, **({'voice': voice} if voice is not None else {}))
        (output / 'home/gamedata').mkdir(parents=True); (output / 'home/gamedata/save.dat').write_bytes(b'synthetic save')
        (output / 'logs/launch-1a2b').mkdir(parents=True); (output / 'logs/launch-1a2b/engine.log').write_text('synthetic log')
        (output / 'launch-error.log').write_text('synthetic launch error')
        return root, source, output, cache

    def player_files(self, output):
        return {name: digest_tree(output / name) for name in ('home', 'logs', 'game')}


class UpdateTests(UpdateFixture, unittest.TestCase):
    def test_update_replaces_only_the_app_and_keeps_the_previous_one(self):
        for host, name in HOSTS:
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                root, source, output, cache = self.installed(temp, host)
                build = root / 'MAG-build'
                before = self.player_files(output)
                game_file = output / 'game/PS3_GAME/USRDIR/EBOOT.elf'
                identity = (game_file.stat().st_ino, game_file.stat().st_dev)
                supervisor = updating.supervisor_path(output / name, host)
                self.setup_host(stack, host, '0.16.0')
                events = []
                self.assertEqual(player_build.update(root, output, cache, events=events.append), output)
                # Same paths; only the app changed.
                self.assertTrue(supervisor.is_file())
                self.assertEqual(updating.app_version(output / name, host), '0.16.0')
                self.assertEqual(sorted(p.name for p in output.iterdir()), sorted([name, 'game', 'home', 'logs', 'launch-error.log']))
                self.assertEqual(self.player_files(output), before)
                self.assertEqual((game_file.stat().st_ino, game_file.stat().st_dev), identity)  # game/ never moved
                self.assertEqual((output / 'launch-error.log').read_text(), 'synthetic launch error')
                # The build folder keeps only its logs, the previous app and the journal (decision 13).
                self.assertEqual(sorted(p.name for p in build.iterdir()), ['build.json', 'logs', 'previous'])
                self.assertEqual(updating.app_version(build / 'previous' / name, host), '0.15.0')
                kept = json.loads((build / 'previous/previous.json').read_text())
                self.assertEqual((kept['version'], kept['game']), ('0.15.0', False))
                journal = json.loads((build / 'build.json').read_text())
                self.assertEqual((journal['state'], journal['swap']['phase'], journal['swap']['kind']), ('complete', 'done', 'update'))
                self.assertEqual((journal['swap']['from_version'], journal['swap']['to_version']), ('0.15.0', '0.16.0'))
                self.assertEqual(journal['swap']['game'], 'kept')
                done = events[-1]
                self.assertEqual((done['event'], done['updated'], done['to_version']), ('player-build-complete', True, '0.16.0'))
                self.assertIn('Your saves, settings and logs were kept', done['message'])
                self.assertEqual(installs.entry(cache, output)['version'], '0.16.0')
                # Another update replaces the kept previous app with this one.
                stack.enter_context(patch('mag_installer.native_package.package', side_effect=self.packager('0.17.0')))
                player_build.update(root, output, cache)
                self.assertEqual(updating.app_version(output / name, host), '0.17.0')
                self.assertEqual(updating.app_version(build / 'previous' / name, host), '0.16.0')
                self.assertEqual(sorted(p.name for p in (build / 'previous').iterdir()), [name, 'previous.json'])
                self.assertEqual(self.player_files(output), before)

    def test_a_failure_at_each_step_restores_the_previous_app(self):
        for step in ('stage', 'set-aside', 'move-in', 'verify', 'permissions'):
            with self.subTest(step=step), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                root, source, output, cache = self.installed(temp)
                build = root / 'MAG-build'
                before = self.player_files(output)
                self.setup_host(stack, 'macos-arm64', '0.16.0')
                def fail(name, step=step):
                    if name == step:
                        raise InstallerError(f'synthetic failure at {name}')
                with patch.object(updating, '_step', side_effect=fail):
                    with self.assertRaisesRegex(InstallerError, f'synthetic failure at {step}'):
                        player_build.update(root, output, cache)
                self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.15.0')
                self.assertEqual(self.player_files(output), before)
                self.assertFalse(any((output / n).exists() for n in (updating.APP_NEW, updating.APP_PREVIOUS, updating.GAME_PREVIOUS)))
                self.assertEqual(updating.app_version(build / 'package/MAG.app', 'macos-arm64'), '0.16.0')  # kept for the retry
                journal = json.loads((build / 'build.json').read_text())
                self.assertEqual((journal['state'], journal['swap']['phase']), ('incomplete', 'rolled-back'))
                self.assertIn(step, journal['swap']['error'])
                # Updating again reuses the attempt's journal (a retry) and finishes.
                player_build.update(root, output, cache)
                self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')
                self.assertEqual(self.player_files(output), before)
                self.assertTrue((build / 'logs/attempts/1').is_dir())

    def test_an_interruption_at_each_step_is_finished_by_the_next_run(self):
        for step in ('stage', 'set-aside', 'move-in', 'verify', 'permissions', 'keep', 'clean'):
            with self.subTest(step=step), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                root, source, output, cache = self.installed(temp)
                build = root / 'MAG-build'
                before = self.player_files(output)
                calls = []
                self.setup_host(stack, 'macos-arm64', '0.16.0', calls=calls)
                def die(name, step=step):
                    if name == step:
                        raise Killed()
                # The process dies at this step: no rollback runs, the journal keeps its phase.
                with patch.object(updating, '_step', side_effect=die), patch.object(updating, 'reverse', side_effect=Killed()):
                    with self.assertRaises((Killed, InstallerError)):
                        player_build.update(root, output, cache)
                phase = json.loads((build / 'build.json').read_text())['swap']['phase']
                self.assertIn(phase, updating.OPEN_PHASES)
                built = len(calls)
                events = []
                player_build.update(root, output, cache, events=events.append)
                self.assertEqual(len(calls), built, 'finishing the swap must not rebuild')
                self.assertTrue(any(event['event'] == 'update-recovering' for event in events))
                self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')
                self.assertEqual(updating.app_version(build / 'previous/MAG.app', 'macos-arm64'), '0.15.0')
                self.assertEqual(self.player_files(output), before)
                self.assertFalse(any((output / n).exists() for n in (updating.APP_NEW, updating.APP_PREVIOUS)))
                journal = json.loads((build / 'build.json').read_text())
                self.assertEqual((journal['state'], journal['swap']['phase']), ('complete', 'done'))
                self.assertEqual(sorted(p.name for p in build.iterdir()), ['build.json', 'logs', 'previous'])

    def test_changed_game_files_are_swapped_too_and_kept_with_the_previous_app(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            build = root / 'MAG-build'
            (source / 'PS3_GAME/USRDIR/extra.dat').write_bytes(b'a file the new release stages')
            self.setup_host(stack, 'macos-arm64', '0.16.0')
            before = {name: digest_tree(output / name) for name in ('home', 'logs')}
            old_game = digest_tree(output / 'game')
            def fail(name):
                if name == 'verify':
                    raise InstallerError('synthetic verification failure')
            with patch.object(updating, '_step', side_effect=fail):
                with self.assertRaisesRegex(InstallerError, 'verification'):
                    player_build.update(root, output, cache, source=source)
            self.assertEqual(digest_tree(output / 'game'), old_game)  # put back with the previous app
            player_build.update(root, output, cache, source=source)
            self.assertTrue((output / 'game/PS3_GAME/USRDIR/extra.dat').is_file())
            self.assertEqual(digest_tree(build / 'previous/game'), old_game)
            self.assertTrue(json.loads((build / 'previous/previous.json').read_text())['game'])
            self.assertEqual({name: digest_tree(output / name) for name in ('home', 'logs')}, before)
            # Going back puts the earlier game files back as well.
            player_build.update(root, output, cache, restore_previous=True)
            self.assertEqual(digest_tree(output / 'game'), old_game)

    def test_restore_previous_swaps_back_and_forth_without_building(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            build = root / 'MAG-build'
            with self.assertRaisesRegex(InstallerError, 'No previous version'):
                with ExitStack() as first:
                    self.setup_host(first, 'macos-arm64', '0.16.0')
                    player_build.update(root, output, cache, restore_previous=True)
            calls = []
            self.setup_host(stack, 'macos-arm64', '0.16.0', calls=calls)
            player_build.update(root, output, cache)
            before = self.player_files(output)
            events = []
            player_build.update(root, output, cache, restore_previous=True, events=events.append)
            self.assertEqual(len(calls), 1)
            self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.15.0')
            self.assertEqual(updating.app_version(build / 'previous/MAG.app', 'macos-arm64'), '0.16.0')
            self.assertTrue(events[-1]['restored'])
            self.assertEqual(installs.entry(cache, output)['version'], '0.15.0')
            player_build.update(root, output, cache, restore_previous=True)
            self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')
            self.assertEqual(self.player_files(output), before)


class RememberedInputTests(UpdateFixture, unittest.TestCase):
    def test_a_build_remembers_its_inputs_options_and_a_firmware_copy(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, output, cache = self.installed(temp, voice=True)
            entry = installs.entry(cache, output)
            self.assertEqual((entry['game_folder'], entry['firmware'], entry['version'], entry['firmware_copy']),
                             (str(source), str(source / 'firmware/libsre.prx'), '0.15.0', True))
            self.assertGreater(entry['peak_build_bytes'], 0)
            self.assertIn('build', entry['stage_seconds'])
            self.assertEqual((cache / installs.FIRMWARE_COPY).read_bytes(), (source / 'firmware/libsre.prx').read_bytes())
            if os.name != 'nt':
                self.assertEqual((cache / installs.FIRMWARE_COPY).stat().st_mode & 0o777, 0o600)
            options = updating.app_options(output / 'MAG.app', 'macos-arm64')
            self.assertEqual(options, {'voice': True, 'game_controls': True, 'x86_level': None})
            # Nothing in the MAG folder names the game folder or the cache.
            for path in output.rglob('*'):
                if path.is_file() and path.suffix == '.json':
                    self.assertNotIn(str(source), path.read_text()); self.assertNotIn(str(cache), path.read_text())

    def test_an_update_keeps_the_options_the_app_was_built_with(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp, voice=True)
            from mag_installer import voice_option
            voice_option.remember(cache, False)  # another MAG folder was built without voice since
            calls = []
            self.setup_host(stack, 'macos-arm64', '0.16.0', calls=calls)
            with patch.dict(os.environ, {'MAG_VOICE': '', 'MAG_GAME_CONTROLS': '', 'MAG_X86_LEVEL': ''}):
                player_build.update(root, output, cache)
            self.assertEqual(calls[-1]['build_options']['voice'], True)
            self.assertIn('opus', calls[-1])
            self.assertFalse(voice_option.remembered(cache))  # the preference itself is unchanged
            with patch.dict(os.environ, {'MAG_VOICE': '', 'MAG_GAME_CONTROLS': '', 'MAG_X86_LEVEL': ''}):
                player_build.update(root, output, cache, voice=False)  # an explicit choice wins
            self.assertEqual(calls[-1]['build_options']['voice'], False)

    def test_moved_inputs_fall_back_to_the_mag_folder_and_the_firmware_copy(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            shutil.rmtree(source)  # the player moved or deleted the game folder and the firmware file
            self.setup_host(stack, 'macos-arm64', '0.16.0')
            stack.enter_context(patch('mag_installer.rpcs3_detect.detect', side_effect=AssertionError('not needed')))
            events = []
            player_build.update(root, output, cache, events=events.append)
            chosen = {event['input']: event['path'] for event in events if event['event'] == 'update-input'}
            self.assertEqual(chosen, {'game': str(output / 'game'), 'firmware': str(cache / installs.FIRMWARE_COPY)})
            self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')
            self.assertTrue((output / 'game/PS3_GAME/USRDIR/EBOOT.elf').is_file())

    def test_rpcs3_is_asked_last_and_missing_inputs_change_nothing(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            elsewhere = root / 'rpcs3-copy'; shutil.copytree(source, elsewhere)
            shutil.rmtree(source); (cache / installs.FIRMWARE_COPY).unlink()
            self.setup_host(stack, 'macos-arm64', '0.16.0')
            before = digest_tree(output)
            survey = stack.enter_context(patch('mag_installer.input_guide.survey', return_value=([], None, None, None, None)))
            stack.enter_context(patch('mag_installer.rpcs3_detect.detect', return_value=object()))
            with self.assertRaisesRegex(InstallerError, 'firmware file'):
                player_build.update(root, output, cache)
            self.assertEqual(digest_tree(output), before)
            survey.return_value = ([], None, None, elsewhere, elsewhere / 'firmware/libsre.prx')
            events = []
            player_build.update(root, output, cache, events=events.append)
            chosen = {event['input']: event['path'] for event in events if event['event'] == 'update-input'}
            self.assertEqual(chosen['firmware'], str(elsewhere / 'firmware/libsre.prx'))
            self.assertEqual(chosen['game'], str(output / 'game'))
            # With the MAG folder's game files changed and nothing remembered, the player must choose.
            (output / 'game/PS3_GAME/USRDIR/EBOOT.elf').unlink()
            (output / 'game/PS3_GAME/USRDIR/EBOOT.elf').write_bytes(b'SCE\x00renamed, still encrypted')
            survey.return_value = ([], None, None, None, None)
            installs.record(cache, output, version='0.16.0')
            with self.assertRaisesRegex(InstallerError, 'Choose your MAG game folder'):
                player_build.update(root, output, cache)


class UpdateRefusalTests(UpdateFixture, unittest.TestCase):
    def test_refuses_while_mag_runs_from_the_folder(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            tools = self.setup_host(stack, 'macos-arm64', '0.16.0')
            tools.reset_mock()
            before = digest_tree(root)
            # The launcher holds the session lock (shared) while it runs.
            lock = output / updating.SESSION_LOCK
            lock.write_bytes(b'')
            handle = os.open(lock, os.O_RDWR)
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(handle, msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(handle, fcntl.LOCK_SH)
                with self.assertRaisesRegex(InstallerError, 'MAG is running'):
                    player_build.update(root, output, cache)
            finally:
                os.close(handle)
            lock.unlink()
            self.assertEqual(digest_tree(root), {**before})
            # MAG built before the lock: found by its processes' paths.
            engine = output / 'MAG.app/Contents/MacOS/MagEngine'
            with patch.object(platforms, 'running_executables', return_value=[os.path.realpath(engine)]):
                with self.assertRaisesRegex(InstallerError, 'MAG is running'):
                    player_build.update(root, output, cache)
            tools.assert_not_called()
            self.assertEqual(digest_tree(root), before)
            # A free lock file is held during the update, and the update goes ahead.
            lock.write_bytes(b'')
            player_build.update(root, output, cache)
            self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')

    @unittest.skipUnless(sys.platform.startswith('linux') and Path('/bin/sleep').is_file(), 'a program to run from the app folder')
    def test_a_real_process_inside_the_app_folder_is_found(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); output = root / 'MAG'
            app = output / 'MAG'; (app / 'bin').mkdir(parents=True)
            program = app / 'bin/sleep'; shutil.copy2('/bin/sleep', program)
            self.assertIsNone(updating.running_reason(output, 'linux-x86_64'))
            try:
                process = subprocess.Popen([str(program), '30'])
            except OSError as exc:  # a temporary folder mounted noexec
                self.skipTest(f'cannot run a program from the temporary folder: {exc}')
            try:
                for _ in range(50):
                    if updating.running_reason(output, 'linux-x86_64'):
                        break
                    import time; time.sleep(0.1)
                self.assertIn('MAG is running', updating.running_reason(output, 'linux-x86_64') or '')
            finally:
                process.kill(); process.wait()

    def test_refuses_without_disk_space_and_changes_nothing(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            tools = self.setup_host(stack, 'macos-arm64', '0.16.0')
            tools.reset_mock()
            before, build_before = digest_tree(output), digest_tree(root / 'MAG-build')
            usage = shutil.disk_usage(root)
            # Without a measured peak (no record of this folder), the default applies.
            entry = installs.entry(cache, output)
            (cache / installs.INSTALLS).unlink()
            with patch.object(updating.shutil, 'disk_usage', return_value=usage._replace(free=3 * updating.GIB)):
                with self.assertRaisesRegex(InstallerError, r'needs about .* free'):
                    player_build.update(root, output, cache)
            tools.assert_not_called()
            self.assertEqual(digest_tree(output), before)
            self.assertEqual(digest_tree(root / 'MAG-build'), build_before)  # not even the build folder changed
            # The measured peak replaces the default: a small build needs little room.
            kept = {key: value for key, value in entry.items() if key not in ('output', 'recorded')}
            installs.record(cache, output, **{**kept, 'peak_build_bytes': 1024})
            with patch.object(updating.shutil, 'disk_usage', return_value=usage._replace(free=3 * updating.GIB)):
                player_build.update(root, output, cache)
            self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')

    def test_unknown_files_or_a_foreign_build_folder_stop_the_update(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            self.setup_host(stack, 'macos-arm64', '0.16.0')
            (output / 'notes.txt').write_text('the player\'s own file')
            before = digest_tree(root)
            with self.assertRaisesRegex(InstallerError, 'notes.txt'):
                player_build.update(root, output, cache)
            self.assertEqual(digest_tree(root), before)
            (output / 'notes.txt').unlink()
            (root / 'MAG-build/notes.txt').write_text('another file')
            with self.assertRaisesRegex(InstallerError, 'notes.txt'):
                player_build.update(root, output, cache)
            (root / 'MAG-build/notes.txt').unlink()
            with self.assertRaisesRegex(InstallerError, 'There is no MAG folder'):
                player_build.update(root, root / 'Elsewhere', cache)

    def test_the_earlier_layout_is_moved_first_then_updated(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output = self.legacy(root); cache = root / 'cache'
            self.setup_host(stack, 'macos-arm64', '0.16.0')
            player_build.update(root, output, cache, source=source)
            self.assertEqual(updating.app_version(output / 'MAG.app', 'macos-arm64'), '0.16.0')
            self.assertEqual((output / 'home/gamedata/save.dat').read_text(), 'synthetic save')
            self.assertEqual((output / 'logs/launch-1a2b/engine.log').read_text(), 'synthetic launch log')


@unittest.skipIf(os.name == 'nt', 'POSIX modes')
class PosixPermissionTests(UpdateFixture, unittest.TestCase):
    def test_the_new_app_gets_the_previous_app_folders_mode(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            (output / 'MAG.app').chmod(0o750)
            self.setup_host(stack, 'macos-arm64', '0.16.0')
            player_build.update(root, output, cache)
            self.assertEqual((output / 'MAG.app').stat().st_mode & 0o777, 0o750)


@unittest.skipUnless(os.name == 'nt', 'Windows folder permissions')
class WindowsPermissionTests(UpdateFixture, unittest.TestCase):
    def installed_in_profile(self):
        # Like the player's own profile folder: tempfile.mkdtemp would itself be protected.
        import secrets
        parent = (Path(tempfile.gettempdir()) / ('mag-update-acl-' + secrets.token_hex(6))).resolve()
        parent.mkdir()
        self.addCleanup(shutil.rmtree, parent, ignore_errors=True)
        root, source, output, cache = self.installed(parent, host='windows-x86_64')
        app = output / 'MAG'
        # The player's own explicit full-access entry, as the manual fix added it.
        sid = platforms.current_user_sid()
        flags, aces = platforms.dacl_parts(platforms.folder_dacl(app))
        explicit = ''.join(f'({ace})' for ace in aces if 'ID' not in ace.split(';')[1])
        platforms.apply_explicit_dacl(app, f'D:{flags}{explicit}(A;OICI;FA;;;{sid})')
        return root, output, cache, app, sid

    def test_the_new_app_keeps_the_previous_app_folders_permissions(self):
        root, output, cache, app, sid = self.installed_in_profile()
        before = platforms.dacl_parts(platforms.folder_dacl(app))
        folder = platforms.dacl_parts(platforms.folder_dacl(output))
        self.assertTrue(any(sid in ace and 'ID' not in ace.split(';')[1] for ace in before[1]))
        with ExitStack() as stack:
            self.setup_host(stack, 'windows-x86_64', '0.16.0')
            player_build.update(root, output, cache)
        self.assertEqual(updating.app_version(app, 'windows-x86_64'), '0.16.0')
        self.assertEqual(platforms.dacl_parts(platforms.folder_dacl(app)), before)
        self.assertEqual(platforms.dacl_parts(platforms.folder_dacl(output)), folder)
        for path in [app, *app.rglob('*')]:
            self.assertTrue(platforms.grants_full_control(platforms.dacl_parts(platforms.folder_dacl(path))[1], sid), path)

    def test_a_permission_mismatch_rolls_the_update_back(self):
        root, output, cache, app, sid = self.installed_in_profile()
        before = platforms.dacl_parts(platforms.folder_dacl(app))
        with ExitStack() as stack:
            self.setup_host(stack, 'windows-x86_64', '0.16.0')
            stack.enter_context(patch.object(platforms, 'apply_explicit_dacl'))  # the entries are not copied
            with self.assertRaisesRegex(InstallerError, 'permissions differ'):
                player_build.update(root, output, cache)
        self.assertEqual(updating.app_version(app, 'windows-x86_64'), '0.15.0')
        self.assertEqual(platforms.dacl_parts(platforms.folder_dacl(app)), before)


class EntryPointTests(UpdateFixture, unittest.TestCase):
    def test_build_on_an_existing_mag_folder_updates_it(self):
        from mag_installer import __main__ as cli
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            calls = []
            stack.enter_context(patch.object(player_build, '_manifest', return_value={'host': 'macos-arm64', 'kit': {}}))
            update = stack.enter_context(patch.object(player_build, 'update', side_effect=lambda *a, **k: calls.append((a, k))))
            install = stack.enter_context(patch.object(player_build, 'install'))
            stack.enter_context(contextlib.redirect_stdout(open(os.devnull, 'w')))
            self.assertEqual(cli.main(['build', '--output', str(output), '--cache', str(cache), '--json']), 0)
            install.assert_not_called()
            self.assertEqual((calls[-1][0][1], calls[-1][1]['source']), (output, None))
            self.assertEqual(cli.main(['update', '--output', str(output), '--cache', str(cache), '--restore-previous', '--json']), 0)
            self.assertTrue(calls[-1][1]['restore_previous'])
            # A new folder still builds, and needs the game folder.
            self.assertEqual(cli.main(['build', '--output', str(root / 'New'), '--cache', str(cache), '--json']), 2)
            self.assertEqual(cli.main(['build', '--output', str(root / 'New'), '--source', str(source), '--json']), 0)
            install.assert_called_once()

    def test_wizard_offers_update_mag_for_an_existing_folder(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root, source, output, cache = self.installed(temp)
            stack.enter_context(patch.object(player_build, '_manifest', return_value={'host': 'macos-arm64', 'kit': {}}))
            calls = []
            controller = PlayerBuildController(root, readiness=lambda r: {'ready': True},
                                               updater=lambda *a, **k: calls.append((a, k)) or a[1])
            plan = controller.update_plan(root, cache)
            self.assertEqual((plan['from_version'], plan['remembered']['game_folder']), ('0.15.0', str(source)))
            self.assertEqual(controller.update_label(plan), 'Update MAG (0.15.0 → this version)')
            self.assertEqual(controller.update_label({'from_version': None, 'to_version': '0.16.0'}),
                             'Update MAG (an earlier MAG → 0.16.0)')
            self.assertIsNone(controller.update_plan(root / 'nowhere', cache))
            # No "Output already exists": the folder is updated, with empty fields meaning "as last time".
            controller.start('', root, cache, update=True)
            controller.worker.join(2); controller.poll()
            self.assertEqual(controller.state, 'complete')
            self.assertEqual((calls[0][0][1], calls[0][1]['source'], calls[0][1]['firmware']), (output, None, None))
            with self.assertRaisesRegex(InstallerError, 'Output already exists'):
                controller.start(source, root, cache)


class HelperTests(unittest.TestCase):
    def test_the_self_check_reads_receipts_launcher_and_game_identities(self):
        for host, name in HOSTS:
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temp:
                output = Path(temp).resolve() / 'MAG'
                elf = output / 'game/PS3_GAME/USRDIR/EBOOT.elf'; elf.parent.mkdir(parents=True); elf.write_bytes(b'\x7fELF one')
                app = make_app(output / name, host, '1.0.0')
                updating.verify_app(app, host, output)
                elf.write_bytes(b'\x7fELF two')
                with self.assertRaisesRegex(InstallerError, 'not the ones'):
                    updating.verify_app(app, host, output)
                elf.write_bytes(b'\x7fELF one')
                updating.receipts_folder(app, host).joinpath('engine.json').write_text('{"binary_sha256": "0"}')
                with self.assertRaisesRegex(InstallerError, 'engine does not match'):
                    updating.verify_app(app, host, output)
                updating.supervisor_path(app, host).unlink()
                with self.assertRaisesRegex(InstallerError, 'launcher'):
                    updating.verify_app(app, host, output)

    def test_same_tree_compares_names_and_contents(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / 'a/sub').mkdir(parents=True); (root / 'a/sub/f').write_bytes(b'one')
            link_tree(root / 'a', root / 'linked', Cancellation())
            shutil.copytree(root / 'a', root / 'copied')
            self.assertTrue(updating.same_tree(root / 'a', root / 'linked'))
            self.assertTrue(updating.same_tree(root / 'a', root / 'copied'))
            (root / 'copied/sub/f').write_bytes(b'two')
            self.assertFalse(updating.same_tree(root / 'a', root / 'copied'))
            (root / 'linked/extra').write_bytes(b'')
            self.assertFalse(updating.same_tree(root / 'a', root / 'linked'))

    def test_a_damaged_firmware_copy_is_deleted_and_records_tolerate_damage(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'; firmware = Path(temp) / 'libsre.prx'; firmware.write_bytes(b'\x7fELFfirmware')
            digest = hashlib.sha256(firmware.read_bytes()).hexdigest()
            self.assertFalse(installs.keep_firmware(cache, firmware, '0' * 64))  # a mismatch keeps nothing
            self.assertFalse((cache / installs.FIRMWARE_COPY).exists())
            self.assertTrue(installs.keep_firmware(cache, firmware, digest))
            reviewed = {'firmware': [{'sha256': digest}]}
            self.assertEqual(installs.firmware_copy(cache, reviewed), cache / installs.FIRMWARE_COPY)
            (cache / installs.FIRMWARE_COPY).write_bytes(b'changed')
            self.assertIsNone(installs.firmware_copy(cache, reviewed))
            self.assertFalse((cache / installs.FIRMWARE_COPY).exists())
            (cache / installs.INSTALLS).write_text('{not json')
            self.assertEqual(installs.read(cache), {})
            installs.record(cache, Path(temp) / 'MAG', version='1.0.0')
            self.assertEqual(installs.entry(cache, Path(temp) / 'MAG')['version'], '1.0.0')

    def test_build_options_are_checked_in_the_recipe(self):
        from mag_installer.native_recipe import _object
        self.assertIsNone(_object({'voice': True}, set(), {'voice', 'game_controls', 'x86_level'}, 'build_options'))


if __name__ == '__main__':
    unittest.main()
