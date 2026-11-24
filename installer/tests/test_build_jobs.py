"""The player-chosen compile job count: default, validation, persistence and where it reaches."""
import io
import json
import os
from contextlib import ExitStack, redirect_stdout
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from mag_installer import build_jobs, player_build
from mag_installer.model import InstallerError

GIB = 1024 ** 3


class DefaultTests(unittest.TestCase):
    def test_default_follows_memory_capped_by_cpus_with_the_old_count_as_floor(self):
        for cpus, memory, expected in (
                (32, 64 * GIB, 15),    # 16-core desktop, 64 GiB: (64 - 4) / 4
                (32, 128 * GIB, 31),
                (32, 192 * GIB, 32),   # memory allows more than the CPUs
                (14, 48 * GIB, 11),
                (10, 16 * GIB, 3),
                (8, 8 * GIB, 2),       # never below the 2 earlier installers used
                (4, 4 * GIB, 2),
                (1, 64 * GIB, 1),      # one CPU
                (256, 2048 * GIB, 64)):
            with self.subTest(cpus=cpus, memory=memory):
                self.assertEqual(build_jobs.recommended(cpus, memory), expected)

    def test_unknown_cpus_or_memory_fall_back_to_two(self):
        with patch.object(build_jobs, 'logical_cpus', return_value=None), patch.object(build_jobs, 'total_memory', return_value=None):
            self.assertEqual(build_jobs.recommended(), 2)
        with patch.object(build_jobs, 'logical_cpus', return_value=16), patch.object(build_jobs, 'total_memory', return_value=None):
            self.assertEqual(build_jobs.recommended(), 2)

    def test_this_computer_reports_cpus_and_memory(self):
        cpus, memory = build_jobs.logical_cpus(), build_jobs.total_memory()
        self.assertTrue(cpus is None or cpus >= 1)
        self.assertTrue(memory is None or memory > 0)
        self.assertTrue(build_jobs.MIN_JOBS <= build_jobs.recommended() <= build_jobs.MAX_JOBS)


class ValidationTests(unittest.TestCase):
    def test_whole_numbers_from_1_to_64(self):
        for value, expected in (('1', 1), (64, 64), (' 8 ', 8), ('016', 16)):
            with self.subTest(value=value):
                self.assertEqual(build_jobs.parse(value), expected)
        for value in ('0', '65', '-1', '', 'eight', '2.5', '1e2', True, None, '٣', 0, 100):
            with self.subTest(value=value), self.assertRaisesRegex(InstallerError, 'whole number from 1 to 64'):
                build_jobs.parse(value)


class PersistenceTests(unittest.TestCase):
    def test_choice_is_remembered_overridden_and_forgotten(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'   # created on first use
            default = build_jobs.recommended()
            self.assertEqual(build_jobs.resolve(cache), (default, 'default'))
            self.assertFalse((cache / 'preferences.json').exists())
            self.assertEqual(build_jobs.resolve(cache, '6'), (6, 'chosen'))
            self.assertEqual(json.loads((cache / 'preferences.json').read_text()), {'schema_version': 1, 'jobs': 6})
            self.assertEqual(build_jobs.resolve(cache), (6, 'remembered'))
            self.assertEqual(build_jobs.resolve(cache, 3), (3, 'chosen'))   # an explicit value wins and is kept
            self.assertEqual(build_jobs.resolve(cache), (3, 'remembered'))
            with self.assertRaises(InstallerError):
                build_jobs.resolve(cache, '0')
            self.assertEqual(build_jobs.remembered(cache), 3)               # an invalid value changes nothing
            self.assertEqual(build_jobs.resolve(cache, 'AUTO'), (default, 'default'))
            self.assertIsNone(build_jobs.remembered(cache))
            self.assertEqual(build_jobs.resolve(cache), (default, 'default'))

    def test_unreadable_or_foreign_preferences_use_the_default_and_keep_other_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp); path = cache / 'preferences.json'
            for text in ('not json', '[]', '{"schema_version": 99, "jobs": 8}', '{"schema_version": 1, "jobs": 500}',
                         '{"schema_version": 1, "jobs": "8"}'):
                with self.subTest(text=text):
                    path.write_text(text)
                    self.assertIsNone(build_jobs.remembered(cache))
            path.write_text('{"schema_version": 1, "other": "kept"}')
            build_jobs.remember(cache, 12)
            self.assertEqual(json.loads(path.read_text()), {'schema_version': 1, 'other': 'kept', 'jobs': 12})

    @unittest.skipIf(os.name == 'nt', 'symlinks need privileges on Windows')
    def test_symlinked_preferences_are_neither_read_nor_written(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'; cache.mkdir()
            target = Path(temp) / 'elsewhere.json'; target.write_text('{"schema_version": 1, "jobs": 9}')
            (cache / 'preferences.json').symlink_to(target)
            self.assertIsNone(build_jobs.remembered(cache))
            with self.assertRaises(InstallerError):
                build_jobs.remember(cache, 4)
            self.assertEqual(json.loads(target.read_text())['jobs'], 9)


def _inputs(root):
    for name in ('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN', 'PS3_GAME/USRDIR/EBOOT.elf',
                 'PS3_GAME/USRDIR/bin/mage_g.self', 'PS3_GAME/USRDIR/bin/mage_g.elf', 'firmware/libsre.prx'):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'synthetic input')


