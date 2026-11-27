"""The player's voice chat build option: off by default, how it is chosen, remembered and passed on."""
import io
import json
import os
from contextlib import ExitStack, redirect_stdout
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from mag_installer import build_jobs, managed_tools, player_build, voice_option
from mag_installer.gui import PlayerBuildController
from mag_installer.model import InstallerError

OPUS = '/managed/prefix-opus'
LINUX_TOOLS = {'cc': '/usr/bin/clang', 'cxx': '/usr/bin/clang++'}


class ChoiceTests(unittest.TestCase):
    def test_unset_is_off_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'
            for environ in ({}, {'MAG_VOICE': ''}, {'MAG_VOICE': '  '}):
                self.assertEqual(voice_option.resolve(cache, environ), (False, 'default'))
            self.assertEqual(voice_option.resolve(cache, {}, False), (False, 'chosen'))
            self.assertFalse((cache / 'preferences.json').exists())
            self.assertIsNone(voice_option.describe(False, 'default'))

    def test_environment_turns_it_on_and_it_is_remembered(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'
            self.assertEqual(voice_option.resolve(cache, {'MAG_VOICE': '1'}), (True, 'environment'))
            self.assertEqual(voice_option.resolve(cache, {}), (True, 'remembered'))
            self.assertIn('Building with voice chat', voice_option.describe(True, 'remembered'))
            self.assertEqual(voice_option.resolve(cache, {'MAG_VOICE': '0'}), (False, 'environment'))
            self.assertEqual(voice_option.resolve(cache, {}), (False, 'default'))
            self.assertNotIn('voice', json.loads((cache / 'preferences.json').read_text()))

    def test_a_choice_wins_over_the_environment_and_keeps_the_job_count(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'
            build_jobs.remember(cache, 6)
            self.assertEqual(voice_option.resolve(cache, {'MAG_VOICE': '0'}, True), (True, 'chosen'))
            self.assertEqual(json.loads((cache / 'preferences.json').read_text()),
                             {'schema_version': build_jobs.SCHEMA, 'jobs': 6, 'voice': True})
            build_jobs.remember(cache, 8)   # the job count keeps the voice choice too
            self.assertTrue(voice_option.remembered(cache))
            self.assertEqual(voice_option.resolve(cache, {'MAG_VOICE': '1'}, False), (False, 'chosen'))
            self.assertEqual(build_jobs.remembered(cache), 8)
            self.assertFalse(voice_option.remembered(cache))

    def test_invalid_values_fail_clearly_and_write_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'
            for value in ('yes', 'on', 'ON', 'true', '2'):
                with self.subTest(value=value), self.assertRaisesRegex(InstallerError, 'MAG_VOICE must be 1'):
                    voice_option.resolve(cache, {'MAG_VOICE': value})
            with self.assertRaises(InstallerError):
                voice_option.resolve(cache, {}, 'on')
            self.assertFalse((cache / 'preferences.json').exists())


class OpusTests(unittest.TestCase):
    def test_opus_is_kept_apart_from_the_dependency_prefixes(self):
        # A build without voice downloads and builds exactly what it did before.
        for lock in managed_tools.LOCKS.values():
            self.assertNotIn('source-opus', lock)
            self.assertNotIn(managed_tools.OPUS_LOCK['sha256'], [spec['sha256'] for spec in lock.values()])
        self.assertTrue(managed_tools.OPUS_LOCK['url'].startswith('https://github.com/xiph/opus/releases/download/'))

    def test_plan_builds_a_static_library_on_every_host(self):
        paths = {'cmake': '/t/cmake', 'ninja': '/t/ninja', 'source-opus': '/t/opus-1.5.2'}
        for host in managed_tools.SUPPORTED_HOSTS:
            with self.subTest(host=host):
                stages = managed_tools.opus_plan(paths, Path('/m/prefix-opus'), host)
                self.assertEqual([s['id'] for s in stages], ['configure-opus', 'build-opus', 'install-opus'])
                configure = stages[0]['argv']
                for option in ('-DBUILD_SHARED_LIBS=OFF', '-DOPUS_BUILD_SHARED_LIBRARY=OFF', '-DOPUS_BUILD_PROGRAMS=OFF',
                               '-DOPUS_BUILD_TESTING=OFF', '-DBUILD_TESTING=OFF', '-DCMAKE_INSTALL_PREFIX=/m/prefix-opus'):
                    self.assertIn(option, configure)
                windows = host == 'windows-x86_64'
                # Opus sets the MSVC runtime itself; the engine's static CRT needs OPUS_STATIC_RUNTIME.
                self.assertEqual('-DOPUS_STATIC_RUNTIME=ON' in configure, windows)
                # MSVC's cl on Windows: Opus gives clang-cl's SIMD sources no target flags.
                self.assertEqual('-DCMAKE_C_COMPILER=cl' in configure, windows)
                self.assertFalse(any('clang' in a for a in configure))
                self.assertEqual('-DCMAKE_POSITION_INDEPENDENT_CODE=ON' in configure, not windows)
                self.assertEqual(stages[1]['argv'][-2:], ['--parallel', '2'])

    def test_windows_and_linux_need_their_toolchain(self):
        with self.assertRaisesRegex(InstallerError, 'toolchain'):
            managed_tools.opus_prefix({'root': '/m'}, host='linux-x86_64')


def _inputs(root):
    for name in ('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN', 'PS3_GAME/USRDIR/EBOOT.elf',
                 'PS3_GAME/USRDIR/bin/mage_g.self', 'PS3_GAME/USRDIR/bin/mage_g.elf', 'firmware/libsre.prx'):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'synthetic input')


class PlayerBuildTests(unittest.TestCase):
    """The choice reaches build-settings.json, the journal and the log, and only when it is on."""
    def build(self, stack, host='macos-arm64', environ=None):
        settings = []
        stack.enter_context(patch.dict('os.environ', {'MAG_BUILD_CACHE': '0', 'MAG_GAME_CONTROLS': '0', **(environ or {})}))
        for name in ('MAG_VOICE', 'MAG_X86_LEVEL'):
            if name not in (environ or {}):
                os.environ.pop(name, None)
        stack.enter_context(patch.object(player_build, 'readiness', return_value={'ready': True}))
        stack.enter_context(patch.object(player_build, '_manifest', return_value={'host': host, 'kit': {}}))
        stack.enter_context(patch.object(player_build, '_load_kit', return_value={'source_tree': 'source', 'sdks': {}}))
        stack.enter_context(patch('mag_installer.native_build._verify_identities'))
        stack.enter_context(patch('mag_installer.managed_tools.ensure_apple_sdk'))
        stack.enter_context(patch('mag_installer.managed_tools.ensure_linux_build_tools', return_value=LINUX_TOOLS))
        tools = stack.enter_context(patch('mag_installer.managed_tools.bootstrap', return_value={
            'root': 'managed', 'python': 'python', 'cmake': 'cmake', 'ninja': 'ninja', 'git': 'git', 'ffmpeg': {}}))
        opus = stack.enter_context(patch('mag_installer.managed_tools.opus_prefix', return_value=OPUS))
        def native(config, work, **kwargs):
            settings.append(json.loads(Path(config).read_text()))
            (work / 'workspace').mkdir(parents=True)
        stack.enter_context(patch('mag_installer.native_build.install', side_effect=native))
        stack.enter_context(patch('mag_installer.native_package.package', side_effect=lambda config, work, target, **kwargs: target))
        return settings, tools, opus

    def test_unset_builds_exactly_as_before(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            settings, _, opus = self.build(stack)
            events = []
            player_build.install(root, source, root / 'MAG', cache, events=events.append)
            self.assertNotIn('cmake_args', settings[0])
            self.assertNotIn('opus', settings[0])
            opus.assert_not_called()
            self.assertNotIn('voice', json.loads((root / 'MAG-build/build.json').read_text()))
            self.assertFalse(any(e.get('event') == 'build-voice' for e in events))
            self.assertFalse((cache / 'preferences.json').exists())

    def test_environment_builds_with_voice_and_later_builds_keep_it(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            settings, _, opus = self.build(stack, environ={'MAG_VOICE': '1'})
            events = []
            player_build.install(root, source, root / 'MAG', cache, events=events.append)
            self.assertEqual(settings[0]['cmake_args'], ['-DMAG_VOICE=ON'])
            self.assertEqual(settings[0]['opus'], {'root': OPUS})
            self.assertEqual(opus.call_args.args[0]['root'], 'managed')
            self.assertIsNone(opus.call_args.kwargs['toolchain'])   # macOS: the Apple Command Line Tools
            self.assertTrue(json.loads((root / 'MAG-build/build.json').read_text())['voice'])
            notice = next(e for e in events if e.get('event') == 'build-voice')
            self.assertEqual((notice['voice'], notice['source']), (True, 'environment'))
            # An update: a new build without MAG_VOICE keeps voice chat.
            os.environ.pop('MAG_VOICE')
            player_build.install(root, source, root / 'MAG2', cache)
            self.assertEqual(settings[1]['cmake_args'], ['-DMAG_VOICE=ON'])
            # Choosing off forgets it.
            player_build.install(root, source, root / 'MAG3', cache, voice=False)
            self.assertNotIn('cmake_args', settings[2])
            player_build.install(root, source, root / 'MAG4', cache)
            self.assertNotIn('cmake_args', settings[3])

    def test_linux_passes_its_compilers_and_keeps_the_x86_level(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            settings, _, opus = self.build(stack, host='linux-x86_64', environ={'MAG_X86_LEVEL': 'baseline'})
            player_build.install(root, source, root / 'MAG', cache, voice=True)
            self.assertEqual(settings[0]['cmake_args'], ['-DMAG_X86_LEVEL=baseline', '-DMAG_VOICE=ON'])
            self.assertEqual(opus.call_args.kwargs['toolchain'], LINUX_TOOLS)
            self.assertEqual(opus.call_args.kwargs['host'], 'linux-x86_64')

    def test_invalid_environment_stops_before_any_tool_setup(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            _, tools, opus = self.build(stack, environ={'MAG_VOICE': 'yes'})
            with self.assertRaisesRegex(InstallerError, 'MAG_VOICE must be 1'):
                player_build.install(root, source, root / 'MAG', cache)
            tools.assert_not_called(); opus.assert_not_called()
            self.assertFalse((root / 'MAG-build').exists())

    def test_earlier_layout_is_not_moved_without_the_voice_rebuild(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve(); source = root / 'inputs'; _inputs(source); cache = root / 'cache'
            self.build(stack)
            output = root / 'MAG'; output.mkdir()
            (output / 'build.json').write_text(json.dumps({'schema_version': 2, 'layout': 1, 'state': 'complete'}))
            with patch.object(player_build, '_migrate') as migrate:
                with self.assertRaisesRegex(InstallerError, 'without voice chat'):
                    player_build.install(root, source, output, cache, voice=True)
                migrate.assert_not_called()
                player_build.install(root, source, output, cache, voice=False)
                migrate.assert_called_once()


class FrontEndTests(unittest.TestCase):
    def test_cli_passes_the_choice_only_when_given(self):
        from mag_installer.__main__ import main
        with patch('mag_installer.player_build.install') as build, redirect_stdout(io.StringIO()):
            main(['build', '--source', '/input', '--parent', '/destination'])
            self.assertNotIn('voice', build.call_args.kwargs)
            main(['build', '--source', '/input', '--parent', '/destination', '--voice'])
            self.assertIs(build.call_args.kwargs['voice'], True)
            main(['build', '--source', '/input', '--parent', '/destination', '--no-voice'])
            self.assertIs(build.call_args.kwargs['voice'], False)

    def test_wizard_box_shows_the_environment_or_remembered_choice_and_passes_changes(self):
        calls = []
        def build(root, source, output, cache, token=None, events=None, **kwargs):
            calls.append(kwargs); return output
        c = PlayerBuildController(Path('/kit'), readiness=lambda root: {'ready': True, 'reason': ''}, installer=build)
        with tempfile.TemporaryDirectory() as temp:
            cache = Path(temp) / 'cache'
            self.assertFalse(c.voice_choice(cache, {}))
            self.assertTrue(c.voice_choice(cache, {'MAG_VOICE': '1'}))
            self.assertFalse(c.voice_choice(cache, {'MAG_VOICE': 'bad'}))
            voice_option.remember(cache, True)
            self.assertTrue(c.voice_choice(cache, {}))
            self.assertFalse(c.voice_choice(cache, {'MAG_VOICE': '0'}))
            for voice in (None, True):
                c.start(Path(temp) / 'disc', Path(temp), cache, voice=voice)
                c.worker.join(5); c.poll()
            self.assertEqual(calls, [{}, {'voice': True}])


if __name__ == '__main__': unittest.main()
