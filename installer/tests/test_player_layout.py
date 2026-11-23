"""The self-contained MAG folder: its layout, relocation, and moving an earlier layout to it.

Synthetic only: generated files stand in for game files, tools and binaries.
"""
import json
import os
from contextlib import ExitStack
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from mag_installer import player_build
from mag_installer.gui import PlayerBuildController
from mag_installer.model import InstallerError
from mag_installer.safety import Cancellation


def tree(root):
    return sorted(p.relative_to(root).as_posix() for p in root.rglob('*'))


class LinkTests(unittest.TestCase):
    def test_game_files_are_hard_linked_where_possible_and_copied_otherwise(self):
        from mag_installer.native_build import link_or_copy, link_tree
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'source'
            (source / 'USRDIR/bin').mkdir(parents=True)
            (source / 'PARAM.SFO').write_bytes(b'synthetic param')
            (source / 'USRDIR/bin/data.bin').write_bytes(b'synthetic data')
            counts = link_tree(source, root / 'linked', Cancellation())
            self.assertEqual(counts, {'linked': 2, 'copied': 0})
            self.assertTrue((root / 'linked/USRDIR/bin/data.bin').samefile(source / 'USRDIR/bin/data.bin'))
            # Another volume, or a file system without links: a copy, with the same bytes.
            with patch('os.link', side_effect=OSError(18, 'cross-device link')):
                self.assertFalse(link_or_copy(source / 'PARAM.SFO', root / 'copied/PARAM.SFO', Cancellation()))
            self.assertFalse((root / 'copied/PARAM.SFO').samefile(source / 'PARAM.SFO'))
            self.assertEqual((root / 'copied/PARAM.SFO').read_bytes(), b'synthetic param')
            with self.assertRaisesRegex(InstallerError, 'already exists'):
                link_or_copy(source / 'PARAM.SFO', root / 'copied/PARAM.SFO', Cancellation())
            skipped = link_tree(source, root / 'skipped', Cancellation(), skip={(source / 'PARAM.SFO').resolve()})
            self.assertEqual((skipped, tree(root / 'skipped')), ({'linked': 1, 'copied': 0}, ['USRDIR', 'USRDIR/bin', 'USRDIR/bin/data.bin']))

    @unittest.skipUnless(os.name == 'nt', 'Windows read-only attribute')
    def test_read_only_files_are_copied_on_windows(self):
        from mag_installer.native_build import link_or_copy
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'locked.bin'; source.write_bytes(b'synthetic')
            os.chmod(source, 0o444)
            self.assertFalse(link_or_copy(source, root / 'copy.bin', Cancellation()))
            shutil.rmtree(root / 'copy.bin', ignore_errors=True)
            os.chmod(source, 0o666)


class LayoutFixture:
    host = 'macos-arm64'

    def inputs(self, root):
        for name in ('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN', 'PS3_GAME/USRDIR/EBOOT.elf',
                     'PS3_GAME/USRDIR/bin/mage_g.self', 'PS3_GAME/USRDIR/bin/mage_g.elf', 'firmware/libsre.prx'):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic input')

    def patches(self, stack, package=None, native=None, host=None):
        host = host or self.host
        stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
        stack.enter_context(patch.object(player_build, '_manifest', return_value={'host': host, 'kit': {}}))
        stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree': 'source', 'sdks': {}}))
        stack.enter_context(patch('mag_installer.native_build._verify_identities'))
        stack.enter_context(patch('mag_installer.native_build.check_windows_location'))
        stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
        stack.enter_context(patch('mag_installer.managed_tools.ensure_windows_build_tools',
                                  return_value={k: 'C:/VS/' + k for k in ('vcvars', 'clang_cl', 'installation', 'version', 'product')}))
        stack.enter_context(patch('mag_installer.managed_tools.ensure_linux_build_tools', return_value={'cc': 'cc', 'cxx': 'c++'}))
        tools = stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={
            'root': 'managed', 'python': 'python', 'cmake': 'cmake', 'ninja': 'ninja', 'git': 'git', 'ffmpeg': {}}))
        # No test downloads the pinned Rust toolchain for the game controls.
        stack.enter_context(patch('mag_installer.managed_tools.rust_toolchain',
                                  return_value={'cargo': 'cargo', 'rustc': 'rustc'}))
        def default_native(config, work, **kwargs):
            (work / 'workspace').mkdir(parents=True)
        stack.enter_context(patch('mag_installer.native_build.install', side_effect=native or default_native))
        def default_package(config, workspace, destination, **kwargs):
            destination.mkdir(); (destination / 'runtime.json').write_text('{"game": "../game"}')
            (destination.parent / 'game/PS3_GAME').mkdir(parents=True)
            return destination
        packager = stack.enter_context(patch('mag_installer.native_package.package', side_effect=package or default_package))
        return tools, packager

    def legacy(self, root, state='complete', name='MAG.app', host=None):
        """A MAG folder an earlier installer finished, with its build files inside."""
        output = root / 'MAG'
        for path in ('build-work/workspace/local/retail-v1.0/PS3_GAME', 'build-kit/source', 'managed-tools/prefix',
                     name + '/Contents', 'home/gamedata', 'logs/launch-1a2b', 'logs/attempts/1'):
            (output / path).mkdir(parents=True)
        (output / 'build.json').write_text(json.dumps({
            'schema_version': 2, 'state': state, 'output': str(output), 'attempt': 1, 'managed_complete': True,
            'tools_started': True, 'binding': {'host': host or self.host}}))
        (output / 'build-settings.json').write_text(json.dumps({'source_tree': str(output / 'build-kit/source')}))
        (output / name / 'Contents/old-app.txt').write_text('reads ../build-work/workspace')
        (output / 'home/gamedata/save.dat').write_text('synthetic save')
        (output / 'logs/launch-1a2b/engine.log').write_text('synthetic launch log')
        (output / 'logs/attempts/1/build.json').write_text('{}')
        (output / 'launch-error.log').write_text('synthetic launch error')
        return output