class PlayerBuildTests(unittest.TestCase):
    """The job count reaches build-settings.json, is checked before any work and may change on a retry."""
    def build(self, stack, root, package_error=None):
        settings, events = [], []
        stack.enter_context(patch.dict('os.environ', {'MAG_BUILD_CACHE': '0', 'MAG_GAME_CONTROLS': '0'}))
        stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
        stack.enter_context(patch.object(player_build, '_manifest', return_value={'host': 'macos-arm64', 'kit': {}}))
        stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree': 'source', 'sdks': {}}))
        verify = stack.enter_context(patch('mag_installer.native_build._verify_identities'))
        stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
        tools = stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={
            'root': 'managed', 'python': 'python', 'cmake': 'cmake', 'ninja': 'ninja', 'git': 'git', 'ffmpeg': {}}))
        def native(config, work, **kwargs):
            settings.append(json.loads(Path(config).read_text())['jobs'])
            (work / 'workspace').mkdir(parents=True)
        stack.enter_context(patch('mag_installer.native_build.install', side_effect=native))
        package = stack.enter_context(patch('mag_installer.native_package.package', side_effect=package_error))
        return settings, events, verify, tools, package

    def test_invalid_count_stops_before_any_check_download_or_folder(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            _, _, verify, tools, _ = self.build(stack, root)
            for value in ('0', '65', 'lots'):
                with self.subTest(value=value), self.assertRaisesRegex(InstallerError, 'Compile jobs must be'):
                    player_build.install(root, source, root / 'MAG', cache, jobs=value)
            verify.assert_not_called(); tools.assert_not_called()
            self.assertFalse((root / 'MAG-build').exists())
            self.assertFalse((cache / 'preferences.json').exists())

    def test_chosen_count_reaches_the_build_settings_and_is_used_again(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            settings, events, _, _, package = self.build(stack, root)
            package.side_effect = lambda config, work, target, **kwargs: target
            player_build.install(root, source, root / 'MAG', cache, jobs='12', events=events.append)
            self.assertEqual(settings, [12])
            notice = next(e for e in events if e.get('event') == 'build-jobs')
            self.assertEqual((notice['jobs'], notice['source']), (12, 'chosen'))
            self.assertIn('Compiling with 12 jobs', notice['message'])
            # A later build without a choice uses the remembered one.
            player_build.install(root, source, root / 'MAG2', cache, events=events.append)
            self.assertEqual(settings, [12, 12])
            self.assertEqual([e['source'] for e in events if e.get('event') == 'build-jobs'], ['chosen', 'remembered'])
            player_build.install(root, source, root / 'MAG3', cache, jobs='auto')
            self.assertEqual(settings[-1], build_jobs.recommended())

    def test_retry_may_use_another_count(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            settings, _, _, _, package = self.build(stack, root, package_error=InstallerError('synthetic failure'))
            output = root / 'MAG'
            with self.assertRaisesRegex(InstallerError, 'synthetic failure'):
                player_build.install(root, source, output, cache, jobs=2)
            package.side_effect = lambda config, work, target, **kwargs: target
            self.assertEqual(player_build.install(root, source, output, cache, retry=True, jobs=7), output)
            self.assertEqual(settings, [2, 7])
            build = root / 'MAG-build'
            self.assertEqual(json.loads((build / 'build-settings.json').read_text())['jobs'], 7)
            self.assertEqual(json.loads((build / 'logs/attempts/1/build-settings.json').read_text())['jobs'], 2)
            self.assertEqual(json.loads((build / 'build.json').read_text())['state'], 'complete')

    def test_cli_flag_reaches_the_build_with_and_without_retry(self):
        from mag_installer.__main__ import main
        with patch('mag_installer.player_build.install') as build, redirect_stdout(io.StringIO()):
            self.assertEqual(main(['build', '--source', '/input', '--parent', '/destination']), 0)
            self.assertIsNone(build.call_args.kwargs['jobs'])
            self.assertEqual(main(['build', '--source', '/input', '--parent', '/destination', '--jobs', '8']), 0)
            self.assertEqual(build.call_args.kwargs['jobs'], '8')
            self.assertEqual(main(['build', '--source', '/input', '--output', '/destination/MAG', '--retry', '--jobs', 'auto']), 0)
            self.assertEqual((build.call_args.kwargs['jobs'], build.call_args.kwargs['retry']), ('auto', True))

    def test_cli_reports_an_invalid_count_as_an_error(self):
        from mag_installer.__main__ import main
        with tempfile.TemporaryDirectory() as temp:
            errors = io.StringIO()
            with patch('sys.stderr', errors), redirect_stdout(io.StringIO()):
                code = main(['build', '--json', '--source', temp, '--output', str(Path(temp) / 'MAG'),
                             '--cache', str(Path(temp) / 'cache'), '--jobs', '0'])
            self.assertEqual(code, 2)
            self.assertIn('Compile jobs must be a whole number from 1 to 64', json.loads(errors.getvalue())['message'])
            self.assertFalse((Path(temp) / 'cache/preferences.json').exists())


class ReachTests(unittest.TestCase):
    """The count reaches cmake --parallel and CMAKE_BUILD_PARALLEL_LEVEL, and nothing a cache keys on."""
    def plan(self, root, jobs):
        from test_native_recipe import NativeRecipeTests
        fixture = NativeRecipeTests()
        folder = root / str(jobs); folder.mkdir()
        config = fixture.load(folder, dict(fixture.config(folder), jobs=jobs))
        workspace = fixture.workspace(folder)
        from mag_installer.native_recipe import build_plan
        replace = lambda text: str(text).replace(str(folder), '<root>')
        return config, [{'id': s['id'], 'argv': [replace(a) for a in s['argv']]} for s in build_plan(config, workspace)]

    def test_count_reaches_parallel_and_environment_only(self):
        from mag_installer import native_build
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            config_a, plan_a = self.plan(root, 2)
            config_b, plan_b = self.plan(root, 16)
            for stage in (s for s in plan_b if s['id'] in ('build-launcher', 'build-engine')):
                self.assertEqual(stage['argv'][stage['argv'].index('--parallel') + 1], '16')
            self.assertEqual(native_build._environment(config_b, root)['CMAKE_BUILD_PARALLEL_LEVEL'], '16')
            # Generation (what the lift cache keys on) and configure (what ccache's per-unit compile
            # commands come from) are identical, so another count reuses earlier work.
            def without_parallel(plan):
                return [s if '--parallel' not in s['argv'] else {**s, 'argv': s['argv'][:s['argv'].index('--parallel')]}
                        for s in plan]
            self.assertEqual(without_parallel(plan_a), without_parallel(plan_b))
            first_configure = next(i for i, s in enumerate(plan_a) if s['id'].startswith('configure-'))
            self.assertEqual(plan_a[:first_configure], plan_b[:first_configure])


class ControllerTests(unittest.TestCase):
    def controller(self, calls):
        from mag_installer.gui import PlayerBuildController
        def build(root, source, output, cache, **kwargs):
            calls.append(kwargs.get('jobs', 'unset')); return output
        return PlayerBuildController(Path('/kit'), readiness=lambda root: {'ready': True}, installer=build)

    def test_field_shows_remembered_choice_else_default_and_hint(self):
        calls = []
        c = self.controller(calls)
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp)
            with patch.object(build_jobs, 'logical_cpus', return_value=32), patch.object(build_jobs, 'total_memory', return_value=64 * GIB):
                choice = c.jobs_choice(cache)
                self.assertEqual((choice['value'], choice['default'], choice['remembered']), (15, 15, None))
                self.assertEqual((choice['minimum'], choice['maximum']), (1, 64))
                hint = c.jobs_hint(choice)
                self.assertIn('Default for this computer: 15 (32 logical CPUs, 64 GiB memory)', hint)
                self.assertIn('1–64', hint)
                build_jobs.remember(cache, 6)
                self.assertEqual(c.jobs_choice(cache)['value'], 6)
                # The field at the default follows the computer; any other value is the player's choice.
                self.assertEqual(c.jobs_request('15', 15), 'auto')
                self.assertEqual(c.jobs_request(' 6', 15), 6)
                with self.assertRaisesRegex(InstallerError, 'whole number'):
                    c.jobs_request('0', 15)

    def test_start_checks_the_count_before_starting_and_passes_it_on(self):
        calls = []
        c = self.controller(calls)
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            with self.assertRaisesRegex(InstallerError, 'whole number'):
                c.start(parent / 'disc', parent, parent / 'cache', jobs='99')
            self.assertIsNone(c.worker)
            for jobs, expected in ((None, 'unset'), ('auto', 'auto'), ('5', 5), (9, 9)):
                c.start(parent / 'disc', parent, parent / 'cache', jobs=jobs)
                c.worker.join(5); c.poll()
                self.assertEqual(c.state, 'complete')
            self.assertEqual(calls, ['unset', 'auto', 5, 9])


if __name__ == '__main__':
    unittest.main()
