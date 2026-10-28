import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import io
from contextlib import redirect_stdout

from mag_installer import player_build
from mag_installer.model import InstallerError


class PlayerBuildTests(unittest.TestCase):
    def test_player_cli_uses_same_automatic_flow_without_recipe_argument(self):
        from mag_installer.__main__ import main
        with patch('mag_installer.player_build.install') as build, redirect_stdout(io.StringIO()):
            self.assertEqual(main(['build', '--source', '/input', '--parent', '/destination']), 0)
            self.assertEqual(build.call_args.args[1:3], (Path('/input'), Path('/destination/MAG')))
            self.assertFalse(build.call_args.kwargs['retry'])
            self.assertEqual(main(['build', '--source', '/input', '--output', '/destination/MAG', '--retry']), 0)
            self.assertTrue(build.call_args.kwargs['retry'])

    def inputs(self, root):
        for name in ('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN',
                     'PS3_GAME/USRDIR/EBOOT.elf', 'PS3_GAME/USRDIR/bin/mage_g.self',
                     'PS3_GAME/USRDIR/bin/mage_g.elf', 'firmware/libsre.prx'):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic input')

    def test_input_parent_and_disc_child_resolve_same_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            self.inputs(root)
            self.assertEqual(player_build.locate_inputs(root), player_build.locate_inputs(root / 'PS3_GAME'))
            (root / 'firmware/libsre.prx').unlink()
            with self.assertRaisesRegex(InstallerError, 'firmware/libsre.prx'):
                player_build.locate_inputs(root)

    def test_single_action_orders_tools_native_build_and_final_package(self):
        for fail_package in (False, True):
            with self.subTest(fail_package=fail_package), tempfile.TemporaryDirectory() as temp:
                root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
                output = root / 'MAG'; cache = root / 'cache'; steps = []
                kit = {'source_tree': 'source', 'sdks': {role: {'path': 'sdks/' + role, 'revision': 'a' * 40}
                       for role in ('analysis', 'ppu', 'runtime', 'spu')}}
                def tools(managed, **kwargs):
                    steps.append('tools')
                    self.assertEqual(managed, output / 'managed-tools')
                    return {'python': '/managed/python', 'cmake': '/managed/cmake', 'ninja': '/managed/ninja',
                            'git': '/usr/bin/git', 'ffmpeg': {}, 'prefix': '/managed/prefix'}
                def native(config_path, native_output, **kwargs):
                    steps.append('native')
                    config = json.loads(config_path.read_text())
                    self.assertEqual(config['managed_prefix'], '/managed/prefix')
                    self.assertEqual(config['jobs'], 2)
                    (native_output / 'workspace').mkdir(parents=True)
                def package(config, workspace, destination, **kwargs):
                    steps.append('package')
                    self.assertEqual(workspace, output / 'build-work/workspace')
                    self.assertEqual(destination, output / 'MAG.app')
                    if fail_package:
                        raise InstallerError('synthetic package failure')
                    destination.mkdir()
                    return destination
                with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                     patch.object(player_build, '_manifest', return_value={'kit': {}, 'host': 'macos-arm64'}), \
                     patch.object(player_build, '_load_kit', return_value=kit), \
                     patch('mag_installer.native_build._verify_identities'), \
                     patch('mag_installer.managed_tools.ensure_apple_sdk'), \
                     patch('mag_installer.managed_tools.bootstrap', side_effect=tools), \
                     patch('mag_installer.native_build.install', side_effect=native), \
                     patch('mag_installer.native_package.package', side_effect=package):
                    if fail_package:
                        with self.assertRaisesRegex(InstallerError, 'package failure'):
                            player_build.install(root, source, output, cache)
                    else:
                        self.assertEqual(player_build.install(root, source, output, cache), output)
                    self.assertEqual(steps, ['tools', 'native', 'package'])
                    self.assertEqual(json.loads((output / 'build.json').read_text())['state'],
                                     'incomplete' if fail_package else 'complete')
                    with self.assertRaisesRegex(InstallerError, 'already exists'):
                        player_build.install(root, source, output, cache)
                    self.assertEqual((source / 'PS3_GAME/USRDIR/EBOOT.elf').read_bytes(), b'synthetic input')

    def test_windows_route_uses_build_tools_msvc_record_and_mag_folder(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output = root / 'MAG'; cache = root / 'cache'; steps = []
            kit = {'source_tree': 'source', 'sdks': {role: {'path': 'sdks/' + role, 'revision': 'a' * 40}
                   for role in ('analysis', 'ppu', 'runtime', 'spu')}}
            toolchain = {'vcvars': 'C:/VS/VC/Auxiliary/Build/vcvars64.bat', 'clang_cl': 'C:/VS/VC/Tools/Llvm/x64/bin/clang-cl.exe',
                         'installation': 'C:/VS', 'version': '17.14', 'product': 'Microsoft.VisualStudio.Product.BuildTools'}
            def build_tools(**kwargs):
                steps.append('build-tools'); return toolchain
            def tools(managed, **kwargs):
                steps.append('tools')
                return {'python': 'C:/m/python.exe', 'cmake': 'C:/m/cmake.exe', 'ninja': 'C:/m/ninja.exe', 'git': 'C:/m/git.exe'}
            def native(config_path, native_output, **kwargs):
                steps.append('native')
                config = json.loads(config_path.read_text())
                self.assertEqual(config['msvc'], toolchain)
                self.assertNotIn('ffmpeg', config)
                (native_output / 'workspace').mkdir(parents=True)
            def package(config, workspace, destination, **kwargs):
                steps.append('package')
                self.assertEqual(destination, output / 'MAG')
                destination.mkdir(); return destination
            with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                 patch.object(player_build, '_manifest', return_value={'kit': {}, 'host': 'windows-x86_64'}), \
                 patch.object(player_build, '_load_kit', return_value=kit), \
                 patch('mag_installer.native_build._verify_identities'), \
                 patch('mag_installer.native_build.check_windows_location'), \
                 patch('mag_installer.managed_tools.ensure_windows_build_tools', side_effect=build_tools), \
                 patch('mag_installer.managed_tools.ensure_apple_sdk', side_effect=AssertionError('no Apple SDK on Windows')), \
                 patch('mag_installer.managed_tools.bootstrap', side_effect=tools), \
                 patch('mag_installer.native_build.install', side_effect=native), \
                 patch('mag_installer.native_package.package', side_effect=package):
                self.assertEqual(player_build.install(root, source, output, cache), output)
            self.assertEqual(steps, ['build-tools', 'tools', 'native', 'package'])

    def test_missing_prepared_input_prevents_tool_download_and_output_creation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'inputs'; source.mkdir()
            with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                 patch('mag_installer.managed_tools.bootstrap') as bootstrap:
                with self.assertRaisesRegex(InstallerError, 'missing required prepared files'):
                    player_build.install(root, source, root / 'MAG', root / 'cache')
                bootstrap.assert_not_called()
                self.assertFalse((root / 'MAG').exists())

    def test_identity_failure_precedes_all_tool_setup(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                 patch.object(player_build, '_manifest', return_value={'kit': {}, 'host': 'macos-arm64'}), \
                 patch.object(player_build, '_load_kit', return_value={'source_tree': 'source'}), \
                 patch('mag_installer.native_build._verify_identities', side_effect=InstallerError('wrong identity')), \
                 patch('mag_installer.managed_tools.ensure_apple_sdk') as sdk, \
                 patch('mag_installer.managed_tools.bootstrap') as tools:
                with self.assertRaisesRegex(InstallerError, 'wrong identity'):
                    player_build.install(root, source, root / 'MAG', root / 'cache')
                sdk.assert_not_called(); tools.assert_not_called()

    def test_failed_and_cancelled_build_retry_revalidates_and_retains_evidence(self):
        from contextlib import ExitStack
        from mag_installer.safety import Cancelled
        for failure in (InstallerError('synthetic failure'), Cancelled('synthetic cancellation')):
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
                output = root / 'MAG'; cache = root / 'cache'
                manifest = {'host': 'macos-arm64', 'kit': {}}
                stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
                stack.enter_context(patch.object(player_build, '_manifest', side_effect=lambda root: manifest))
                stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree':'source', 'sdks': {}}))
                verify = stack.enter_context(patch('mag_installer.native_build._verify_identities'))
                stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
                stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={
                    'python':'python', 'cmake':'cmake', 'ninja':'ninja', 'git':'git', 'ffmpeg':{}}))
                def native(config, work, **kwargs):
                    (work / 'logs').mkdir(parents=True)
                    (work / 'logs/failure.log').write_text('retained error evidence')
                stack.enter_context(patch('mag_installer.native_build.install', side_effect=native))
                package = stack.enter_context(patch('mag_installer.native_package.package', side_effect=failure))
                with self.assertRaises(type(failure)):
                    player_build.install(root, source, output, cache)
                original = json.loads((output / 'build.json').read_text())
                manifest['kit'] = {'sha256': 'different'}
                with self.assertRaisesRegex(InstallerError, 'matching incomplete'):
                    player_build.install(root, source, output, cache, retry=True)
                manifest['kit'] = {}
                path = source / 'PS3_GAME/USRDIR/EBOOT.elf'; path.write_bytes(b'changed')
                with self.assertRaisesRegex(InstallerError, 'matching incomplete'):
                    player_build.install(root, source, output, cache, retry=True)
                path.write_bytes(b'synthetic input')
                (output / 'unrelated').write_text('preserve')
                with self.assertRaisesRegex(InstallerError, 'Unknown files'):
                    player_build.install(root, source, output, cache, retry=True)
                (output / 'unrelated').unlink()
                # A killed process leaves 'building'; only the kernel lease may
                # authorize replay, never the recorded PID or journal alone.
                stale = json.loads((output / 'build.json').read_text())
                stale['state'] = 'building'
                (output / 'build.json').write_text(json.dumps(stale))
                from mag_installer.pipeline import _exclusive_lock
                with _exclusive_lock(output.with_name('.MAG.player-lock'), True):
                    with self.assertRaisesRegex(InstallerError, 'Another installer holds'):
                        player_build.install(root, source, output, cache, retry=True)
                with _exclusive_lock(output / '.build-work.native-lock', True):
                    with self.assertRaisesRegex(InstallerError, 'Another installer holds'):
                        player_build.install(root, source, output, cache, retry=True)
                # Cleanup failures retain a retryable journal and original
                # diagnostics, without accumulating a second workspace copy.
                with patch('mag_installer.player_build.shutil.rmtree', side_effect=OSError('cleanup interrupted')):
                    with self.assertRaisesRegex(OSError, 'cleanup interrupted'):
                        player_build.install(root, source, output, cache, retry=True)
                self.assertEqual(json.loads((output / 'build.json').read_text())['state'], 'incomplete')
                package.side_effect = lambda config, workspace, destination, **kwargs: destination
                player_build.install(root, source, output, cache, retry=True)
                self.assertEqual(verify.call_count, 2)
                self.assertFalse((output / 'logs/attempts/1/build-work/workspace').exists())
                self.assertEqual((output / 'logs/attempts/1/build-work/logs/failure.log').read_text(), 'retained error evidence')
                self.assertEqual(json.loads((output / 'logs/attempts/1/build.json').read_text())['error'], original['error'])
                with self.assertRaisesRegex(InstallerError, 'matching incomplete'):
                    player_build.install(root, source, output, cache, retry=True)

    def test_unknown_and_symlink_retry_output_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'MAG'; output.mkdir()
            with self.assertRaisesRegex(InstallerError, 'Unknown output'):
                player_build._retry_status(output, {})
            linked = Path(temp) / 'link'; linked.symlink_to(output, target_is_directory=True)
            with self.assertRaisesRegex(InstallerError, 'without symlinks'):
                player_build._retry_status(linked, {})

    def test_linux_route_uses_compilers_ffmpeg_and_mag_directory(self):
        from contextlib import ExitStack
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output = root / 'MAG'
            stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
            stack.enter_context(patch.object(player_build, '_manifest', return_value={'host':'linux-x86_64', 'kit':{}}))
            stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree':'source', 'sdks':{}}))
            stack.enter_context(patch('mag_installer.native_build._verify_identities'))
            stack.enter_context(patch('mag_installer.managed_tools.ensure_linux_build_tools', return_value={'cc':'/usr/bin/clang', 'cxx':'/usr/bin/clang++'}))
            apple = stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
            stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={'python':'python', 'cmake':'cmake', 'ninja':'ninja', 'git':'git', 'ffmpeg':{'avcodec':'codec.so'}}))
            def native(config, work, **kwargs):
                data = json.loads(config.read_text())
                self.assertEqual(data['host'], 'linux-x86_64')
                self.assertEqual(data['linux_toolchain'], {'cc':'/usr/bin/clang', 'cxx':'/usr/bin/clang++'})
                self.assertEqual(data['ffmpeg'], {'avcodec':'codec.so'})
            stack.enter_context(patch('mag_installer.native_build.install', side_effect=native))
            package = stack.enter_context(patch('mag_installer.native_package.package', side_effect=lambda config, work, target, **kwargs: target))
            player_build.install(root, source, output, root / 'cache')
            self.assertEqual(package.call_args.args[2], output / 'MAG')
            apple.assert_not_called()

    def test_rejected_game_identity_can_be_corrected_in_same_destination(self):
        from contextlib import ExitStack
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output = root / 'MAG'; path = source / 'PS3_GAME/USRDIR/EBOOT.elf'
            stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
            stack.enter_context(patch.object(player_build, '_manifest', return_value={'host':'macos-arm64', 'kit':{}}))
            stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree':'source', 'sdks':{}}))
            def verify(*args):
                if path.read_bytes() != b'corrected synthetic input':
                    raise InstallerError('wrong input identity')
            stack.enter_context(patch('mag_installer.native_build._verify_identities', side_effect=verify))
            sdk = stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
            tools = stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={'python':'python', 'cmake':'cmake', 'ninja':'ninja', 'git':'git', 'ffmpeg':{}}))
            stack.enter_context(patch('mag_installer.native_build.install'))
            stack.enter_context(patch('mag_installer.native_package.package', side_effect=lambda config, work, target, **kwargs: target))
            with self.assertRaisesRegex(InstallerError, 'wrong input identity'):
                player_build.install(root, source, output, root / 'cache')
            sdk.assert_not_called(); tools.assert_not_called()
            rejected = json.loads((output / 'build.json').read_text())
            path.write_bytes(b'corrected synthetic input')
            player_build.install(root, source, output, root / 'cache', retry=True)
            self.assertEqual(json.loads((output / 'build.json').read_text())['state'], 'complete')
            self.assertEqual(json.loads((output / 'logs/attempts/1/build.json').read_text()), rejected)
            self.assertEqual(tools.call_count, 1)
