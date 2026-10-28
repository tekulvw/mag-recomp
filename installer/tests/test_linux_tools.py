"""Linux bootstrap tests use fixtures only; never install host packages."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from mag_installer import managed_tools as tools
from mag_installer.model import InstallerError
import test_native_recipe as recipe_tests
from mag_installer.native_recipe import load_config, build_plan


class LinuxToolsTests(unittest.TestCase):
    def test_official_complete_linux_lock(self):
        self.assertEqual(set(tools.LINUX_LOCK), set(tools.LOCK))
        for key, spec in tools.LINUX_LOCK.items():
            self.assertEqual(len(spec['sha256']), 64)
            self.assertGreater(spec['size'], 0)
            self.assertTrue(spec['url'].startswith(('https://github.com/', 'https://files.pythonhosted.org/', 'https://ffmpeg.org/')))
        self.assertIn('cp39-abi3-manylinux', tools.LINUX_LOCK['wheel-pycryptodome']['url'].replace('https://files.pythonhosted.org/', 'https://files.pythonhosted.org/'))

    def test_existing_toolchain_never_invokes_install(self):
        toolchain = {'cc': '/usr/bin/clang', 'cxx': '/usr/bin/clang++', 'git': '/usr/bin/git'}
        with patch.object(tools, 'linux_platform_probe', return_value=(True, '')), patch.object(tools, 'linux_toolchain_probe', return_value=(toolchain, '')), patch.object(tools, 'run_command') as run:
            self.assertEqual(tools.ensure_linux_build_tools(), toolchain)
            run.assert_not_called()

    def test_install_is_fixed_policykit_argv_and_rechecks(self):
        toolchain = {'cc': '/usr/bin/clang', 'cxx': '/usr/bin/clang++', 'git': '/usr/bin/git'}
        with tempfile.TemporaryDirectory() as tmp, patch.object(tools, 'linux_platform_probe', return_value=(True, '')), patch.object(tools, 'linux_toolchain_probe', side_effect=[(None, 'missing'), (toolchain, '')]), patch.object(Path, 'is_file', return_value=True), patch.object(tools, 'run_command') as run:
            self.assertEqual(tools.ensure_linux_build_tools(cache=tmp), toolchain)
            self.assertEqual(run.call_args.args[0], ['/usr/bin/pkexec', '/usr/bin/apt-get', 'install', '--yes', '--no-install-recommends', *tools.LINUX_PACKAGES])
            self.assertIn('libssl-dev', tools.LINUX_PACKAGES)

    def test_unsupported_distribution_cannot_install(self):
        with patch.object(tools, 'linux_platform_probe', return_value=(False, 'Unsupported distribution')), patch.object(tools, 'run_command') as run:
            with self.assertRaisesRegex(InstallerError, 'Unsupported'): tools.ensure_linux_build_tools()
            run.assert_not_called()

    def test_linux_dependency_plan_and_outputs(self):
        paths = {key: '/private/' + key for key in ('cmake', 'ninja', 'source-sdl2', 'source-spirv-cross', 'source-glslang', 'source-ffmpeg')}
        paths['host'] = 'linux-x86_64'
        plan = tools.dependency_plan(paths, '/private/prefix')
        configure = next(s['argv'] for s in plan if s['id'] == 'configure-ffmpeg')
        self.assertIn('--disable-x86asm', configure)
        for stage in plan:
            self.assertFalse(any('xcrun' in a or 'OSX' in a for a in stage['argv']))
        libs = [str(p) for p in tools._dependency_paths(Path('/private/prefix'), linux=True)]
        self.assertTrue(any(p.endswith('libavcodec.so.63') for p in libs))
        self.assertFalse(any('.dylib' in p for p in libs))

    def test_linux_bootstrap_uses_linux_artifacts_and_environment(self):
        toolchain = {'cc': '/usr/bin/clang', 'cxx': '/usr/bin/clang++', 'git': '/usr/bin/git'}
        with tempfile.TemporaryDirectory() as tmp, patch.object(tools, 'host_key', return_value='linux-x86_64'), patch.object(tools, 'linux_toolchain_probe', return_value=(toolchain, '')), patch.object(tools, 'materialize', side_effect=lambda spec, *args: Path(tmp) / spec['id']) as materialize, patch.object(tools, '_install_wheels'), patch.object(tools, 'run_command') as run:
            managed = tools.bootstrap(Path(tmp) / 'managed', build_dependencies=False)
            self.assertEqual(managed['linux_toolchain'], {k: toolchain[k] for k in ('cc', 'cxx')})
            self.assertEqual(managed['host'], 'linux-x86_64')
            self.assertTrue(managed['ffmpeg']['avcodec'].endswith('libavcodec.so.63'))
            self.assertFalse(managed['dependencies_ready'])
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[4]['CC'], '/usr/bin/clang')
            self.assertTrue(all(not call.args[0]['id'].startswith('source-') for call in materialize.call_args_list))

    def test_linux_recipe_uses_explicit_compilers(self):
        fixture = recipe_tests.NativeRecipeTests()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = fixture.config(root)
            raw.update(host='linux-x86_64', linux_toolchain={'cc': str(root / 'cmake'), 'cxx': str(root / 'ninja')})
            (root / 'config.json').write_text(json.dumps(raw))
            config = load_config(root / 'config.json')
            workspace = fixture.workspace(root / 'work')
            plan = build_plan(config, workspace)
            argv = next(s['argv'] for s in plan if s['id'] == 'configure-engine')
            self.assertIn('-DCMAKE_C_COMPILER=' + str((root / 'cmake').resolve()), argv)
            self.assertIn('-DCMAKE_CXX_COMPILER=' + str((root / 'ninja').resolve()), argv)
            self.assertFalse(any('OSX' in a for a in argv))
            self.assertIn('-DPS3RECOMP_RSX_VULKAN=ON', argv)
            self.assertIn('-DPS3RECOMP_FFMPEG_RUNTIME=ON', argv)


if __name__ == '__main__': unittest.main()
