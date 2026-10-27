"""Profiles pin each other by SHA-256; editing one without re-pinning its dependants breaks the build."""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / 'builds'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def path_pins(node):
    """Yield every {"path": ..., "sha256": ...} pair in a profile."""
    if isinstance(node, dict):
        if isinstance(node.get('path'), str) and isinstance(node.get('sha256'), str):
            yield node['path'], node['sha256']
        for value in node.values():
            yield from path_pins(value)
    elif isinstance(node, list):
        for value in node:
            yield from path_pins(value)


class ProfilePinTests(unittest.TestCase):
    def load(self, name):
        return json.loads((PROFILES / name).read_text())

    def test_engine_spu_pins_launcher_worker_profile(self):
        self.assertEqual(self.load('retail-v1.0-engine-spu.json')['worker_profile_sha256'],
                         sha256(PROFILES / 'retail-v1.0-spu.json'))

    def test_engine_modules_pin_shared_firmware_profile(self):
        self.assertEqual(self.load('retail-v1.0-engine-modules.json')['shared_profiles']['firmware'],
                         sha256(PROFILES / 'retail-v1.0-firmware.json'))

    def test_nonlocal_profiles_pin_their_configuration(self):
        for name in ('retail-v1.0-EBOOT-reservations.json', 'retail-v1.0-nonlocal.json'):
            profile = self.load(name)
            self.assertEqual(profile['configuration_sha256'], sha256(PROFILES / profile['configuration']), name)

    def test_every_tracked_path_pin_matches(self):
        checked = 0
        for profile in sorted(PROFILES.glob('*.json')):
            for path, digest in path_pins(json.loads(profile.read_text())):
                target = ROOT / path
                # Pins naming locally generated or original inputs are checked by the tools at run time.
                if target.parent != PROFILES or not target.is_file():
                    continue
                checked += 1
                self.assertEqual(digest, sha256(target), f'{profile.name} -> {path}')
        self.assertGreater(checked, 0)


if __name__ == '__main__':
    unittest.main()