class PlayerLayoutTests(LayoutFixture, unittest.TestCase):
    def test_build_folder_sits_beside_the_mag_folder(self):
        self.assertEqual(player_build.build_folder(Path('/install/MAG')), Path('/install/MAG-build'))
        self.assertEqual(player_build.WINDOWS_BUILD_RESERVE, len('-build\\build-work'))

    def test_new_build_keeps_the_mag_folder_self_contained_and_relocatable(self):
        for host, name in (('macos-arm64', 'MAG.app'), ('windows-x86_64', 'MAG'), ('linux-x86_64', 'MAG')):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
                output, build = root / 'MAG', root / 'MAG-build'; events = []
                self.patches(stack, host=host)
                player_build.install(root, source, output, root / 'cache', events=events.append)
                self.assertEqual(sorted(p.name for p in output.iterdir()), [name, 'game'])
                self.assertEqual({p.name for p in build.iterdir()}, {'build.json', 'build-settings.json', 'build-work', 'logs'})
                done = events[-1]
                self.assertEqual((done['event'], done['output'], done['build']), ('player-build-complete', str(output), str(build)))
                self.assertIn('self-contained', done['message'])
                # Relocatable: nothing in the MAG folder names the build folder or the location;
                # moved elsewhere with the build folder deleted, its relative game path still resolves.
                for path in output.rglob('*'):
                    if path.is_file():
                        self.assertNotIn(str(root), path.read_text())
                moved = root / 'moved/MAG'; moved.parent.mkdir(); output.rename(moved); shutil.rmtree(build)
                game = json.loads((moved / name / 'runtime.json').read_text())['game']
                self.assertTrue((moved / name / game / 'PS3_GAME').resolve().is_dir())

    def test_publish_interrupted_after_the_rename_is_recorded_on_the_next_run(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output, build = root / 'MAG', root / 'MAG-build'
            self.patches(stack)
            player_build.install(root, source, output, root / 'cache')
            journal = json.loads((build / 'build.json').read_text())
            journal['state'] = 'publishing'; journal.pop('app')  # power lost right after the rename
            (build / 'build.json').write_text(json.dumps(journal))
            events = []
            self.assertEqual(player_build.install(root, source, output, root / 'cache', events=events.append), output)
            journal = json.loads((build / 'build.json').read_text())
            self.assertEqual((journal['state'], journal['app']), ('complete', str(output / 'MAG.app')))
            self.assertEqual(events[-1]['event'], 'player-build-complete')

    def test_finished_legacy_folder_moves_to_the_new_layout_without_rebuilding(self):
        for host, name in (('macos-arm64', 'MAG.app'), ('windows-x86_64', 'MAG'), ('linux-x86_64', 'MAG')):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                root = Path(temp).resolve(); output = self.legacy(root, name=name, host=host); build = root / 'MAG-build'
                calls = []
                def package(config, workspace, destination, **kwargs):
                    calls.append((config, workspace, destination))
                    destination.mkdir(); (destination / 'new-app.txt').write_text('reads ../game')
                    (destination.parent / 'game/PS3_GAME').mkdir(parents=True)
                    return destination
                tools, _ = self.patches(stack, package=package, host=host)
                events = []
                # The source folder is not read: no inputs, tools or compile are needed.
                self.assertEqual(player_build.install(root, root / 'absent', output, root / 'cache', events=events.append), output)
                tools.assert_not_called()
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0][1], output / 'build-work/workspace')  # the finished binaries, before the move
                self.assertEqual(calls[0][0], {'source_tree': str(output / 'build-kit/source')})
                self.assertEqual(sorted(p.name for p in output.iterdir()), sorted([name, 'game', 'home', 'logs', 'launch-error.log']))
                self.assertEqual((output / name / 'new-app.txt').read_text(), 'reads ../game')
                self.assertEqual((output / 'home/gamedata/save.dat').read_text(), 'synthetic save')
                self.assertEqual((output / 'logs/launch-1a2b/engine.log').read_text(), 'synthetic launch log')
                self.assertEqual(sorted(p.name for p in build.iterdir()),
                                 ['build-kit', 'build-settings.json', 'build-work', 'build.json', 'logs', 'managed-tools'])
                self.assertEqual(tree(build / 'logs'), ['attempts', 'attempts/1', 'attempts/1/build.json'])
                journal = json.loads((build / 'build.json').read_text())
                self.assertEqual((journal['layout'], journal['migration'], journal['state']), (2, 'complete', 'complete'))
                self.assertEqual((journal['output'], journal['build'], journal['app']), (str(output), str(build), str(output / name)))
                self.assertTrue(events[-1]['migrated']); self.assertIn('Nothing was rebuilt', events[-1]['message'])
                # Self-contained afterwards: move it and delete the build folder.
                moved = root / 'elsewhere/MAG'; moved.parent.mkdir(); output.rename(moved); shutil.rmtree(build)
                self.assertEqual((moved / 'home/gamedata/save.dat').read_text(), 'synthetic save')
                self.assertTrue((moved / 'game/PS3_GAME').is_dir())

    def test_interrupted_legacy_moves_continue_and_never_lose_saves(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); output = self.legacy(root); build = root / 'MAG-build'
            failures = [InstallerError('synthetic package failure')]
            def package(config, workspace, destination, **kwargs):
                destination.mkdir(); (destination.parent / 'game').mkdir()
                if failures: raise failures.pop()
                return destination
            self.patches(stack, package=package)
            # Packaging fails: the earlier folder still works as it was.
            with self.assertRaisesRegex(InstallerError, 'synthetic package failure'):
                player_build.install(root, root / 'absent', output, root / 'cache')
            self.assertTrue((output / 'MAG.app/Contents/old-app.txt').is_file())
            self.assertEqual((output / 'home/gamedata/save.dat').read_text(), 'synthetic save')
            self.assertEqual(json.loads((output / 'build.json').read_text())['migration'], 'staging')
            # Interrupted after packaging, while the player's files move: it carries on from there.
            with patch.object(Path, 'rename', side_effect=OSError('synthetic interruption')):
                with self.assertRaisesRegex(OSError, 'synthetic interruption'):
                    player_build.install(root, root / 'absent', output, root / 'cache')
            self.assertEqual(json.loads((output / 'build.json').read_text())['migration'], 'moving')
            self.assertEqual((output / 'home/gamedata/save.dat').read_text(), 'synthetic save')
            # Interrupted after the build files moved: the next Build MAG finishes the swap.
            with patch.object(player_build, '_finish_migration', side_effect=OSError('synthetic power loss')):
                with self.assertRaisesRegex(OSError, 'power loss'):
                    player_build.install(root, root / 'absent', output, root / 'cache')
            self.assertFalse(output.exists())
            self.assertEqual((build / 'package' / 'home' / 'gamedata' / 'save.dat').read_text(), 'synthetic save')
            self.assertEqual(json.loads((build / 'build.json').read_text())['migration'], 'swapping')
            player_build.install(root, root / 'absent', output, root / 'cache')
            self.assertEqual((output / 'home/gamedata/save.dat').read_text(), 'synthetic save')
            self.assertFalse((build / 'MAG.app').exists())
            self.assertEqual(json.loads((build / 'build.json').read_text())['migration'], 'complete')

    def test_legacy_folder_with_unknown_files_or_an_existing_build_folder_is_preserved(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); output = self.legacy(root)
            _, packager = self.patches(stack)
            (output / 'notes.txt').write_text('the player\'s own file')
            before = tree(output)
            with self.assertRaisesRegex(InstallerError, 'notes.txt'):
                player_build.install(root, root / 'absent', output, root / 'cache')
            (output / 'notes.txt').unlink()
            (root / 'MAG-build').mkdir()
            with self.assertRaisesRegex(InstallerError, 'already exists'):
                player_build.install(root, root / 'absent', output, root / 'cache')
            packager.assert_not_called()
            self.assertEqual(tree(output), [p for p in before if p != 'notes.txt'])

    def test_failed_legacy_build_retries_from_the_build_folder(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output, build = root / 'MAG', root / 'MAG-build'
            failures = [InstallerError('synthetic failure')]
            def native(config, work, **kwargs):
                (work / 'workspace').mkdir(parents=True)
                if failures: raise failures.pop()
            tools, _ = self.patches(stack, native=native)
            with self.assertRaisesRegex(InstallerError, 'synthetic failure'):
                player_build.install(root, source, output, root / 'cache')
            # Recreate what an earlier installer left: the same failed build inside the MAG folder.
            build.rename(output)
            journal = json.loads((output / 'build.json').read_text())
            for key in ('layout', 'build'): journal.pop(key)
            (output / 'build.json').write_text(json.dumps(journal))
            (output / 'managed-tools').mkdir()
            self.assertTrue(PlayerBuildController(root, readiness=lambda r: {'ready': True}).has_journal(root))
            player_build.install(root, source, output, root / 'cache', retry=True)
            self.assertEqual(sorted(p.name for p in output.iterdir()), ['MAG.app', 'game'])
            # Managed tools record their old location, so the retry set them aside and rebuilt them.
            self.assertTrue((build / 'logs/attempts/1/managed-tools').is_dir())
            self.assertTrue((build / 'logs/attempts/1/build-work/workspace').is_dir())
            self.assertEqual(tools.call_count, 2)
            self.assertEqual(json.loads((build / 'build.json').read_text())['state'], 'complete')

    def test_gui_offers_retry_and_the_layout_move(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            controller = PlayerBuildController(root, readiness=lambda r: {'ready': True})
            self.assertFalse(controller.has_journal(root)); self.assertFalse(controller.legacy_install(root))
            (root / 'MAG-build').mkdir()
            (root / 'MAG-build/build.json').write_text(json.dumps({'schema_version': 2, 'state': 'incomplete'}))
            self.assertTrue(controller.has_journal(root))
            shutil.rmtree(root / 'MAG-build')
            self.legacy(root)
            self.assertTrue(controller.legacy_install(root)); self.assertFalse(controller.has_journal(root))
            calls = []
            controller = PlayerBuildController(root, readiness=lambda r: {'ready': True},
                                               installer=lambda *args, **kwargs: calls.append(args) or args[2])
            controller.start(root / 'disc', root, root / 'cache')  # an existing earlier-layout folder is accepted
            controller.worker.join(2); controller.poll()
            self.assertEqual((controller.state, controller.logs), ('complete', root / 'MAG-build/logs'))


@unittest.skipUnless(platform.system() == 'Darwin' and Path('/usr/bin/xcrun').is_file(), 'macOS packaging and supervisor')
class RealMacMigrationTests(LayoutFixture, unittest.TestCase):
    def test_moved_legacy_install_starts_after_its_build_folder_is_deleted(self):
        """Real packager and supervisor with fixture images: a finished earlier-layout MAG folder is
        moved to the new layout, then relocated with its build folder deleted, and still starts."""
        import test_native_package as fixtures
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); output = self.legacy(root); build = root / 'MAG-build'
            shutil.rmtree(output / 'build-work/workspace')
            scratch = root / 'scratch'; scratch.mkdir()
            fixtures.NativePackageTests().supervisor_workspace(scratch).rename(output / 'build-work/workspace')
            from mag_installer.native_package import package as real_package
            self.patches(stack, package=real_package)
            player_build.install(root, root / 'absent', output, root / 'cache')
            runtime = json.loads((output / 'MAG.app/Contents/Resources/runtime.json').read_text())
            self.assertEqual(runtime['game'], '../game')
            self.assertTrue((output / 'game/PS3_GAME/USRDIR/EBOOT.elf').samefile(
                build / 'build-work/workspace/local/retail-v1.0/PS3_GAME/USRDIR/EBOOT.elf'))
            moved = root / 'elsewhere/MAG'; moved.parent.mkdir(); output.rename(moved); shutil.rmtree(build)
            result = subprocess.run([str(moved / 'MAG.app/Contents/MacOS/MAG')], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 17, result.stderr)
            # The save written before the move is still there, and the new launch added to it.
            self.assertEqual((moved / 'home/gamedata/save.dat').read_text(), 'synthetic save')
            self.assertEqual((moved / 'home/gamedata/save.txt').read_text(), 'x\n')
            self.assertEqual(len(list((moved / 'logs').glob('launch-*'))), 2)


if __name__ == '__main__':
    unittest.main()
