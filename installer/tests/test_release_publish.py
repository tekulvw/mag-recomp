import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'publish_release.py'
SPEC = importlib.util.spec_from_file_location('publish_release', SCRIPT)
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class ReleasePublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tag, self.revision = 'v1.2.3', 'a' * 40
        self.calls = []
        self.remote = {'tag_name': self.tag, 'draft': False, 'immutable': False, 'assets': []}
        self.files = []
        for target, suffix in [('windows-x86_64', '.zip'), ('macos-arm64', '.zip')]:
            archive = f'mag-installer-{self.tag}-{target}{suffix}'
            self.files.extend([archive, archive + '.sha256'])
        self.files.append('SHA256SUMS.txt')
        records = []
        for name in self.files:
            data = ('synthetic fixture ' + name).encode()
            (self.root / name).write_bytes(data)
            records.append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        self.manifest = {'schema_version': 1, 'version': self.tag, 'source_revision': self.revision,
                         'kind': 'release', 'files': records}
        self.save_manifest()

    def save_manifest(self):
        (self.root / 'RELEASE-ASSETS.json').write_text(json.dumps(self.manifest))

    def runner(self, argv, **kwargs):
        self.calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, json.dumps(self.remote))

    def publish(self):
        return publisher.publish(self.root, self.tag, 'example/mag', self.revision, self.runner)

    def test_uploads_exact_verified_set_without_clobber(self):
        names = self.publish()
        self.assertEqual(set(names), set(self.files) | {'RELEASE-ASSETS.json'})
        self.assertEqual(self.calls[0][2], 'verify-set')
        self.assertIn(self.revision, self.calls[0])
        self.assertEqual(self.calls[1][:4], ['gh', 'api', '--method', 'GET'])
        self.assertEqual(self.calls[2][:6], ['gh', 'release', 'upload', self.tag, '--repo', 'example/mag'])
        self.assertEqual({Path(value).name for value in self.calls[2][6:]}, set(names))
        self.assertNotIn('--clobber', self.calls[2])

    def test_changed_bytes_refuse_before_any_command(self):
        (self.root / self.files[0]).write_bytes(b'changed')
        with self.assertRaises(publisher.PublicationError): self.publish()
        self.assertFalse(self.calls)

    def test_extra_private_file_refuses_before_any_command(self):
        (self.root / 'private-capture.log').write_text('synthetic private fixture')
        with self.assertRaises(publisher.PublicationError): self.publish()
        self.assertFalse(self.calls)

    def test_preview_and_wrong_source_refuse_before_any_command(self):
        for field, value in [('kind', 'preview'), ('source_revision', 'b' * 40), ('version', 'v9')]:
            with self.subTest(field=field):
                old = self.manifest[field]
                self.manifest[field] = value
                self.save_manifest()
                with self.assertRaises(publisher.PublicationError): self.publish()
                self.assertFalse(self.calls)
                self.manifest[field] = old

    def test_immutable_or_existing_assets_never_upload(self):
        for change in [{'immutable': True}, {'assets': [{'name': self.files[0]}]}, {'draft': True}]:
            with self.subTest(change=change):
                self.calls.clear()
                self.remote = {'tag_name': self.tag, 'draft': False, 'immutable': False, 'assets': []} | change
                with self.assertRaises(publisher.PublicationError): self.publish()
                self.assertEqual(len(self.calls), 2)

    def test_validator_failure_never_contacts_github(self):
        def refuse(argv, **kwargs):
            self.calls.append(argv)
            return subprocess.CompletedProcess(argv, 2)
        with self.assertRaises(publisher.PublicationError):
            publisher.publish(self.root, self.tag, 'example/mag', self.revision, refuse)
        self.assertEqual(len(self.calls), 1)

    def test_invalid_tag_never_enters_shell_or_command(self):
        self.tag = 'v1; echo unsafe'
        with self.assertRaises(publisher.PublicationError): self.publish()
        self.assertFalse(self.calls)


if __name__ == '__main__':
    unittest.main()
