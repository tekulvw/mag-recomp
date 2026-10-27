import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from mag_installer.model import InstallerError, Profile, host_key
from mag_installer.safety import Cancellation, validate_inputs


def profile_data(data=b'synthetic input\n', host=None):
    return {
        'schema_version': 1, 'id': 'synthetic-fixture', 'revision': 1,
        'kind': 'synthetic', 'approval': 'reviewed',
        'identity': {'label': 'Synthetic test fixture', 'provenance': 'tests only; no game data',
                     'inputs': [{'path': 'game data/入力.txt', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}]},
        'platforms': {host or host_key(): {'status': 'ready', 'reason': 'Synthetic fixture only',
                                        'tools': [], 'stages': [], 'package': []}}
    }


class ModelTests(unittest.TestCase):
    def test_strict_profile_rejects_unknown_stale_and_unapproved_recipe(self):
        for mutation in ('schema', 'unknown', 'approval'):
            data = profile_data()
            if mutation == 'schema': data['schema_version'] = 99
            if mutation == 'unknown': data['executable_from_input'] = 'bad'
            if mutation == 'approval': data['approval'] = 'unconfigured'
            with self.subTest(mutation=mutation), self.assertRaises(InstallerError):
                Profile.from_dict(data)

    def test_host_adapter_maps_platform_and_refuses_unknown_arch(self):
        for system, machine, expected in [
            ('Windows', 'AMD64', 'windows-x86_64'), ('Windows', 'ARM64', 'windows-arm64'),
            ('Linux', 'x86_64', 'linux-x86_64'), ('Linux', 'aarch64', 'linux-arm64'),
            ('Darwin', 'arm64', 'macos-arm64'), ('Darwin', 'x86_64', 'macos-x86_64')]:
            self.assertEqual(host_key(system, machine), expected)
        with self.assertRaises(InstallerError): host_key('Linux', 'riscv64')

    def test_hash_size_version_identity_and_unicode_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / 'game data/入力.txt'
            path.parent.mkdir()
            path.write_bytes(b'synthetic input\n')
            profile = Profile.from_dict(profile_data())
            identity = validate_inputs(profile, root, Cancellation())
            self.assertEqual(identity['files'][0]['sha256'], '3099b6c8cfec72b28f3150fc80c97025ae313d03dc9c72dc9a5556523b939456')
            path.write_bytes(b'changed contents')
            with self.assertRaises(InstallerError): validate_inputs(profile, root, Cancellation())

    def test_source_traversal_and_symlink_refused(self):
        for name in ('../escape', '/absolute', 'C:/drive', 'a\\b', 'CON', 'a/../b'):
            data = profile_data()
            data['identity']['inputs'][0]['path'] = name
            with self.subTest(name=name), self.assertRaises(InstallerError): Profile.from_dict(data)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'game data').symlink_to(root, target_is_directory=True)
            (root / '入力.txt').write_bytes(b'synthetic input\n')
            with self.assertRaises(InstallerError):
                validate_inputs(Profile.from_dict(profile_data()), root, Cancellation())

    def test_cancellation_during_hash_never_returns_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            token = Cancellation()
            token.cancel()
            with self.assertRaises(InstallerError):
                validate_inputs(Profile.from_dict(profile_data()), Path(temp), token)


if __name__ == '__main__': unittest.main()
