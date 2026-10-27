"""The ps3recomp table in profiles/framework.json is the single source of SDK pins."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from tools.fetch_ps3recomp import fetch

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / 'profiles'


def load(name):
    return json.loads((PROFILES / name).read_text())


class PinTableTests(unittest.TestCase):
    def setUp(self):
        self.framework = load('framework.json')
        self.table = self.framework['ps3recomp']

    def test_accept_lists_name_only_the_pinned_runtime_and_ppu_lifter(self):
        runtime = self.table['runtime']['commit']
        for key in ('allowed_runtime_revisions', 'strict_runtime_revisions', 'strict_hle_runtime_revisions'):
            self.assertEqual(self.framework[key], [runtime], key)
        self.assertTrue(set(self.framework['first_nonblack_capture_revisions']) <= {runtime})
        self.assertEqual(self.framework['allowed_lifter_revisions'], [self.table['lifter-ppu']['commit']])

    def test_profiles_use_the_table(self):
        spu, ppu = self.table['lifter-spu'], self.table['lifter-ppu']
        for name, pin in (('retail-v1.0-spu.json', spu), ('retail-v1.0-engine-spu.json', spu), ('retail-v1.0-firmware.json', ppu)):
            profile = load(name)
            self.assertEqual((profile['lifter_repository'], profile['lifter_revision']), (pin['path'], pin['commit']), name)
        for name in ('retail-v1.0-EBOOT-reservations.json', 'retail-v1.0-nonlocal.json'):
            self.assertEqual(load(name)['sdk_revision'], ppu['commit'], name)
        self.assertEqual(load('retail-v1.0-spu-coverage.json')['lifter_revision'], spu['commit'])

    def test_building_guide_uses_the_table(self):
        guide = (ROOT / 'BUILDING.md').read_text()
        for role, pin in self.table.items():
            self.assertIn(pin['tag'], guide, role)
            self.assertIn(pin['path'], guide, role)


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.origin = base / 'origin'
        self.root = base / 'repo'
        git = lambda *a: subprocess.run(['git', '-C', str(self.origin), *a], check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run(['git', 'init', '-q', str(self.origin)], check=True)
        for n in ('one', 'two'):
            (self.origin / 'f').write_text(n)
            git('add', 'f')
            git('-c', 'user.name=t', '-c', 'user.email=t@example.invalid', 'commit', '-q', '-m', n)
            git('tag', '-a', f'mag-recomp/x-{n}', '-m', n)
        self.first = git('rev-parse', 'mag-recomp/x-one^{commit}')
        self.second = git('rev-parse', 'HEAD')

    def test_checks_out_the_pinned_commit(self):
        path = fetch(self.root, str(self.origin), 'x', {'tag': 'mag-recomp/x-one', 'commit': self.first, 'path': 'local/x'})
        self.assertEqual((path / 'f').read_text(), 'one')

    def test_refuses_a_tag_that_names_another_commit(self):
        with self.assertRaisesRegex(ValueError, 'tag mag-recomp/x-two'):
            fetch(self.root, str(self.origin), 'x', {'tag': 'mag-recomp/x-two', 'commit': self.first, 'path': 'local/x'})


if __name__ == '__main__':
    unittest.main()
