import json
import os
from pathlib import Path
import tempfile
import unittest
from mag_installer.model import InstallerError
from mag_installer.native_recipe import ROLES, build_plan, framework_pins, load_config, splice_pins, x86_level_override

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
                           {**PINS, 'runtime': {**PINS['runtime'], 'path': '../outside'}},
                           {**PINS, 'lifter-ppu': {**PINS['lifter-ppu'], 'splice_after': []}},
                           {**PINS, 'lifter-ppu': {**PINS['lifter-ppu'], 'splice_after': [{'tag': 'other/x', 'commit': 'a' * 40}]}},
                           {**PINS, 'lifter-ppu': {**PINS['lifter-ppu'], 'splice_after': {'tag': 'mag-recomp/x', 'commit': 'short'}}}):
                (workspace / 'profiles/framework.json').write_text(json.dumps({'ps3recomp': broken}))
                with self.subTest(broken=broken), self.assertRaises(InstallerError): framework_pins(workspace)

    def test_framework_pins_accept_one_or_several_splice_commits(self):
        one = {'tag': 'mag-recomp/lifter-fix-1', 'commit': 'e' * 40}
        two = {'tag': 'mag-recomp/lifter-fix-2', 'commit': 'f' * 40}
        with tempfile.TemporaryDirectory() as tmp:
            workspace = self.workspace(Path(tmp))
            for extra in (one, [one, two]):
                pins = {**PINS, 'lifter-ppu': {**PINS['lifter-ppu'], 'splice_after': extra}}
                (workspace / 'profiles/framework.json').write_text(json.dumps({'ps3recomp': pins}))
                with self.subTest(extra=extra):
                    self.assertEqual(framework_pins(workspace)['lifter-ppu']['splice_after'], extra)
                    self.assertEqual(splice_pins(pins['lifter-ppu']), extra if isinstance(extra, list) else [extra])
            self.assertEqual(splice_pins(PINS['runtime']), [])

    def test_plan_follows_building_md_with_sdks_at_their_pinned_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); config = self.load(root, self.config(root)); workspace = self.workspace(root)
            plan = build_plan(config, workspace); commands = {p['id']: p['argv'] for p in plan}
            self.assertEqual(list(commands), ['prepare-inputs', 'inventory', 'lift-launcher', 'lift-engine', 'splice-engine-dispatcher', 'splice-engine-ctr64', 'worker-spu',
                                              'engine-spu', 'streamed-jobs', 'firmware', 'spu-programs', 'configure-launcher',
                                              'build-launcher', 'configure-engine', 'build-engine'])
            self.assertIn(str(workspace / 'local/ps3recomp-lifter-spu'), commands['inventory'])
            self.assertIn(str(workspace / 'local/ps3recomp-lifter-ppu'), commands['lift-engine'])
            self.assertIn(PINS['spu-programs']['commit'], commands['spu-programs'])
            # The splices chain: lift -> dispatcher fix -> CTR fix -> engine build.
            generated = workspace / 'generated/retail-v1.0'
            self.assertIn('-DRECOMP_DIR=' + str(generated / 'mage_g-ctr64'), commands['configure-engine'])
            chain = [('splice-engine-dispatcher', 'mage_g', 'mage_g-dispatcher', 'retail-v1.0-ppu-jumptable-splice.json'),
                     ('splice-engine-ctr64', 'mage_g-dispatcher', 'mage_g-ctr64', 'retail-v1.0-ppu-ctr64-splice.json')]
            for step, base, output, profile in chain:
                splice = commands[step]
                self.assertEqual(splice[splice.index('--base') + 1], str(generated / base))
                self.assertEqual(splice[splice.index('--output') + 1], str(generated / output))
                self.assertEqual(splice[splice.index('--profile') + 1], str(workspace / 'profiles' / profile))
                self.assertEqual(splice[splice.index('--before-sdk') + 1], str(workspace / 'local/ps3recomp-lifter-ppu'))
                self.assertEqual(splice[splice.index('--after-sdk') + 1], str(workspace / 'local/ps3recomp-lifter-ppu'))
            self.assertNotEqual(commands['splice-engine-dispatcher'][commands['splice-engine-dispatcher'].index('--work') + 1],
                                commands['splice-engine-ctr64'][commands['splice-engine-ctr64'].index('--work') + 1])
            self.assertIn('-DPS3RECOMP_DIR=' + str(workspace / 'local/ps3recomp-runtime'), commands['configure-engine'])
            streamed = commands['streamed-jobs']
            self.assertIn('wws_job_lift.py', ' '.join(streamed))
            self.assertIn(str(workspace/'local/ps3recomp-runtime'), streamed)
            self.assertIn(PINS['runtime']['commit'], streamed)
            self.assertIn('--merge-chunks', streamed)
            self.assertIn('--require-complete', streamed)
            self.assertIn('--static-indirect', streamed)
            self.assertIn('--engine-profile', streamed)
            self.assertIn('-DSPU_DIR='+str(workspace/'generated/retail-v1.0/spu-engine-retained'), commands['configure-engine'])
            self.assertIn('-DSPU_DIR='+str(workspace/'generated/retail-v1.0/spu-eboot'), commands['configure-launcher'])
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
            plan = {stage['id']: stage['argv'] for stage in build_plan(config, self.workspace(root))}
            configure = plan['configure-engine']
            self.assertIn('-DCMAKE_C_COMPILER=' + Path(config['msvc']['clang_cl']).as_posix(), configure)
            self.assertIn('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', configure)
            self.assertFalse(any('FFMPEG' in argument for argument in configure))
            self.assertTrue(all('\\' not in argument for argument in configure if argument.startswith('-D')))
            self.assertFalse(any('PS3RECOMP_RSX_VULKAN' in argument for argument in configure))
            ffmpeg_root = root / 'ffmpeg'; (ffmpeg_root / 'include').mkdir(parents=True); (ffmpeg_root / 'lib').mkdir()
            for name in ('avcodec', 'avformat', 'avutil', 'swscale'): (ffmpeg_root / 'lib' / (name + '.lib')).write_text('fixture')
            movies = self.load(root, dict(data, ffmpeg={'include': 'ffmpeg/include', **{n: 'ffmpeg/lib/' + n + '.lib' for n in ('avcodec', 'avformat', 'avutil', 'swscale')}}))
            configure = {stage['id']: stage['argv'] for stage in build_plan(movies, root / 'workspace')}['configure-engine']
            for flag in ('-DPS3RECOMP_SAIL_FFMPEG=ON', '-DPS3RECOMP_FFMPEG_RUNTIME=ON',
                         '-DPS3RECOMP_FFMPEG_INCLUDE_DIR=' + (ffmpeg_root / 'include').resolve().as_posix(),
                         '-DPS3RECOMP_FFMPEG_AVCODEC_LIBRARY=' + (ffmpeg_root / 'lib/avcodec.lib').resolve().as_posix()):
                self.assertIn(flag, configure)
            self.assertTrue(all('\\' not in argument for argument in configure if argument.startswith('-D')))
            prefix = root / 'prefix'; prefix.mkdir()
            vulkan = self.load(root, dict(data, managed_prefix='prefix'))
            configure = {stage['id']: stage['argv'] for stage in build_plan(vulkan, root / 'workspace')}['configure-engine']
            self.assertIn('-DPS3RECOMP_RSX_VULKAN=ON', configure)
            self.assertIn('-DPS3RECOMP_VULKAN_INCLUDE_DIR=' + (prefix.resolve() / 'include').as_posix(), configure)
            self.assertIn('-DCMAKE_PREFIX_PATH=' + prefix.resolve().as_posix(), configure)
            self.assertTrue(all('\\' not in argument for argument in configure if argument.startswith('-D')))

    def test_game_controls_are_opt_in_and_prepare_their_own_engine_lift(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = self.config(root); workspace = self.workspace(root)
            default = {stage['id']: stage['argv'] for stage in build_plan(self.load(root, data), workspace)}
            self.assertNotIn('prepare-controls-hooks', default)
            self.assertFalse(any('GAME_CONTROLS' in a or 'MAG_INPUT' in a for argv in default.values() for a in argv))
            for name in ('cargo', 'rustc'):
                (root / name).write_text('fixture'); (root / name).chmod(0o755)
            with self.assertRaisesRegex(InstallerError, 'game_controls'):
                self.load(root, dict(data, game_controls={'cargo': 'cargo'}))
            config = self.load(root, dict(data, game_controls={'cargo': 'cargo', 'rustc': 'rustc'}))
            plan = build_plan(config, workspace); commands = {p['id']: p['argv'] for p in plan}
            ids = [p['id'] for p in plan]
            self.assertEqual(ids.index('prepare-controls-hooks'), ids.index('splice-engine-ctr64') + 1)
            prepare = commands['prepare-controls-hooks']
            generated = workspace / 'generated/retail-v1.0'
            self.assertEqual(prepare[prepare.index('--base') + 1], str(generated / 'mage_g-ctr64'))
            self.assertEqual(prepare[prepare.index('--output') + 1], str(generated / 'mage_g-controls'))
            self.assertEqual(prepare[-2:], ['--stage', 'controls'])
            engine = commands['configure-engine']
            self.assertIn('-DRECOMP_DIR=' + str(generated / 'mage_g-controls'), engine)
            for flag in ('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', '-DMAG_INPUT_CARGO=' + str((root / 'cargo').resolve()),
                         '-DMAG_INPUT_RUSTC=' + str((root / 'rustc').resolve())):
                self.assertIn(flag, engine)
            self.assertFalse(any('GAME_CONTROLS' in a or 'MAG_INPUT' in a for a in commands['configure-launcher']))


    def test_x86_level_passes_nothing_by_default_and_allows_an_override(self):
        for host in ('windows-x86_64', 'linux-x86_64', 'macos-arm64'):
            self.assertEqual(x86_level_override({}, host), [])
            self.assertEqual(x86_level_override({'MAG_X86_LEVEL': ''}, host), [])
        for host in ('windows-x86_64', 'linux-x86_64'):
            for level in ('auto', 'v3', 'baseline'):
                self.assertEqual(x86_level_override({'MAG_X86_LEVEL': level}, host), ['-DMAG_X86_LEVEL=' + level])
            with self.assertRaisesRegex(InstallerError, 'auto, v3 or baseline'):
                x86_level_override({'MAG_X86_LEVEL': 'v4'}, host)
        with self.assertRaisesRegex(InstallerError, 'only to Windows and Linux'):
            x86_level_override({'MAG_X86_LEVEL': 'baseline'}, 'macos-arm64')

    def test_x86_level_override_reaches_both_configures(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = self.config(root)
            plan = build_plan(self.load(root, data), self.workspace(root))
            self.assertFalse(any('MAG_X86_LEVEL' in argument for stage in plan for argument in stage['argv']))
            config = self.load(root, dict(data, cmake_args=['-DMAG_X86_LEVEL=baseline']))
            plan = {stage['id']: stage['argv'] for stage in build_plan(config, root / 'workspace')}
            for name in ('configure-launcher', 'configure-engine'):
                self.assertIn('-DMAG_X86_LEVEL=baseline', plan[name])
            for bad in ('-DMAG_X86_LEVEL=native', '-DMAG_X86_LEVEL=v3 -DCMAKE_C_FLAGS=x'):
                with self.subTest(bad=bad), self.assertRaises(InstallerError):
                    self.load(root, dict(data, cmake_args=[bad]))


if __name__ == '__main__': unittest.main()
