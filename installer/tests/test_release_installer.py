import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
import zipfile
from unittest.mock import patch

from mag_installer.model import InstallerError
from mag_installer.releasing import check, stage, package_payload, verify_artifact, verify_set, artifact_name, build

ROOT = Path(__file__).resolve().parents[1]
REVISION = 'a' * 40


class ReleaseTests(unittest.TestCase):
    def kit_root(self, root, host, kit_bytes=b'synthetic kit'):
        """A copy of the installer source with a per-host kit, as CI's kit job writes it."""
        stage = root / ('source-' + host)
        shutil.copytree(ROOT, stage, ignore=shutil.ignore_patterns('__pycache__', 'artifacts'))
        (stage / 'release/native-kit.tar.gz').write_bytes(kit_bytes)
        manifest = {'schema_version': 1, 'host': host, 'distribution': 'source-kit',
                    'kit': {'embedded': 'release/native-kit.tar.gz', 'size': len(kit_bytes),
                            'sha256': hashlib.sha256(kit_bytes).hexdigest()}}
        (stage / 'release/player-build.json').write_text(json.dumps(manifest))
        return stage

    def test_source_kit_resource_is_exactly_pinned_and_rejects_other_paths(self):
        from mag_installer.releasing import _extra_resources
        with tempfile.TemporaryDirectory() as temp:
            root = self.kit_root(Path(temp), 'macos-arm64')
            manifest_bytes = (root / 'release/player-build.json').read_bytes()
            self.assertEqual(_extra_resources(root, 'macos-arm64'),
                             {'release/player-build.json': manifest_bytes, 'release/native-kit.tar.gz': b'synthetic kit'})
            with self.assertRaisesRegex(InstallerError, 'another host'):
                _extra_resources(root, 'windows-x86_64')
            (root / 'release/native-kit.tar.gz').write_bytes(b'tampered! kit')
            with self.assertRaises(InstallerError):
                _extra_resources(root)
            manifest = json.loads(manifest_bytes); manifest['kit']['embedded'] = 'game/EBOOT.BIN'
            (root / 'release/player-build.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(InstallerError, 'Unrecognized'):
                _extra_resources(root)
        self.assertEqual(_extra_resources(ROOT), {})  # the checkout's placeholder carries no kit

    def test_set_with_per_host_kits_verifies_against_the_plain_checkout(self):
        # Regression: each host embeds its own kit manifest, and the publishing
        # checkout has only the placeholder; the set must still verify.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); out = root / 'assets'; out.mkdir()
            for host, kit in (('windows-x86_64', b'windows kit'), ('macos-arm64', b'macos kit'), ('linux-x86_64', b'linux kit')):
                package_payload(self.payload(root, host), out, 'preview-test', host, 'preview', REVISION,
                                self.kit_root(root, host, kit))
            manifest = verify_set(out, ROOT, 'preview-test', REVISION, preview=True)
            self.assertEqual(manifest['kind'], 'preview')

    def test_kit_for_another_host_or_tampered_kit_is_refused_in_an_archive(self):
        for case in ('host', 'tampered'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); out = root / 'assets'; out.mkdir()
                stage = self.kit_root(root, 'windows-x86_64')
                artifact = package_payload(self.payload(root, 'windows-x86_64'), out, 'preview-test', 'windows-x86_64',
                                           'preview', REVISION, stage)
                verify_artifact(artifact, ROOT)
                changed = root / 'changed'; changed.mkdir(); target = changed / artifact.name
                with zipfile.ZipFile(artifact) as source, zipfile.ZipFile(target, 'w') as destination:
                    for info in source.infolist():
                        data = source.read(info)
                        if case == 'host' and info.filename.endswith('/resources/release/player-build.json'):
                            data = data.replace(b'windows-x86_64', b'macos-arm64')
                        if case == 'tampered' and info.filename.endswith('/resources/release/native-kit.tar.gz'):
                            data = b'tampered! kit'
                        destination.writestr(info, data)
                with self.assertRaises(InstallerError): verify_artifact(target, ROOT)

    def test_release_installer_without_its_kit_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); out = root / 'assets'; out.mkdir()
            with patch.dict(os.environ, {'GITHUB_REPOSITORY': 'tekulvw/mag-recomp'}):
                artifact = package_payload(self.payload(root, 'windows-x86_64'), out, 'v0.1.0', 'windows-x86_64',
                                           'release', REVISION, ROOT, smoke_verified=True)
                with self.assertRaisesRegex(InstallerError, 'must carry its source kit'):
                    verify_artifact(artifact, ROOT)

    def test_native_build_requires_tk_runtime_without_creating_display(self):
        from mag_installer.releasing import _check_tk_runtime
        with patch('mag_installer.releasing.subprocess.run') as run:
            _check_tk_runtime()
            command = run.call_args.args[0]
            self.assertIn('import tkinter', command[-1])
            self.assertNotIn('Tk()', command[-1])
        import subprocess
        with patch('mag_installer.releasing.subprocess.run', side_effect=subprocess.CalledProcessError(1, ['probe'])):
            with self.assertRaisesRegex(InstallerError, 'Tcl/Tk'):
                _check_tk_runtime()

    def test_frozen_smoke_failure_retains_bounded_decoded_diagnostics(self):
        from mag_installer.releasing import _smoke_command
        import subprocess
        failure = subprocess.CalledProcessError(1, ['fixture', '--help'], output=b'output evidence', stderr=b'x' * 8000 + b'failure evidence')
        with patch('mag_installer.releasing.subprocess.run', side_effect=failure):
            with self.assertRaises(InstallerError) as caught:
                _smoke_command(['fixture', '--help'], timeout=30)
        self.assertIn('output evidence', str(caught.exception))
        self.assertIn('failure evidence', str(caught.exception))
        self.assertIn('exit 1', str(caught.exception))
        self.assertLess(len(str(caught.exception)), 6500)

    def test_macos_app_wraps_cli_binary_and_rejects_wrong_identity(self):
        from mag_installer.releasing import _prepare_launcher, MAC_PLIST
        import plistlib
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'launcher'
            source.write_bytes(b'SYNTHETIC-NOT-EXECUTED')
            payload = root / 'payload'
            binary = _prepare_launcher(source, payload, 'macos-arm64', 'preview-test')
            self.assertEqual(binary.relative_to(payload).as_posix(), 'MAG Installer.app/Contents/MacOS/mag-installer')
            self.assertEqual(binary.read_bytes(), source.read_bytes())
            self.assertEqual(plistlib.loads((payload / MAC_PLIST).read_bytes())['CFBundleExecutable'], binary.name)
            licenses = payload / 'licenses'
            licenses.mkdir()
            for name in ('PYTHON-LICENSE.txt', 'PYINSTALLER-COPYING.txt'):
                (licenses / name).write_text('Fixture notice')
            artifact = package_payload(payload, root / 'out', 'preview-test', 'macos-arm64', 'preview', REVISION, ROOT)
            verify_artifact(artifact, ROOT)
            (payload / MAC_PLIST).write_bytes(b'<plist>wrong identity</plist>')
            with self.assertRaisesRegex(InstallerError, 'metadata'):
                package_payload(payload, root / 'bad', 'preview-test', 'macos-arm64', 'preview', REVISION, ROOT)

    def payload(self, root, target='macos-arm64'):
        payload = root / ('payload-' + target)
        payload.mkdir()
        from mag_installer.releasing import _binary_path, _app_plist, MAC_PLIST
        binary = payload / _binary_path(target)
        binary.parent.mkdir(parents=True, exist_ok=True)
        if target.startswith('macos-'):
            (payload / MAC_PLIST).write_bytes(_app_plist('preview-test'))
        binary.write_bytes(b'SYNTHETIC ARCHIVE FIXTURE - NEVER EXECUTED')
        binary.chmod(0o755)
        licenses = payload / 'licenses'
        licenses.mkdir()
        (licenses / 'PYTHON-LICENSE.txt').write_text('Synthetic Python license fixture')
        (licenses / 'PYINSTALLER-COPYING.txt').write_text('Synthetic packager license fixture')
        return payload

    def artifact(self, root, target='macos-arm64', version='preview-test', revision=REVISION):
        out = root / 'assets'
        out.mkdir(exist_ok=True)
        return package_payload(self.payload(root, target), out, version, target, 'preview', revision, ROOT)

    def test_release_policy_needs_a_release_please_tag_from_this_repository(self):
        self.assertEqual(check('v0.2.0', 'release', 'tekulvw/mag-recomp')['targets'], ['windows-x86_64', 'macos-arm64', 'linux-x86_64'])
        self.assertEqual(check('preview-7', 'preview', 'anyone/fork')['kind'], 'preview')
        for version, repository in [('preview-7', 'tekulvw/mag-recomp'), ('v1', 'tekulvw/mag-recomp'),
                                    ('v0.2.0', 'someone/fork'), ('v0.2.0', 'unknown')]:
            with self.subTest(version=version, repository=repository), self.assertRaises(InstallerError):
                check(version, 'release', repository)
        with self.assertRaises(InstallerError): check('v0.2.0', 'production', 'tekulvw/mag-recomp')

    def test_clean_stage_excludes_untracked_game_cache_and_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns('__pycache__', 'artifacts'))
            for name in ('game/EBOOT.elf', 'cache/compiler.zip', 'tests/private.txt', 'artifacts/local-app'):
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'PRIVATE-DECOY')
            destination = stage(source, root / 'clean')
            self.assertTrue((destination / 'scripts/release_installer.py').is_file())
            self.assertTrue((destination / 'scripts/publish_release.py').is_file())
            for path in destination.rglob('*'):
                if path.is_file(): self.assertNotIn(b'PRIVATE-DECOY', path.read_bytes())
            self.assertFalse((destination / 'tests').exists())
            with self.assertRaises(InstallerError): stage(source, destination)

    def test_archive_inventory_modes_hashes_and_no_synthetic_game_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact = self.artifact(root)
            report = verify_artifact(artifact, ROOT)
            self.assertEqual(report['kind'], 'preview')
            self.assertFalse(report['release_ready'])
            self.assertEqual(report['source_revision'], REVISION)
            with zipfile.ZipFile(artifact) as archive:
                self.assertTrue(any(name.endswith('licenses/PYTHON-LICENSE.txt') for name in archive.namelist()))
                binary = next(info for info in archive.infolist() if info.filename.endswith('/mag-installer'))
                self.assertEqual((binary.external_attr >> 16) & 0o777, 0o755)

    def test_archive_member_tampering_and_executable_mode_loss_are_refused(self):
        for mode_only in (False, True):
            with self.subTest(mode_only=mode_only), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                artifact = self.artifact(root)
                (root / 'changed').mkdir()
                changed = root / 'changed' / artifact.name
                with zipfile.ZipFile(artifact) as source, zipfile.ZipFile(changed, 'w') as destination:
                    for info in source.infolist():
                        data = source.read(info)
                        if info.filename.endswith('/mag-installer'):
                            if mode_only: info.external_attr = 0o100644 << 16
                            else: data = b'changed executable'
                        destination.writestr(info, data)
                with self.assertRaises(InstallerError): verify_artifact(changed, ROOT)

    def test_archive_traversal_and_unlisted_member_are_refused(self):
        for name in ('../escape', 'extra.bin'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                artifact = self.artifact(root)
                with zipfile.ZipFile(artifact, 'a') as archive: archive.writestr(name, b'bad')
                with self.assertRaises(InstallerError): verify_artifact(artifact, ROOT)

    def test_exact_preview_set_metadata_and_release_refusal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for target in ('windows-x86_64', 'macos-arm64', 'linux-x86_64'): self.artifact(root, target)
            directory = root / 'assets'
            with self.assertRaises(InstallerError): verify_set(directory, ROOT, 'preview-test', REVISION)
            manifest = verify_set(directory, ROOT, 'preview-test', REVISION, preview=True)
            self.assertEqual(manifest['kind'], 'preview')
            self.assertEqual(len(manifest['files']), 7)
            self.assertTrue((directory / 'SHA256SUMS.txt').is_file())
            self.assertEqual(verify_set(directory, ROOT, 'preview-test', REVISION, preview=True), manifest)
            (directory / 'unexpected.log').write_text('unexpected')
            with self.assertRaises(InstallerError): verify_set(directory, ROOT, 'preview-test', REVISION, preview=True)

    def test_set_refuses_missing_target_wrong_tag_revision_and_altered_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for target in ('windows-x86_64', 'macos-arm64', 'linux-x86_64'): self.artifact(root, target)
            directory = root / 'assets'
            for version, revision in [('wrong-tag', REVISION), ('preview-test', 'b' * 40)]:
                with self.subTest(version=version, revision=revision), self.assertRaises(InstallerError):
                    verify_set(directory, ROOT, version, revision, preview=True)
            verify_set(directory, ROOT, 'preview-test', REVISION, preview=True)
            (directory / 'RELEASE-ASSETS.json').write_text('{}')
            with self.assertRaises(InstallerError): verify_set(directory, ROOT, 'preview-test', REVISION, preview=True)

    def test_unsafe_version_and_foreign_target_names_are_refused(self):
        for version in ('../escape', '/absolute', 'x\\y', '.', 'x:y'):
            with self.subTest(version=version), self.assertRaises(InstallerError): artifact_name(version, 'windows-x86_64', True)
        for target in ('windows-arm64', 'linux-arm64'):
            with self.subTest(target=target), self.assertRaises(InstallerError): artifact_name('v1', target, True)

    def test_build_refuses_foreign_or_blocked_public_target_without_starting_tools(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'output'
            with patch('mag_installer.releasing.subprocess.run', side_effect=AssertionError('tool execution is forbidden in this source test')):
                with patch('mag_installer.releasing.host_key', return_value='windows-arm64'):
                    with self.assertRaises(InstallerError): build(ROOT, output, 'preview-test', preview=True)
                with self.assertRaises(InstallerError): build(ROOT, output, 'v1', preview=False)
            self.assertFalse(output.exists())

    def test_binary_disguised_as_license_is_not_packaged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            payload = self.payload(root)
            (payload / 'licenses/unexpected.bin').write_bytes(b'\x00binary asset')
            with self.assertRaises(InstallerError):
                package_payload(payload, root / 'out', 'preview-test', 'macos-arm64', 'preview', REVISION, ROOT)
            self.assertEqual(list((root / 'out').glob('*.zip')), [])

    def test_empty_executable_with_rewritten_inventory_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact = self.artifact(root)
            with zipfile.ZipFile(artifact) as archive:
                members = [(info, archive.read(info)) for info in archive.infolist()]
            with zipfile.ZipFile(artifact, 'w') as archive:
                for info, data in members:
                    if info.filename.endswith('/mag-installer'): data = b''
                    if info.filename.endswith('/INVENTORY.json'):
                        inventory = json.loads(data)
                        inventory['files']['MAG Installer.app/Contents/MacOS/mag-installer']['size'] = 0
                        inventory['files']['MAG Installer.app/Contents/MacOS/mag-installer']['sha256'] = hashlib.sha256(b'').hexdigest()
                        data = json.dumps(inventory).encode()
                    archive.writestr(info, data)
            with self.assertRaises(InstallerError): verify_artifact(artifact, ROOT)

    def test_frozen_resource_lookup_uses_bundle_resources_not_cwd(self):
        import mag_installer.__main__ as cli
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch('sys.frozen', True, create=True), patch('sys._MEIPASS', str(root), create=True):
                self.assertEqual(cli.resource_root(), root / 'resources')


if __name__ == '__main__': unittest.main()
