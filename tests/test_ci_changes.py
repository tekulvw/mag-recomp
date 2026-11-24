"""Synthetic checks for the pull request job filter (tools/ci_changes.py)."""
import fnmatch
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.ci_changes import FILTERS, FLAGS, classify

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / '.github/workflows'


def ran(files, **kwargs):
    return {flag for flag, on in classify(files, **kwargs).items() if on}


class ClassifyTests(unittest.TestCase):
    def test_documentation_runs_no_filtered_job(self):
        self.assertEqual(ran(['docs/player-settings.md', 'README.md', 'crates/mag-render-abi/README.md',
                              'docs/development/workflow.md']), set())

    def test_documents_read_by_tests_run_python_only(self):
        self.assertEqual(ran(['BUILDING.md']), {'python'})
        self.assertEqual(ran(['docs/ps3recomp-flags.md', 'docs/native-controls.md']), {'python'})

    def test_installer_markdown_still_builds_installers(self):
        # The installer distribution ships its own README and docs.
        self.assertEqual(ran(['installer/docs/testing.md']),
                         {'installers', 'installer_toolchain'})
        self.assertIn('installers', ran(['NOTICE.md']))

    def test_controls_only_cpp_skips_rust_and_shader_builds(self):
        result = ran(['src/game_controls/mag_game_controls_hooks.cpp', 'tests/test_game_controls_hooks.py',
                      'docs/native-controls.md'])
        self.assertEqual(result, {'python', 'windows_native', 'installers'})

    def test_render_crate_change(self):
        self.assertEqual(ran(['crates/mag-render-native/native/mag_render_vulkan.cpp']),
                         {'python', 'render'})
        self.assertEqual(ran(['shaders/render_frontend/scene_vertex.wgsl']), {'python', 'render'})

    def test_render_boundary_sources_run_render_and_installers(self):
        result = ran(['src/mag_render_host_bridge.cpp'])
        self.assertTrue({'render', 'installers', 'python', 'windows_native'} <= result)
        self.assertIn('render', ran(['src/render_adapter/mag_render_update_loop.cpp']))
        self.assertIn('render', ran(['tools/check_render_frontend_native.py']))
        self.assertIn('render', ran(['cmake/MagRender.cmake']))

    def test_player_render_scale_is_not_the_frontend(self):
        self.assertNotIn('render', ran(['cmake/MagRenderScale.cmake', 'src/player_render_scale.cpp',
                                        'tests/test_player_render_scale_vulkan.py']))

    def test_installer_only_change(self):
        self.assertEqual(ran(['installer/mag_installer/native_build.py']),
                         {'python', 'installers', 'installer_toolchain'})
        self.assertEqual(ran(['.github/scripts/linux_native_check.py']),
                         {'python', 'installers', 'installer_toolchain'})

    def test_source_change_skips_linux_toolchain_build(self):
        self.assertNotIn('installer_toolchain', ran(['src/player_portal.cpp']))
        self.assertIn('installer_toolchain', ran(['src/player_portal.cpp', 'profiles/framework.json']))

    def test_input_crate(self):
        result = ran(['crates/mag-input/src/lib.rs'])
        self.assertTrue({'rust_input', 'windows_native', 'installers', 'python'} <= result)
        self.assertNotIn('render', result)

    def test_macos_settings_sources(self):
        self.assertIn('macos_settings', ran(['src/player_settings_macos.m']))
        self.assertIn('macos_settings', ran(['tests/test_player_reports.py']))
        self.assertNotIn('macos_settings', ran(['src/main.cpp']))

    def test_each_workflow_file_runs_its_own_jobs(self):
        self.assertTrue({'rust_input', 'windows_native', 'macos_settings', 'python'}
                        <= ran(['.github/workflows/tests.yml']))
        self.assertIn('render', ran(['.github/workflows/render-frontend.yml']))
        self.assertTrue({'installers', 'installer_toolchain'} <= ran(['.github/workflows/installers.yml']))

    def test_shared_actions_run_every_runner_job(self):
        result = ran(['.github/actions/self-hosted-linux/action.yml'])
        self.assertTrue({'python', 'rust_input', 'render', 'installers', 'installer_toolchain'} <= result)

    def test_everything_runs_outside_ordinary_pull_requests(self):
        everything = set(FLAGS)
        for event in ('push', 'workflow_dispatch', 'workflow_call', 'schedule'):
            self.assertEqual(ran(['docs/a.md'], event=event), everything)
        self.assertEqual(ran(['CHANGELOG.md', '.release-please-manifest.json'],
                             head_ref='release-please--branches--main'), everything)

    def test_unknown_or_empty_diff_runs_everything(self):
        self.assertEqual(ran(None), set(FLAGS))
        self.assertEqual(ran([]), set(FLAGS))
        self.assertEqual(ran(['', '  ']), set(FLAGS))

    def test_changing_the_filter_runs_everything(self):
        self.assertEqual(ran(['tools/ci_changes.py']), set(FLAGS))


