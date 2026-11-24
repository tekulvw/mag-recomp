"""Friends and presence through the portal (src/player_social.cpp, src/mag_social_bridge.cpp).

All inputs are synthetic: the portal, its friends, presence bytes and device tokens are made up by
the tests, and the runtime's provider API is faked for the bridge.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = pathlib.Path(os.environ.get('MAG_TEST_SDK') or ROOT / 'local/ps3recomp-runtime')
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


def build(binary, sources, extra=()):
    # C++17: the packaged supervisors compile the core too.
    return subprocess.run([COMPILER, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(ROOT / 'src'), *extra,
                           *[str(ROOT / s) for s in sources], '-o', str(binary)], capture_output=True, text=True, timeout=180)


@unittest.skipUnless(COMPILER and os.name != 'nt', 'C++ compiler (POSIX)')
class SocialCoreTests(unittest.TestCase):
    def test_snapshot_diff_lines_portal_worker_and_spool(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'social'
            result = build(binary, ['tests/player_social.cpp', 'src/player_social.cpp', 'src/player_portal.cpp'])
            self.assertEqual(result.returncode, 0, result.stderr)
            scratch = pathlib.Path(tmp) / 'scratch'; scratch.mkdir()
            run = subprocess.run([str(binary), str(scratch)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('portal social checks passed', run.stdout)


@unittest.skipUnless(COMPILER and os.name != 'nt' and (SDK / 'include/ps3emu/np_social.h').is_file(),
                     'C++ compiler and a runtime with the NP social provider (include/ps3emu/np_social.h)')
class SocialBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.temp.name) / 'bridge'
        result = build(cls.binary, ['tests/mag_social_bridge.cpp', 'src/mag_social_bridge.cpp', 'src/player_social.cpp',
                                    'src/player_portal.cpp'], ['-I' + str(SDK / 'include')])
        if result.returncode:
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_mode(self, mode, env=None):
        scratch = pathlib.Path(tempfile.mkdtemp(dir=self.temp.name))
        clean = {k: v for k, v in os.environ.items() if not k.startswith('MAG_SOCIAL')}
        clean.update(env or {})
        result = subprocess.run([str(self.binary), mode, str(scratch)], capture_output=True, text=True, timeout=60, env=clean)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_spool_bridge(self):
        self.assertIn('social bridge spool checks passed', self.run_mode('spool').stdout)

    def test_in_process_worker_marks_the_player_offline_at_exit(self):
        out = self.run_mode('inprocess').stdout
        self.assertIn('social bridge inprocess checks passed', out)
        self.assertIn('offline published', out)

    def test_inert_without_a_link_or_when_switched_off(self):
        self.run_mode('none')
        spool = pathlib.Path(tempfile.mkdtemp(dir=self.temp.name))
        (spool / 'to-game').write_bytes(b''); (spool / 'from-game').write_bytes(b'')
        self.run_mode('off', {'MAG_SOCIAL': '0', 'MAG_SOCIAL_SPOOL': str(spool), 'MAG_SOCIAL_PORTAL': 'https://portal.test',
                              'MAG_SOCIAL_LINK_FILE': str(spool / 'portal-link.json')})


if __name__ == '__main__':
    unittest.main()
