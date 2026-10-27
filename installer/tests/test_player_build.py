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
