"""Synthetic regressions for Conventional Commit and SDK PR title policy."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.check_pr_title import validate_title

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / 'release-please-config.json').read_text())


class PRTitleTests(unittest.TestCase):
    def check(self, title, paths, accepted):
        errors = validate_title(title, paths, CONFIG)
        self.assertEqual(not errors, accepted, errors)

    def test_missed_runtime_pin_release_regression(self):
        self.check('build: refresh reviewed runtime pin and coherence defaults',
                   ['profiles/framework.json'], False)

    def test_sdk_releasing_types_and_breaking_marker(self):
        for title in ['fix(sdk): refresh reviewed runtime pin',
                      'perf(sdk): improve coherence defaults',
                      'feat(runtime): support the new renderer',
                      'fix!: require the new runtime']:
            with self.subTest(title=title):
                self.check(title, ['profiles/framework.json'], True)

    def test_non_releasing_sdk_types_rejected(self):
        for kind in ['build', 'chore', 'ci', 'docs', 'refactor', 'test', 'build!']:
            with self.subTest(kind=kind):
                self.check(f'{kind}: update runtime pin',
                           ['profiles/framework.json'], False)

    def test_ordinary_prs_and_release_bot_title(self):
        for title, paths in [
            ('docs: explain the runtime pin', ['docs/sdk.md']),
            ('ci: refresh test jobs', ['.github/workflows/tests.yml']),
            ('chore: tidy tooling', ['tools/helper.py']),
            ('chore(main): release 0.5.1', ['CHANGELOG.md', '.release-please-manifest.json']),
            ('style: format code', ['tools/helper.py']),
            ('revert: undo the previous change', ['tools/helper.py']),
        ]:
            with self.subTest(title=title):
                self.check(title, paths, True)

    def test_invalid_titles_rejected(self):
        for title in ['Update runtime', 'Fix: runtime', 'unknown: runtime',
                      'fix: ', 'fix(): runtime', 'fix(sdk) : runtime',
                      'fix: runtime\nBREAKING CHANGE: old runtime removed',
                      ' fix: runtime', 'fix: runtime\x00']:
            with self.subTest(title=repr(title)):
                self.check(title, ['README.md'], False)

    def test_types_come_from_release_configuration(self):
        config = {'changelog-sections': [{'type': 'custom'}]}
        self.assertFalse(validate_title('custom: explain change', ['README.md'], config))
        self.assertTrue(validate_title('fix: explain change', ['README.md'], config))

    def run_cli(self, data):
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / 'pr.json'
            payload.write_text(json.dumps(data))
            return subprocess.run([sys.executable, str(ROOT / 'tools/check_pr_title.py'),
                                   '--input', str(payload)], cwd=ROOT,
                                  text=True, capture_output=True)

    def test_cli_treats_title_as_data(self):
        result = self.run_cli({'title': 'fix(sdk): handle $(echo harmless) `echo text`',
                               'files': [{'filename': 'profiles/framework.json'}],
                               'changed_files': 1})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_cli_checks_previous_path_for_sdk_rename(self):
        result = self.run_cli({'title': 'build: relocate the SDK profile',
                               'files': [{'filename': 'profiles/new.json',
                                          'previous_filename': 'profiles/framework.json'}],
                               'changed_files': 1})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SDK', result.stderr)

    def test_cli_refuses_incomplete_or_malformed_file_list(self):
        for data in [
            {'title': 'build: update tools', 'files': [], 'changed_files': 101},
            {'title': 'fix: update tools', 'files': [], 'changed_files': 3001},
            {'title': 'fix: update tools', 'files': [{}], 'changed_files': 1},
            {'title': 'fix: update tools', 'files': [{'filename': 42}], 'changed_files': 1},
        ]:
            with self.subTest(data=data):
                result = self.run_cli(data)
                self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