class RepositoryTests(unittest.TestCase):
    def test_every_pattern_still_names_a_tracked_path(self):
        if not shutil.which('git') or not (ROOT / '.git').exists():
            self.skipTest('needs a Git checkout')
        tracked = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True,
                                 check=True).stdout.splitlines()
        for flag, patterns in FILTERS.items():
            for pattern in patterns:
                self.assertTrue(any(fnmatch.fnmatchcase(path, pattern) for path in tracked),
                                f'{flag}: {pattern} matches no tracked path; update tools/ci_changes.py')

    def test_workflows_use_only_declared_outputs(self):
        for name in ('tests.yml', 'render-frontend.yml', 'installers.yml'):
            text = (WORKFLOWS / name).read_text()
            self.assertIn('tools/ci_changes.py', text, name)
            used = set(re.findall(r'needs\.changes\.outputs\.(\w+)', text))
            declared = set(re.findall(r'^\s+(\w+): \$\{\{ steps\.diff\.outputs\.(\w+) \}\}', text, re.M))
            self.assertTrue(used, name)
            for flag in used:
                self.assertIn(flag, FLAGS, f'{name}: {flag}')
                self.assertIn((flag, flag), declared, f'{name}: {flag} is not a changes output')


class CommandLineTests(unittest.TestCase):
    def run_cli(self, files, event='pull_request', head_ref=''):
        with tempfile.TemporaryDirectory() as temporary:
            listing = Path(temporary) / 'files.txt'
            listing.write_text('\n'.join(files) + '\n')
            output = Path(temporary) / 'output'
            env = dict(os.environ, GITHUB_OUTPUT=str(output))
            subprocess.run([sys.executable, str(ROOT / 'tools/ci_changes.py'), '--event', event,
                            '--head-ref', head_ref, '--files-from', str(listing)],
                           env=env, check=True, capture_output=True, text=True)
            return dict(line.split('=', 1) for line in output.read_text().splitlines())

    def test_writes_every_flag(self):
        values = self.run_cli(['docs/a.md'])
        self.assertEqual(set(values), set(FLAGS))
        self.assertEqual(set(values.values()), {'false'})
        self.assertEqual(self.run_cli(['crates/mag-render/src/lib.rs'])['render'], 'true')
        self.assertEqual(set(self.run_cli(['docs/a.md'], event='push').values()), {'true'})

    def test_git_diff_of_a_synthetic_repository(self):
        if not shutil.which('git'):
            self.skipTest('needs git')
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            git = ['git', '-c', 'user.name=t', '-c', 'user.email=t@example.invalid', '-c',
                   'commit.gpgsign=false']
            subprocess.run(['git', 'init', '-q'], cwd=repo, check=True)
            (repo / 'docs').mkdir()
            (repo / 'docs/a.md').write_text('a\n')
            subprocess.run(git + ['add', '.'], cwd=repo, check=True)
            subprocess.run(git + ['commit', '-qm', 'base'], cwd=repo, check=True)
            (repo / 'docs/a.md').rename(repo / 'docs/b.md')
            (repo / 'shaders').mkdir()
            (repo / 'shaders/x.wgsl').write_text('x\n')
            subprocess.run(git + ['add', '-A'], cwd=repo, check=True)
            subprocess.run(git + ['commit', '-qm', 'head'], cwd=repo, check=True)
            output = repo / 'out'
            env = dict(os.environ, GITHUB_OUTPUT=str(output))
            subprocess.run([sys.executable, str(ROOT / 'tools/ci_changes.py'), '--event', 'pull_request'],
                           cwd=repo, env=env, check=True, capture_output=True, text=True)
            values = dict(line.split('=', 1) for line in output.read_text().splitlines())
            self.assertEqual(values['render'], 'true')
            self.assertEqual(values['installers'], 'false')


if __name__ == '__main__':
    unittest.main()
