"""The opt-in MAG voice service (src/voice, docs/voice-chat.md): synthetic checks of the core, the
guest glue against a stand-in runtime, and the build switch. No game data."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'src/voice'


def compiler():
    return shutil.which('clang++') or (None if os.name == 'nt' else shutil.which('c++'))


def runtime_checkout():
    for candidate in (os.environ.get('MAG_TEST_SDK'), ROOT / 'local/ps3recomp-runtime'):
        if candidate and (Path(candidate) / 'runtime/ppu/ppu_context.h').is_file():
            return Path(candidate)
    return None


def opus():
    """(include flags, link flags) for Opus, or None: MAG_TEST_OPUS_ROOT (an install prefix),
    pkg-config, or the system's default paths."""
    root = os.environ.get('MAG_TEST_OPUS_ROOT')
    if root:
        lib = Path(root) / 'lib'
        archive = next((p for p in (lib / 'libopus.a', lib / 'opus.lib') if p.is_file()), None)
        return (['-I' + str(Path(root) / 'include')], [str(archive)] if archive else ['-L' + str(lib), '-lopus'])
    if shutil.which('pkg-config'):
        run = subprocess.run(['pkg-config', '--cflags', '--libs', 'opus'], capture_output=True, text=True)
        if run.returncode == 0:
            flags = run.stdout.split()
            includes = [f[:-len('/opus')] if f.endswith('/opus') else f for f in flags if f.startswith('-I')]
            return (includes, [f for f in flags if not f.startswith('-I')])
    if Path('/usr/include/opus/opus.h').is_file():
        return ([], ['-lopus'])
    return None


def build(binary, sources, includes, extra=((), ())):
    flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror']
    if sys.platform == 'win32':
        flags += ['-D_CRT_SECURE_NO_WARNINGS']
    else:
        flags += ['-pthread']
    result = subprocess.run([compiler(), *flags, *('-I' + str(i) for i in includes), *extra[0], *map(str, sources),
                             '-o', str(binary), *extra[1]],
                            capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('MAG_VOICE', 'MAG_GAME_CONTROLS', 'VOICE_TEST'))}
    env.update(extra)
    return env


@unittest.skipUnless(compiler(), 'clang++ required')
class VoiceCore(unittest.TestCase):
    def test_core_rules_and_trace(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('core.exe' if sys.platform == 'win32' else 'core')
            build(binary, [SOURCES / 'mag_voice_core.cpp', ROOT / 'tests/voice_core.cpp'], [SOURCES])
            trace = Path(tmp) / 'trace.jsonl'
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                                 env=clean_env(VOICE_TRACE_PATH=str(trace)))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS voice core', run.stdout)
            events = [json.loads(line) for line in trace.read_text().splitlines()]
            self.assertTrue(any(e['event'] == 'summary' for e in events))
            for event in events:
                # Sizes, counts and times only: every value is a number or a short name.
                for key, value in event.items():
                    self.assertTrue(isinstance(value, (int, float)) or key == 'event', (key, value))


@unittest.skipUnless(compiler() and opus(), 'clang++ and Opus (MAG_TEST_OPUS_ROOT, pkg-config or libopus-dev) required')
class VoiceOpus(unittest.TestCase):
    def test_tone_survives_the_codec_and_two_services(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('opus.exe' if sys.platform == 'win32' else 'opus')
            build(binary, [SOURCES / 'mag_voice_core.cpp', SOURCES / 'mag_voice_opus.cpp', ROOT / 'tests/voice_opus.cpp'],
                  [SOURCES], opus())
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=clean_env())
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS voice opus', run.stdout)


def build_glue(binary, push_to_talk=False):
    sdk = runtime_checkout()
    flags = opus()
    if push_to_talk:
        flags = (['-DMAG_VOICE_PUSH_TO_TALK=1', *flags[0]], flags[1])
    build(binary, [ROOT / 'src/mag_extension.cpp', SOURCES / 'mag_voice_core.cpp', SOURCES / 'mag_voice_opus.cpp',
                   SOURCES / 'mag_voice_capture.cpp', SOURCES / 'mag_voice_hle.cpp', ROOT / 'tests/voice_hle.cpp'],
          [ROOT / 'src', ROOT / 'src/game_controls', SOURCES, sdk / 'runtime', sdk / 'include'], flags)


