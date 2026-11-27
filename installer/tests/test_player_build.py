import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import io
from contextlib import redirect_stdout

from mag_installer import build_jobs, player_build
from mag_installer.model import InstallerError


RUST = {'cargo': '/managed/cargo', 'rustc': '/managed/rustc'}


CCACHE = '/managed/artifacts/ccache/ccache'


class PlayerBuildTests(unittest.TestCase):
    def setUp(self):
        # No test downloads the pinned Rust toolchain; each sees where the build would get it.
        patcher = patch('mag_installer.managed_tools.rust_toolchain', return_value=RUST)
        self.rust = patcher.start()
        self.addCleanup(patcher.stop)
        environment = patch.dict('os.environ')
        environment.start(); self.addCleanup(environment.stop)
        os.environ.pop('MAG_GAME_CONTROLS', None)
        os.environ.pop('MAG_BUILD_CACHE', None)
        os.environ.pop('MAG_VOICE', None)
        # Nor the pinned ccache: each sees where the build would get it.
        ccache = patch('mag_installer.compile_cache.ccache_tool', return_value=CCACHE)
        self.ccache = ccache.start(); self.addCleanup(ccache.stop)

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

    def test_firmware_steps_shown_only_when_firmware_is_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); self.inputs(root)
            (root / 'PS3_GAME/USRDIR/EBOOT.elf').unlink()
            with self.assertRaises(InstallerError) as raised: player_build.locate_inputs(root)
            self.assertNotIn('Where to get firmware/libsre.prx', str(raised.exception))
            (root / 'firmware/libsre.prx').unlink()
            with self.assertRaises(InstallerError) as raised: player_build.locate_inputs(root)
            self.assertIn('firmware/libsre.prx', str(raised.exception))
            self.assertIn('rpcs3 --installfw', str(raised.exception))

    def test_single_action_orders_tools_native_build_and_final_package(self):
        for fail_package in (False, True):
            with self.subTest(fail_package=fail_package), tempfile.TemporaryDirectory() as temp:
                root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
                output = root / 'MAG'; build = root / 'MAG-build'; cache = root / 'cache'; steps = []
                kit = {'source_tree': 'source', 'sdks': {role: {'path': 'sdks/' + role, 'revision': 'a' * 40}
                       for role in ('analysis', 'ppu', 'runtime', 'spu')}}
                def tools(managed, **kwargs):
                    steps.append('tools')
                    self.assertEqual(kwargs['build_cache'].root, cache / 'build-cache')
                    self.assertFalse(output.exists())  # the MAG folder appears only when complete
                    self.assertEqual(managed, build / 'managed-tools')
                    return {'root': '/managed', 'python': '/managed/python', 'cmake': '/managed/cmake', 'ninja': '/managed/ninja',
                            'git': '/usr/bin/git', 'ffmpeg': {}, 'prefix': '/managed/prefix'}
                def native(config_path, native_output, **kwargs):
                    steps.append('native')
                    config = json.loads(config_path.read_text())
                    self.assertEqual(config['managed_prefix'], '/managed/prefix')
                    self.assertEqual(config['jobs'], build_jobs.recommended())  # nothing chosen yet
                    # The per-user build cache: inside the installer cache, never the build folder.
                    self.assertEqual(config['build_cache'], {'root': str(cache / 'build-cache'), 'base_dir': str(build),
                                                             'ccache': CCACHE})
                    (native_output / 'workspace').mkdir(parents=True)
                def package(config, workspace, destination, **kwargs):
                    steps.append('package')
                    self.assertEqual(workspace, build / 'build-work/workspace')
                    self.assertEqual(destination, build / 'package/MAG.app')
                    if fail_package:
                        raise InstallerError('synthetic package failure')
                    destination.mkdir(); (destination.parent / 'game').mkdir()
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
                    journal = json.loads((build / 'build.json').read_text())
                    self.assertEqual(journal['state'], 'incomplete' if fail_package else 'complete')
                    self.assertEqual((journal['output'], journal['layout']), (str(output), 2))
                    if fail_package:
                        self.assertFalse(output.exists())
                    else:
                        # The MAG folder holds the product only; the build files are beside it.
                        self.assertEqual(sorted(p.name for p in output.iterdir()), ['MAG.app', 'game'])
                        self.assertEqual(journal['app'], str(output / 'MAG.app'))
                        self.assertFalse((build / 'package').exists())
                    self.assertTrue({'build.json', 'build-settings.json', 'build-work', 'logs'} <= {p.name for p in build.iterdir()})
                    with self.assertRaisesRegex(InstallerError, 'already exists' if not fail_package else 'Build files from an earlier MAG build'):
                        player_build.install(root, source, output, cache)
                    self.assertEqual((source / 'PS3_GAME/USRDIR/EBOOT.elf').read_bytes(), b'synthetic input')

    def test_build_cache_switch_off_builds_without_it_and_invalid_values_stop_early(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output = root / 'MAG'; cache = root / 'cache'
            kit = {'source_tree': 'source', 'sdks': {role: {'path': 'sdks/' + role, 'revision': 'a' * 40}
                   for role in ('analysis', 'ppu', 'runtime', 'spu')}}
            seen = {}
            def tools(managed, **kwargs):
                seen['tools'] = kwargs
                return {'root': '/managed', 'python': '/managed/python', 'cmake': '/managed/cmake', 'ninja': '/managed/ninja',
                        'git': '/usr/bin/git', 'ffmpeg': {}, 'prefix': '/managed/prefix'}
            def native(config_path, native_output, **kwargs):
                seen['config'] = json.loads(config_path.read_text())
                (native_output / 'workspace').mkdir(parents=True)
            def package(config, workspace, destination, **kwargs):
                destination.mkdir(); return destination
            with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                 patch.object(player_build, '_manifest', return_value={'kit': {}, 'host': 'macos-arm64'}), \
                 patch.object(player_build, '_load_kit', return_value=kit), \
                 patch('mag_installer.native_build._verify_identities'), \
                 patch('mag_installer.managed_tools.ensure_apple_sdk'), \
                 patch('mag_installer.managed_tools.bootstrap', side_effect=tools), \
                 patch('mag_installer.native_build.install', side_effect=native), \
                 patch('mag_installer.native_package.package', side_effect=package):
                os.environ['MAG_BUILD_CACHE'] = 'yes'
                with self.assertRaisesRegex(InstallerError, 'MAG_BUILD_CACHE must be 0 or 1'):
                    player_build.install(root, source, output, cache)
                self.assertFalse(output.with_name('MAG-build').exists())
                os.environ['MAG_BUILD_CACHE'] = '0'
                self.assertEqual(player_build.install(root, source, output, cache), output)
            self.assertNotIn('build_cache', seen['tools'])
            self.assertNotIn('build_cache', seen['config'])
            self.assertFalse((cache / 'build-cache').exists())
            self.ccache.assert_not_called()

    def test_windows_route_uses_build_tools_msvc_record_and_mag_folder(self):
        prebuilt = {'ffmpeg': {'include': 'C:/m/ffmpeg/include', 'avcodec': 'C:/m/ffmpeg/lib/avcodec.lib'},
                    'sdl2': {'cmake_dir': 'C:/m/SDL2/cmake', 'dll': 'C:/m/SDL2/lib/x64/SDL2.dll', 'license': 'C:/m/SDL2/LICENSE.txt'}}
        for extra in ({}, prebuilt):
            with self.subTest(prebuilt=sorted(extra)), tempfile.TemporaryDirectory() as temp:
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
                    return {'root': 'C:/m', 'python': 'C:/m/python.exe', 'cmake': 'C:/m/cmake.exe', 'ninja': 'C:/m/ninja.exe', 'git': 'C:/m/git.exe', **extra}
                def native(config_path, native_output, **kwargs):
                    steps.append('native')
                    config = json.loads(config_path.read_text())
                    self.assertEqual(config['msvc'], toolchain)
                    # Windows reuses tool builds and generation, but compiles without ccache.
                    self.assertEqual(set(config['build_cache']), {'root', 'base_dir'})
                    # The prebuilt movie decoder and controller library pass through when acquired.
                    for key in ('ffmpeg', 'sdl2'): self.assertEqual(config.get(key), extra.get(key))
                    (native_output / 'workspace').mkdir(parents=True)
                def package(config, workspace, destination, **kwargs):
                    steps.append('package')
                    self.assertEqual(destination, root / 'MAG-build/package/MAG')
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
                self.assertTrue((output / 'MAG').is_dir())
                self.ccache.assert_not_called()

    def test_missing_prepared_input_prevents_tool_download_and_output_creation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); source = root / 'inputs'; source.mkdir()
            with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                 patch('mag_installer.managed_tools.bootstrap') as bootstrap:
                with self.assertRaisesRegex(InstallerError, 'missing required prepared files') as raised:
                    player_build.install(root, source, root / 'MAG', root / 'cache')
                self.assertIn('Where to get firmware/libsre.prx', str(raised.exception))
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
                    'root':'managed', 'python':'python', 'cmake':'cmake', 'ninja':'ninja', 'git':'git', 'ffmpeg':{}}))
                def native(config, work, **kwargs):
                    (work / 'logs').mkdir(parents=True)
                    (work / 'logs/failure.log').write_text('retained error evidence')
                    (work / 'workspace').mkdir()
                    (work / 'workspace/lifted.txt').write_text('expensive attempt output')
                stack.enter_context(patch('mag_installer.native_build.install', side_effect=native))
                package = stack.enter_context(patch('mag_installer.native_package.package', side_effect=failure))
                build = root / 'MAG-build'
                with self.assertRaises(type(failure)):
                    player_build.install(root, source, output, cache)
                self.assertFalse(output.exists())
                original = json.loads((build / 'build.json').read_text())
                manifest['kit'] = {'sha256': 'different'}
                with self.assertRaisesRegex(InstallerError, 'matching incomplete'):
                    player_build.install(root, source, output, cache, retry=True)
                manifest['kit'] = {}
                path = source / 'PS3_GAME/USRDIR/EBOOT.elf'; path.write_bytes(b'changed')
                with self.assertRaisesRegex(InstallerError, 'matching incomplete'):
                    player_build.install(root, source, output, cache, retry=True)
                path.write_bytes(b'synthetic input')
                (build / 'unrelated').write_text('preserve')
                with self.assertRaisesRegex(InstallerError, 'Unknown files'):
                    player_build.install(root, source, output, cache, retry=True)
                (build / 'unrelated').unlink()
                output.mkdir()
                with self.assertRaisesRegex(InstallerError, 'Move it aside'):
                    player_build.install(root, source, output, cache, retry=True)
                output.rmdir()
                # A killed process leaves 'building'; only the kernel lease may
                # authorize replay, never the recorded PID or journal alone.
                stale = json.loads((build / 'build.json').read_text())
                stale['state'] = 'building'
                (build / 'build.json').write_text(json.dumps(stale))
                from mag_installer.pipeline import _exclusive_lock
                with _exclusive_lock(output.with_name('.MAG.player-lock'), True):
                    with self.assertRaisesRegex(InstallerError, 'Another installer holds'):
                        player_build.install(root, source, output, cache, retry=True)
                with _exclusive_lock(build / '.build-work.native-lock', True):
                    with self.assertRaisesRegex(InstallerError, 'Another installer holds'):
                        player_build.install(root, source, output, cache, retry=True)
                # Cleanup failures retain a retryable journal and original
                # diagnostics, and lose no attempt output.
                with patch('pathlib.Path.rename', side_effect=OSError('cleanup interrupted')):
                    with self.assertRaisesRegex(OSError, 'cleanup interrupted'):
                        player_build.install(root, source, output, cache, retry=True)
                self.assertEqual(json.loads((build / 'build.json').read_text())['state'], 'incomplete')
                self.assertEqual((build / 'build-work/workspace/lifted.txt').read_text(), 'expensive attempt output')
                # A retry that fails again keeps the newest failed attempt's output.
                with self.assertRaises(type(failure)):
                    player_build.install(root, source, output, cache, retry=True)
                self.assertEqual((build / 'logs/attempts/1/build-work/workspace/lifted.txt').read_text(), 'expensive attempt output')
                self.assertEqual((build / 'logs/attempts/1/build-work/logs/failure.log').read_text(), 'retained error evidence')
                self.assertTrue((build / 'logs/attempts/1/package').is_dir())  # the unfinished MAG folder stage
                package.side_effect = lambda config, workspace, destination, **kwargs: destination
                player_build.install(root, source, output, cache, retry=True)
                self.assertEqual(verify.call_count, 3)
                self.assertTrue(output.is_dir())
                # Older attempts keep logs and journal only: at most one extra output copy.
                self.assertFalse((build / 'logs/attempts/1/build-work/workspace').exists())
                self.assertEqual((build / 'logs/attempts/1/build-work/logs/failure.log').read_text(), 'retained error evidence')
                self.assertEqual((build / 'logs/attempts/2/build-work/workspace/lifted.txt').read_text(), 'expensive attempt output')
                self.assertEqual(json.loads((build / 'logs/attempts/1/build.json').read_text())['error'], original['error'])
                with self.assertRaisesRegex(InstallerError, 'already exists'):
                    player_build.install(root, source, output, cache, retry=True)
                output.rename(root / 'moved MAG')
                with self.assertRaisesRegex(InstallerError, 'matching incomplete'):
                    player_build.install(root, source, output, cache, retry=True)

    def test_unknown_and_symlink_retry_output_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / 'MAG-build'; build.mkdir()
            with self.assertRaisesRegex(InstallerError, 'Unknown build folder'):
                player_build._retry_status(build, Path(temp) / 'MAG', {})
            linked = Path(temp) / 'link'; linked.symlink_to(build, target_is_directory=True)
            with self.assertRaisesRegex(InstallerError, 'without symlinks'):
                player_build._retry_status(linked, Path(temp) / 'MAG', {})

    def test_linux_route_uses_compilers_ffmpeg_and_mag_directory(self):
        self.assertNotIn('cmake_args', self.linux_route({}))

    def test_game_controls_are_built_by_default_with_the_pinned_rust(self):
        settings = self.linux_route({})
        self.assertEqual(settings['game_controls'], RUST)
        self.assertEqual(self.rust.call_args.args[0], 'managed')
        self.assertEqual(self.linux_route({'MAG_GAME_CONTROLS': '1'})['game_controls'], RUST)

    def test_game_controls_switch_off_skips_the_adapter_and_its_toolchain(self):
        self.rust.reset_mock()
        self.assertNotIn('game_controls', self.linux_route({'MAG_GAME_CONTROLS': '0'}))
        self.rust.assert_not_called()

    def test_invalid_game_controls_switch_stops_before_any_tool_setup(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict('os.environ', {'MAG_GAME_CONTROLS': 'off'}):
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            with patch.object(player_build, 'readiness', return_value={'ready': True}), \
                 patch.object(player_build, '_manifest', return_value={'kit': {}, 'host': 'linux-x86_64'}), \
                 patch('mag_installer.managed_tools.bootstrap') as tools:
                with self.assertRaisesRegex(InstallerError, 'MAG_GAME_CONTROLS must be 0 or 1'):
                    player_build.install(root, source, root / 'MAG', root / 'cache')
                tools.assert_not_called()
            self.assertFalse((root / 'MAG').exists())

    def test_x86_level_override_reaches_the_build_settings(self):
        self.assertEqual(self.linux_route({'MAG_X86_LEVEL': 'baseline'})['cmake_args'], ['-DMAG_X86_LEVEL=baseline'])

    def linux_route(self, environ):
        from contextlib import ExitStack
        settings = {}
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            stack.enter_context(patch.dict('os.environ', environ))
            if not environ: os.environ.pop('MAG_X86_LEVEL', None)
            root = Path(temp).resolve(); source = root / 'inputs'; self.inputs(source)
            output = root / 'MAG'
            stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
            stack.enter_context(patch.object(player_build, '_manifest', return_value={'host':'linux-x86_64', 'kit':{}}))
            stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree':'source', 'sdks':{}}))
            stack.enter_context(patch('mag_installer.native_build._verify_identities'))
            stack.enter_context(patch('mag_installer.managed_tools.ensure_linux_build_tools', return_value={'cc':'/usr/bin/clang', 'cxx':'/usr/bin/clang++'}))
            apple = stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
            stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={'root':'managed', 'python':'python', 'cmake':'cmake', 'ninja':'ninja', 'git':'git', 'ffmpeg':{'avcodec':'codec.so'}}))
            def native(config, work, **kwargs):
                data = json.loads(config.read_text())
                self.assertEqual(data['host'], 'linux-x86_64')
                self.assertEqual(data['linux_toolchain'], {'cc':'/usr/bin/clang', 'cxx':'/usr/bin/clang++'})
                self.assertEqual(data['ffmpeg'], {'avcodec':'codec.so'})
                settings.update(data)
            stack.enter_context(patch('mag_installer.native_build.install', side_effect=native))
            package = stack.enter_context(patch('mag_installer.native_package.package', side_effect=lambda config, work, target, **kwargs: target))
            player_build.install(root, source, output, root / 'cache')
            self.assertEqual(package.call_args.args[2], root / 'MAG-build/package/MAG')
            apple.assert_not_called()
        return settings

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
            tools = stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={'root':'managed', 'python':'python', 'cmake':'cmake', 'ninja':'ninja', 'git':'git', 'ffmpeg':{}}))
            stack.enter_context(patch('mag_installer.native_build.install'))
            stack.enter_context(patch('mag_installer.native_package.package', side_effect=lambda config, work, target, **kwargs: target))
            with self.assertRaisesRegex(InstallerError, 'wrong input identity'):
                player_build.install(root, source, output, root / 'cache')
            sdk.assert_not_called(); tools.assert_not_called()
            build = root / 'MAG-build'
            rejected = json.loads((build / 'build.json').read_text())
            path.write_bytes(b'corrected synthetic input')
            player_build.install(root, source, output, root / 'cache', retry=True)
            self.assertEqual(json.loads((build / 'build.json').read_text())['state'], 'complete')
            self.assertEqual(json.loads((build / 'logs/attempts/1/build.json').read_text()), rejected)
            self.assertEqual(tools.call_count, 1)
