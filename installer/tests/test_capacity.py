import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
from mag_installer.model import InstallerError, Profile, host_key
from mag_installer.pipeline import preflight
from test_model import profile_data


class CapacityTests(unittest.TestCase):
    def test_same_volume_combines_requirements_without_combining_free_space(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            source.mkdir()
            with patch('mag_installer.capacity.volume', return_value=(1, 1000)):
                result = preflight(Profile.from_dict(profile_data()), source, root / 'out', root / 'cache')
            self.assertEqual(result['required_bytes'], len(b'synthetic input\n'))
            self.assertEqual(result['free_bytes'], 1000)
            self.assertFalse(result['estimate_complete'])
            self.assertEqual(len(result['filesystems']), 1)
            self.assertFalse((root / 'cache').exists())

    def test_insufficient_space_refused_before_any_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            source.mkdir()
            with patch('mag_installer.capacity.volume', return_value=(1, 0)):
                with self.assertRaisesRegex(InstallerError, 'space'):
                    preflight(Profile.from_dict(profile_data()), source, root / 'out', root / 'cache')
            self.assertEqual(list(root.iterdir()), [source])

    def test_separate_volumes_enforce_each_volume(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'; source.mkdir()
            data = profile_data()
            tool = {'id': 'tool', 'version': '1', 'url': 'https://example.com/tool', 'sha256': 'a'*64,
                    'size': 30, 'format': 'zip', 'max_expanded_size': 100, 'max_entries': 2,
                    'executable': 'tool', 'license': 'test'}
            data['platforms'][host_key()]['tools'] = [tool]
            def volumes(path):
                return (2, 20) if path == root / 'cache' else (1, 1000)
            with patch('mag_installer.capacity.volume', side_effect=volumes):
                with self.assertRaisesRegex(InstallerError, 'space'):
                    preflight(Profile.from_dict(data), source, root / 'out', root / 'cache')