@unittest.skipUnless(compiler(), 'clang++ required')
class VoiceCapture(unittest.TestCase):
    def test_ring_resampler_gate_and_missing_microphone(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('capture.exe' if sys.platform == 'win32' else 'capture')
            build(binary, [SOURCES / 'mag_voice_core.cpp', SOURCES / 'mag_voice_capture.cpp', ROOT / 'tests/voice_capture.cpp'],
                  [SOURCES])
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120, env=clean_env())
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS voice capture', run.stdout)


def sdl_flags():
    for tool in (['pkg-config', '--cflags', 'sdl2'], ['sdl2-config', '--cflags']):
        if not shutil.which(tool[0]):
            continue
        run = subprocess.run(tool, capture_output=True, text=True)
        if run.returncode == 0:
            # SDL's own flags name include/SDL2; the sources include <SDL2/SDL.h> where it exists.
            return [f[:-len('/SDL2')] if f.endswith('/SDL2') else f for f in run.stdout.split()] + run.stdout.split()
    if Path('/usr/include/SDL2/SDL.h').is_file():
        return []
    return None


class VoiceBackends(unittest.TestCase):
    """The host capture backends compile (they need a device to run, so only the portable part is
    exercised above)."""

    def compile_only(self, cxx, source, extra=()):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(SOURCES), *extra, '-c',
                                     str(SOURCES / source), '-o', str(Path(tmp) / 'backend.o')],
                                    capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(compiler() and sdl_flags() is not None, 'clang++ and SDL2 (pkg-config) required')
    def test_sdl_capture_compiles(self):
        self.compile_only(compiler(), 'mag_voice_capture_sdl.cpp', sdl_flags())

    @unittest.skipUnless((sys.platform == 'win32' and compiler()) or shutil.which('x86_64-w64-mingw32-g++'),
                         'a Windows compiler (or MinGW) required')
    def test_wasapi_capture_compiles(self):
        cxx = compiler() if sys.platform == 'win32' else shutil.which('x86_64-w64-mingw32-g++')
        self.compile_only(cxx, 'mag_voice_capture_wasapi.cpp')


@unittest.skipUnless(compiler() and runtime_checkout() and opus(),
                     'clang++, a ps3recomp runtime checkout and Opus required')
class VoiceGuestGlue(unittest.TestCase):
    def test_off_unless_selected_and_big_endian_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('hle.exe' if sys.platform == 'win32' else 'hle')
            build_glue(binary)
            for env in (clean_env(), clean_env(MAG_VOICE_TEST_TONE='0'), clean_env(MAG_VOICE_MODE='off'),
                        clean_env(MAG_VOICE_MODE='open'), clean_env(MAG_VOICE_VOLUME='50'),
                        clean_env(MAG_VOICE_INPUT='USB Headset')):
                off = subprocess.run([str(binary), 'disabled'], capture_output=True, text=True, timeout=60, env=env)
                self.assertEqual(off.returncode, 0, off.stdout + off.stderr)
            trace = Path(tmp) / 'hle.jsonl'
            on = subprocess.run([str(binary), 'pattern'], capture_output=True, text=True, timeout=60,
                                env=clean_env(MAG_VOICE_TEST_TONE='pattern', MAG_VOICE_TRACE=str(trace)))
            self.assertEqual(on.returncode, 0, on.stdout + on.stderr)
            self.assertIn('PASS voice hle pattern', on.stdout)
            events = [json.loads(line) for line in trace.read_text().splitlines()]
            names = [e['event'] for e in events]
            for name in ('init', 'create_port', 'connect', 'port_info', 'start', 'read', 'write', 'end', 'summary'):
                self.assertIn(name, names)
            summary = [e for e in events if e['event'] == 'summary'][-1]
            self.assertGreater(summary['received_blocks'], 0)
            self.assertEqual(summary['received_blocks'], summary['pattern_ok'])
            tone = subprocess.run([str(binary), 'tone'], capture_output=True, text=True, timeout=60,
                                  env=clean_env(MAG_VOICE_TEST_TONE='tone', MAG_VOICE_VOLUME='80'))
            self.assertEqual(tone.returncode, 0, tone.stdout + tone.stderr)
            self.assertIn('PASS voice hle tone', tone.stdout)
            # Push to talk needs the controls adapter: without it the player mode only receives
            # and never opens the microphone.
            receive = subprocess.run([str(binary), 'receive'], capture_output=True, text=True, timeout=60,
                                     env=clean_env(MAG_VOICE_MODE='ptt'))
            self.assertEqual(receive.returncode, 0, receive.stdout + receive.stderr)
            self.assertIn('PASS voice hle receive', receive.stdout)
            self.assertIn('no push to talk: this build has no controls adapter', receive.stderr)

    def test_push_to_talk_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('ptt.exe' if sys.platform == 'win32' else 'ptt')
            build_glue(binary, push_to_talk=True)
            run = subprocess.run([str(binary), 'ptt'], capture_output=True, text=True, timeout=60,
                                 env=clean_env(MAG_VOICE_MODE='ptt', MAG_VOICE_INPUT='Stand-in'))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS voice hle ptt', run.stdout)
            self.assertIn('push to talk mode', run.stderr)
            self.assertEqual(run.stderr.count('microphone open'), 1)
            # A lab mode wins and never opens the microphone.
            lab = subprocess.run([str(binary), 'pattern'], capture_output=True, text=True, timeout=60,
                                 env=clean_env(MAG_VOICE_MODE='ptt', MAG_VOICE_TEST_TONE='pattern'))
            self.assertEqual(lab.returncode, 0, lab.stdout + lab.stderr)
            self.assertNotIn('microphone', lab.stderr)
            # No device: receive-only, said once, never fatal.
            missing = subprocess.run([str(binary), 'ptt'], capture_output=True, text=True, timeout=60,
                                     env=clean_env(MAG_VOICE_MODE='ptt', VOICE_TEST_MIC='none'))
            self.assertEqual(missing.returncode, 0, missing.stdout + missing.stderr)
            self.assertEqual(missing.stderr.count('no microphone (no capture device); voice is receive-only'), 1)
            # The controls adapter switched off at run time: receive-only, the microphone never opened.
            off = subprocess.run([str(binary), 'receive'], capture_output=True, text=True, timeout=60,
                                 env=clean_env(MAG_VOICE_MODE='ptt', MAG_GAME_CONTROLS='0'))
            self.assertEqual(off.returncode, 0, off.stdout + off.stderr)
            self.assertIn('the controls adapter is off (MAG_GAME_CONTROLS=0); voice is receive-only', off.stderr)


