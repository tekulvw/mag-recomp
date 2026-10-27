"""MAG_WORKSPACE moves working data; a workspace pin file overrides the public pins."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = ("import json,sys;from tools import prepare_inputs as p;from tools.fetch_ps3recomp import pins;"
         "f,path=p.load_framework();print(json.dumps({'root':str(p.ROOT),'workspace':str(p.WORKSPACE),"
         "'data':str(p.data_path('generated/a')),'framework':str(path),"
         "'runtime':f['allowed_runtime_revisions'],'table':sorted(pins(f))}))")


def probe(workspace=None):
    env = {k: v for k, v in os.environ.items() if k != 'MAG_WORKSPACE'}
    if workspace is not None:
        env['MAG_WORKSPACE'] = str(workspace)
    out = subprocess.run([sys.executable, '-c', PROBE], cwd=ROOT, env=env, check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.workspace = Path(self.tmp.name).resolve()

    def write_overlay(self, revisions):
        public = json.loads((ROOT / 'profiles/framework.json').read_text())
        public['allowed_runtime_revisions'] = revisions
        public['ps3recomp'] = {'runtime': dict(public['ps3recomp']['runtime'], path='elsewhere/runtime')}
        (self.workspace / 'profiles').mkdir()
        (self.workspace / 'profiles/framework.json').write_text(json.dumps(public))

    def test_workspace_default_is_root(self):
        r = probe()
        self.assertEqual((r['workspace'], r['data']), (r['root'], str(ROOT / 'generated/a')))

    def test_workspace_env_moves_data_only(self):
        r = probe(self.workspace)
        self.assertEqual(r['root'], str(ROOT))
        self.assertEqual(r['data'], str(self.workspace / 'generated/a'))

    def test_overlay_absent_uses_public_pins(self):
        r = probe(self.workspace)
        self.assertEqual(r['framework'], str(ROOT / 'profiles/framework.json'))
        self.assertEqual(r['runtime'], json.loads((ROOT / 'profiles/framework.json').read_text())['allowed_runtime_revisions'])

    def test_overlay_used_when_present(self):
        self.write_overlay(['f' * 40])
        r = probe(self.workspace)
        self.assertEqual((r['framework'], r['runtime']), (str(self.workspace / 'profiles/framework.json'), ['f' * 40]))

    def test_overlay_ignored_when_workspace_is_root(self):
        r = probe(ROOT)
        self.assertEqual(r['framework'], str(ROOT / 'profiles/framework.json'))

    def test_fetch_uses_overlay_table(self):
        self.write_overlay(['f' * 40])
        self.assertEqual(probe(self.workspace)['table'], ['runtime'])


if __name__ == '__main__':
    unittest.main()
