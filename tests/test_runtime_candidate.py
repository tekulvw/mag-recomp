import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools import provenance
from tools.prepare_inputs import sha256_file


class RuntimeCandidateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sdk = self.root / 'sdk'
        self.sdk.mkdir()
        self.git('init', '-q')
        (self.sdk / 'runtime.c').write_text('/* candidate runtime */\n')
        self.git('add', 'runtime.c')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'candidate')
        self.revision = self.git('rev-parse', 'HEAD').strip()
        self.framework = {'allowed_runtime_revisions': ['a' * 40]}
        self.framework_path = self.root / 'framework.json'
        self.framework_path.write_text(json.dumps(self.framework))
        self.manifest = self.root / 'candidate.json'
        self.data = dict(schema_version=1, purpose='frontend integration test',
                         sdk_revision=self.revision,
                         sdk_tree=self.git('rev-parse', 'HEAD^{tree}').strip(),
                         framework_sha256=sha256_file(self.framework_path))
        self.write_manifest()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.sdk), *args], text=True)

    def write_manifest(self):
        self.manifest.write_text(json.dumps(self.data))

    def validate(self, manifest=None):
        return provenance.runtime_identity(self.sdk, self.framework,
                                           self.framework_path, manifest)

    def test_explicit_candidate_preserves_normal_pin_and_records_exception(self):
        self.assertTrue(hasattr(provenance, 'runtime_identity'),
                        'explicit candidate validation is not implemented')
        with self.assertRaisesRegex(ValueError, 'framework'):
            self.validate()
        result = self.validate(self.manifest)
        self.assertEqual(result['sdk_revision'], self.revision)
        self.assertTrue(result['runtime_candidate']['experimental'])
        self.assertEqual(result['runtime_candidate']['manifest_sha256'], sha256_file(self.manifest))
        self.assertEqual(json.loads(self.framework_path.read_text()), self.framework)

    def test_candidate_rejects_wrong_revision_tree_or_baseline(self):
        for field in ('sdk_revision', 'sdk_tree', 'framework_sha256'):
            with self.subTest(field=field):
                saved = self.data[field]
                self.data[field] = '0' * len(saved)
                self.write_manifest()
                with self.assertRaises(ValueError):
                    self.validate(self.manifest)
                self.data[field] = saved

    def test_candidate_cannot_accept_modified_or_untracked_source(self):
        for name in ('runtime.c', 'unexpected.h'):
            with self.subTest(name=name):
                path = self.sdk / name
                original = path.read_bytes() if path.exists() else None
                path.write_text('changed source\n')
                with self.assertRaisesRegex(ValueError, 'clean'):
                    self.validate(self.manifest)
                if original is None:
                    path.unlink()
                else:
                    path.write_bytes(original)

    def test_reviewed_default_needs_no_candidate(self):
        self.framework['allowed_runtime_revisions'].append(self.revision)
        result = self.validate()
        self.assertEqual(result['sdk_revision'], self.revision)
        self.assertNotIn('runtime_candidate', result)


if __name__ == '__main__':
    unittest.main()