class VoiceBuildSwitch(unittest.TestCase):
    def test_off_by_default_and_engine_only(self):
        module = (ROOT / 'cmake/MagVoice.cmake').read_text()
        self.assertRegex(module, r'option\(MAG_VOICE "[^"]*" OFF\)')
        self.assertIn('if(NOT MAG_VOICE)\n        return()', module)
        # The launcher image gets only the settings windows' definition, never the service.
        launcher = module[module.index('if(NOT image STREQUAL "mage_g")'):]
        launcher = launcher[:launcher.index('endif()')]
        self.assertIn('return()', launcher)
        self.assertLess(module.index('MAG_PLAYER_VOICE_SUPPORTED=1'), module.index('if(NOT image STREQUAL "mage_g")'))
        self.assertLess(module.index('if(NOT image STREQUAL "mage_g")'), module.index('target_sources('))
        lists = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MagVoice.cmake")', lists)
        self.assertIn('mag_enable_voice(${PROJECT_NAME} "${PS3RECOMP_DIR}" "${MAG_IMAGE}")', lists)
        self.assertIn('list(APPEND MAG_RECEIPT_EXTRA ${MAG_VOICE_RECEIPT_EXTRA})', lists)

    def test_registers_only_from_its_start_callback(self):
        glue = (SOURCES / 'mag_voice_hle.cpp').read_text()
        # Handlers are registered in one place, after the environment selected a mode.
        self.assertEqual(len(re.findall(r'ps3_hle_register_ctx\(ps3_compute_nid', glue)), 1)
        start = glue[glue.index('void voice_start('):glue.index('struct Registration')]
        self.assertLess(start.index('return;'), start.index('ps3_hle_register_ctx('))
        # After the player settings exported MAG_VOICE_* (main.cpp: extensions' prepare, then the
        # settings, then the ELF load and the extensions' start).
        self.assertIn('callbacks.start = voice_start;', glue)
        self.assertNotIn('callbacks.prepare', glue)
        main = (ROOT / 'src/main.cpp').read_text()
        self.assertLess(main.index('mag_player_settings_prepare()'), main.index('mag_extension()->start'))
        # The fork is not patched: no SDK source is renamed or replaced.
        self.assertNotIn('libs/audio', (ROOT / 'cmake/MagVoice.cmake').read_text())


if __name__ == '__main__':
    unittest.main()
