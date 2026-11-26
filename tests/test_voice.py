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


def build(binary, sources, includes):
    flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror']
    if sys.platform == 'win32':
        flags += ['-D_CRT_SECURE_NO_WARNINGS']
    else:
        flags += ['-pthread']
    result = subprocess.run([compiler(), *flags, *('-I' + str(i) for i in includes), *map(str, sources), '-o', str(binary)],
                            capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('MAG_VOICE')}
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


@unittest.skipUnless(compiler() and runtime_checkout(), 'clang++ and a ps3recomp runtime checkout required')
class VoiceGuestGlue(unittest.TestCase):
    def test_off_unless_selected_and_big_endian_blocks(self):
        sdk = runtime_checkout()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('hle.exe' if sys.platform == 'win32' else 'hle')
            build(binary, [ROOT / 'src/mag_extension.cpp', SOURCES / 'mag_voice_core.cpp', SOURCES / 'mag_voice_hle.cpp',
                           ROOT / 'tests/voice_hle.cpp'],
                  [ROOT / 'src', SOURCES, sdk / 'runtime', sdk / 'include'])
            for env in (clean_env(), clean_env(MAG_VOICE_TEST_TONE='0'), clean_env(MAG_VOICE_MODE='off'),
                        clean_env(MAG_VOICE_MODE='ptt')):
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


class VoiceBuildSwitch(unittest.TestCase):
    def test_off_by_default_and_engine_only(self):
        module = (ROOT / 'cmake/MagVoice.cmake').read_text()
        self.assertRegex(module, r'option\(MAG_VOICE "[^"]*" OFF\)')
        self.assertIn('if(NOT MAG_VOICE)\n        return()', module)
        self.assertIn('MAG_VOICE applies to the mage_g engine image only', module)
        lists = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MagVoice.cmake")', lists)
        self.assertIn('mag_enable_voice(${PROJECT_NAME} "${PS3RECOMP_DIR}" "${MAG_IMAGE}")', lists)
        self.assertIn('list(APPEND MAG_RECEIPT_EXTRA ${MAG_VOICE_RECEIPT_EXTRA})', lists)

    def test_registers_only_from_its_prepare_callback(self):
        glue = (SOURCES / 'mag_voice_hle.cpp').read_text()
        # Handlers are registered in one place, after the environment selected a mode.
        self.assertEqual(len(re.findall(r'ps3_hle_register_ctx\(ps3_compute_nid', glue)), 1)
        prepare = glue[glue.index('void voice_prepare('):glue.index('struct Registration')]
        self.assertLess(prepare.index('return;'), prepare.index('ps3_hle_register_ctx('))
        # The fork is not patched: no SDK source is renamed or replaced.
        self.assertNotIn('libs/audio', (ROOT / 'cmake/MagVoice.cmake').read_text())


if __name__ == '__main__':
    unittest.main()
