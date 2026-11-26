"""The installer's shader warm-up step (mag_installer/shader_warm.py): it runs only for Windows
Vulkan builds, MAG_SHADER_CACHE=0 skips it, a tool that fails or cannot be built is a warning and
never fails the installation, cancellation still cancels, and its summary reaches the log."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from mag_installer import shader_warm
from mag_installer.model import InstallerError
from mag_installer.safety import Cancellation, Cancelled

WINDOWS = {'msvc': {'clang_cl': 'C:/clang-cl.exe'}, 'managed_prefix': 'C:/prefix', 'cmake': 'C:/cmake.exe', 'jobs': 4}


class ShaderWarmTests(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / 'logs').mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def warm(self, config, run, environ=None):
        with patch.object(shader_warm, 'run_command', side_effect=run):
            return shader_warm.warm(config, self.root / 'workspace', self.root / 'logs', {'PATH': 'build'},
                                    Cancellation(), self.events.append,
                                    environ={'LOCALAPPDATA': 'C:/Users/P/AppData/Local'} if environ is None else environ)

    def test_only_windows_vulkan_builds(self):
        calls = []
        for config in ({}, {'msvc': {}}, {'host': 'linux-x86_64', 'managed_prefix': '/p'}):
            self.assertEqual(self.warm(config, lambda *a, **k: calls.append(a))['status'], 'not-applicable')
        self.assertEqual(calls, [])
        self.assertEqual(self.events, [])

    def test_switch_skips_it(self):
        calls = []
        result = self.warm(WINDOWS, lambda *a, **k: calls.append(a), environ={'MAG_SHADER_CACHE': '0'})
        self.assertEqual(result['status'], 'skipped')
        self.assertEqual(calls, [])
        self.assertIn('MAG_SHADER_CACHE=0', self.events[-1]['message'])

    def test_builds_then_runs_with_the_players_environment(self):
        calls = []

        def run(argv, cwd, log, token, env, events, stage):
            calls.append((argv, env, stage))
            if stage == 'shader-warm':
                Path(log).write_text('[mag] noise\n[shader-warm] 692 programs (288 vertex, 404 fragment) found in 0.3 s; '
                                     '2044 texts: 2044 translated\n')
        player = {'LOCALAPPDATA': 'C:/Users/P/AppData/Local', 'PS3RECOMP_SPIRV_CACHE': 'D:/mine'}
        result = self.warm(WINDOWS, run, environ=player)
        self.assertEqual(result['status'], 'done')
        (build, build_env, build_stage), (tool, tool_env, tool_stage) = calls
        self.assertEqual(build_stage, 'build-shader-warm')
        self.assertEqual(build[1:], ['--build', str(self.root / 'workspace/build-engine'), '--target', 'MagShaderWarm',
                                     '--parallel', '4'])
        self.assertEqual(build_env, {'PATH': 'build'})
        self.assertEqual(tool_stage, 'shader-warm')
        self.assertEqual(tool, [str(self.root / 'workspace/build-engine/MagShaderWarm.exe'), '--game',
                                str(self.root / 'workspace/local/retail-v1.0/PS3_GAME')])
        self.assertEqual(tool_env, player)
        self.assertIn('2044 translated', result['summary'])
        self.assertNotIn('[mag] noise', result['summary'])
        self.assertEqual(self.events[0], {'event': 'stage-start', 'stage': 'shader-warm', 'logs': str(self.root / 'logs')})
        self.assertIn('2044 translated', self.events[-1]['message'])

    def test_failures_are_warnings(self):
        for failing in ('build-shader-warm', 'shader-warm'):
            with self.subTest(failing=failing):
                self.events.clear()

                def run(argv, cwd, log, token, env, events, stage):
                    if stage == failing:
                        raise InstallerError('Local stage exited with code 1')
                result = self.warm(WINDOWS, run)
                self.assertEqual(result['status'], 'warning')
                self.assertEqual(self.events[-1]['status'], 'warning')
                self.assertIn('MAG is installed', self.events[-1]['message'])

        def gone(*a, **k):
            raise OSError('gone')
        self.assertEqual(self.warm(WINDOWS, gone)['status'], 'warning')

    def test_cancellation_still_cancels(self):
        def run(*a, **k):
            raise Cancelled('Installation cancelled')
        with self.assertRaises(Cancelled):
            self.warm(WINDOWS, run)


if __name__ == '__main__':
    unittest.main()
