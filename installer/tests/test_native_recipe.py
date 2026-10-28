import json
import os
from pathlib import Path
import tempfile
import unittest
from mag_installer.model import InstallerError
from mag_installer.native_recipe import ROLES, build_plan, framework_pins, load_config

PINS = {'runtime': {'tag': 'mag-recomp/runtime-fixture', 'commit': 'a' * 40, 'path': 'local/ps3recomp-runtime'},
        'lifter-spu': {'tag': 'mag-recomp/lifter-spu-fixture', 'commit': 'b' * 40, 'path': 'local/ps3recomp-lifter-spu'},
        'lifter-ppu': {'tag': 'mag-recomp/lifter-ppu-fixture', 'commit': 'c' * 40, 'path': 'local/ps3recomp-lifter-ppu'},
        'spu-programs': {'tag': 'mag-recomp/spu-programs-fixture', 'commit': 'd' * 40, 'path': 'local/ps3recomp-spu-programs'}}


class NativeRecipeTests(unittest.TestCase):
    def config(self, root):
        for name in ['source', 'disc', 'headers', *ROLES]:
            (root / name).mkdir()
        for name in ['eboot', 'engine', 'firmware', 'python', 'cmake', 'ninja', 'git', 'avcodec', 'avformat', 'avutil', 'swscale']:
            (root / name).write_text('fixture')
            (root / name).chmod(0o755)
        return dict(schema_version=1, source_tree='source', disc_root='disc', eboot_elf='eboot',
                    engine_elf='engine', firmware='firmware', python='python', cmake='cmake', ninja='ninja', git='git',
                    sdks={role: dict(path=role, revision=PINS[role]['commit']) for role in ROLES},
                    ffmpeg=dict(include='headers', **{n: n for n in ['avcodec', 'avformat', 'avutil', 'swscale']}))

    def load(self, root, data):
        path = root / 'config.json'; path.write_text(json.dumps(data)); return load_config(path)

    def workspace(self, root):
        profiles = root / 'workspace/profiles'; profiles.mkdir(parents=True)
        (profiles / 'framework.json').write_text(json.dumps({'ps3recomp': PINS}))
        return root / 'workspace'

    def test_config_normalizes_relative_to_config_and_rejects_unknown_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = self.config(root); config = self.load(root, data)
            self.assertEqual(config['source_tree'], str((root / 'source').resolve()))
            self.assertEqual(config['sdks']['lifter-ppu']['path'], str((root / 'lifter-ppu').resolve()))
            self.assertEqual(config['jobs'], 2)
            for key, value in [('commands', []), ('jobs', 5), ('native_graphics', True), ('cmake_args', ['-DMAG_IMAGE=evil'])]:
                with self.subTest(key=key), self.assertRaises(InstallerError): self.load(root, dict(data, **{key: value}))
            data['sdks'].pop('runtime')
            with self.assertRaises(InstallerError): self.load(root, data)

    def test_missing_executable_and_missing_ffmpeg_fail_clearly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = self.config(root)
            if os.name != 'nt':  # Windows has no executable mode bit to remove
                (root / 'python').chmod(0o644)
                with self.assertRaisesRegex(InstallerError, 'python'): self.load(root, data)
                (root / 'python').chmod(0o755)
            data.pop('ffmpeg')
            with self.assertRaisesRegex(InstallerError, 'ffmpeg'): self.load(root, data)

    def test_framework_pins_require_the_complete_table_inside_the_workspace(self):
        with tempfile.TemporaryDirectory() as tmp:
            workspace = self.workspace(Path(tmp))
            self.assertEqual(framework_pins(workspace), PINS)
            for broken in ({k: v for k, v in PINS.items() if k != 'runtime'},
                           {**PINS, 'runtime': {**PINS['runtime'], 'commit': 'short'}},
                           {**PINS, 'runtime': {**PINS['runtime'], 'path': '../outside'}}):
                (workspace / 'profiles/framework.json').write_text(json.dumps({'ps3recomp': broken}))
                with self.subTest(broken=broken), self.assertRaises(InstallerError): framework_pins(workspace)

    def test_plan_follows_building_md_with_sdks_at_their_pinned_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); config = self.load(root, self.config(root)); workspace = self.workspace(root)
            plan = build_plan(config, workspace); commands = {p['id']: p['argv'] for p in plan}
            self.assertEqual(list(commands), ['prepare-inputs', 'inventory', 'lift-launcher', 'lift-engine', 'splice-engine-dispatcher', 'worker-spu',
                                              'engine-spu', 'firmware', 'spu-programs', 'configure-launcher',
                                              'build-launcher', 'configure-engine', 'build-engine'])
            self.assertIn(str(workspace / 'local/ps3recomp-lifter-spu'), commands['inventory'])
            self.assertIn(str(workspace / 'local/ps3recomp-lifter-ppu'), commands['lift-engine'])
            self.assertIn(PINS['spu-programs']['commit'], commands['spu-programs'])
            self.assertIn(str(workspace / 'generated/retail-v1.0/mage_g-dispatcher'), ' '.join(commands['configure-engine']))
            self.assertIn(str(workspace / 'local/ps3recomp-runtime'), commands['splice-engine-dispatcher'])
            self.assertIn('-DPS3RECOMP_DIR=' + str(workspace / 'local/ps3recomp-runtime'), commands['configure-engine'])
            self.assertIn('-DMAG_SPU_PROGRAMS_DIR=' + str(workspace / 'local/spu-programs'), commands['configure-engine'])
            self.assertIn('-DPS3RECOMP_SAIL_FFMPEG=ON', commands['configure-engine'])
            self.assertEqual(commands['build-engine'][-1], '2')
            self.assertEqual(commands['prepare-inputs'][commands['prepare-inputs'].index('--source-root') + 1],
                             str(workspace / 'local/retail-v1.0/PS3_GAME/USRDIR'))
            self.assertTrue(all(isinstance(arg, str) for step in plan for arg in step['argv']))

    def test_windows_config_uses_msvc_record_and_build_plan_targets_clang_cl(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = self.config(root); data.pop('ffmpeg')
            for name in ('vcvars64.bat', 'clang-cl.exe'):
                (root / name).write_text('fixture'); (root / name).chmod(0o755)
            data['msvc'] = {'vcvars': 'vcvars64.bat', 'clang_cl': 'clang-cl.exe', 'version': '17.14'}
            config = self.load(root, data)
            self.assertEqual(config['msvc']['clang_cl'], str((root / 'clang-cl.exe').resolve()))
            with self.assertRaisesRegex(InstallerError, 'FFmpeg'):
                self.load(root, dict(data, ffmpeg=self.config(Path(tempfile.mkdtemp()))['ffmpeg']))
            plan = {stage['id']: stage['argv'] for stage in build_plan(config, self.workspace(root))}
            configure = plan['configure-engine']
            self.assertIn('-DCMAKE_C_COMPILER=' + Path(config['msvc']['clang_cl']).as_posix(), configure)
            self.assertIn('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', configure)
            self.assertFalse(any('FFMPEG' in argument for argument in configure))
            self.assertTrue(all('\\' not in argument for argument in configure if argument.startswith('-D')))


if __name__ == '__main__': unittest.main()
