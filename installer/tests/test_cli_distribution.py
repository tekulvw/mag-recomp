import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from mag_installer.distribution import bundle
from mag_installer.model import InstallerError

ROOT = Path(__file__).resolve().parents[1]


class CliDistributionTests(unittest.TestCase):
    def cli(self, *args):
        return subprocess.run([sys.executable, '-m', 'mag_installer', *args], cwd=ROOT,
                              capture_output=True, text=True, timeout=15)

    def test_unsupported_python_exits_before_wizard(self):
        from mag_installer.__main__ import main
        import io
        with patch('mag_installer.__main__.sys.version_info', (3, 9)), patch('sys.stderr', new_callable=io.StringIO) as errors:
            self.assertEqual(main([]), 2)
            self.assertIn('Python 3.11 or newer', errors.getvalue())

    def test_no_arguments_dispatches_wizard_without_opening_window(self):
        from mag_installer.__main__ import main
        with patch('mag_installer.gui.launch', return_value=0) as launch:
            self.assertEqual(main([]), 0)
            launch.assert_called_once_with(ROOT, synthetic=False)

    def test_human_status_and_explicit_json(self):
        human = self.cli('status', '--human')
        self.assertEqual(human.returncode, 0, human.stderr)
        self.assertIn('blocked', human.stdout.lower())
        self.assertFalse(human.stdout.startswith('{'))
        self.assertEqual(json.loads(self.cli('status', '--json').stdout)['kind'], 'production')

    def test_parent_and_default_cache_are_resolved(self):
        from mag_installer.__main__ import main
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            with patch('mag_installer.__main__.install') as run:
                self.assertEqual(main(['install', '--synthetic', '--source', temp,
                                       '--parent', temp, '--keep-work']), 0)
                self.assertEqual(run.call_args.args[2], parent / 'MAG Demo')
                self.assertIsInstance(run.call_args.args[3], Path)
                self.assertTrue(run.call_args.kwargs['keep_work'])

    def test_status_reports_production_blocked_for_all_platforms(self):
        result = self.cli('status')
        self.assertEqual(result.returncode, 0, result.stderr)
        status = json.loads(result.stdout)
        self.assertEqual(status['kind'], 'production')
        self.assertFalse(status['release_ready'])
        self.assertEqual(len(status['platforms']), 6)
        self.assertTrue(all(item['status'] == 'blocked' for item in status['platforms'].values()))

    def test_production_install_fails_before_tool_acquisition(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            result = self.cli('install', '--source', str(root), '--output', str(root / 'out'), '--cache', str(root / 'cache'))
            self.assertEqual(result.returncode, 2)
            self.assertIn('blocked', result.stderr)
            self.assertFalse((root / 'cache').exists())
            self.assertFalse((root / 'out').exists())

    def test_cli_synthetic_end_to_end_and_profile_path_not_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source 日本語'
            source.mkdir()
            (source / 'SYNTHETIC-INPUT.txt').write_bytes(b'This is synthetic installer input.\n')
            result = self.cli('install', '--synthetic', '--source', str(source), '--output', str(root / 'out space'), '--cache', str(root / 'cache'))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('synthetic-complete', result.stdout)
            self.assertTrue((root / 'out space/SYNTHETIC-RESULT.txt').is_file())
            rejected = self.cli('status', '--profile', str(source / 'malicious.json'))
            self.assertEqual(rejected.returncode, 2)

    def test_asset_free_bundle_excludes_adjacent_private_content_and_existing_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'tooling'
            shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns('__pycache__', '.DS_Store'))
            for name in ('game-assets/game.bin', 'generated/game.cpp', 'application/MagRecomp', 'accounts/account.json', 'untracked.py'):
                path = source / name
                path.parent.mkdir(exist_ok=True, parents=True)
                path.write_bytes(b'PRIVATE-DECOY-CONTENT')
            output = bundle(source, root / 'foundation.zip')
            with zipfile.ZipFile(output) as archive:
                names = archive.namelist()
                self.assertIn('mag_installer/__main__.py', names)
                self.assertIn('BUNDLE-CHECKSUMS.json', names)
                for name in names:
                    self.assertNotIn(b'PRIVATE-DECOY-CONTENT', archive.read(name))
            original = output.read_bytes()
            with self.assertRaises(InstallerError): bundle(source, output)
            self.assertEqual(output.read_bytes(), original)

    def test_package_rejects_manifest_expansion_and_symlinked_payload(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'tooling'
            shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns('__pycache__', '.DS_Store'))
            manifest_path = source / 'distribution-manifest.json'
            original = manifest_path.read_text()
            manifest = json.loads(original)
            manifest['includes'].append('game.bin')
            (source / 'game.bin').write_bytes(b'PRIVATE')
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaises(InstallerError): bundle(source, root / 'bad.zip')
            manifest_path.write_text(original)
            (source / 'README.md').unlink()
            (source / 'README.md').symlink_to(source / 'game.bin')
            with self.assertRaises(InstallerError): bundle(source, root / 'bad-link.zip')


if __name__ == '__main__': unittest.main()
