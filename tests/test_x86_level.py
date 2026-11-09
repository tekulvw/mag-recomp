"""MAG_X86_LEVEL (cmake/MagX86Level.cmake): x86-64-v3/v4 code generation for Linux and Windows builds.

A fixture project (tests/fixtures/x86_level) holds one unit of every class the real build compiles,
with the same per-class options. Its compile_commands.json shows what each level adds:

  * baseline gives exactly the commands of a build without the module;
  * v3 (v4) adds -march=x86-64-v3 (-v4) and -ffp-contract=off to every unit, and nothing else;
  * a floating-point program built with the v3 options rounds a*b + c twice, never fused.

Synthetic: no game files. The x86 checks run on x86-64 Linux and Windows hosts and skip elsewhere,
where the level checks that the option changes nothing instead. With MAG_TEST_SDK naming a pinned
runtime checkout, the real runtime's units are checked too.
"""
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/x86_level'
X86_HOST = sys.platform in ('linux', 'win32') and platform.machine().lower() in ('x86_64', 'amd64')
TOOLS = shutil.which('cmake') and shutil.which('ninja')


def v3_flags(entry_tokens, level='v3'):
    """The two options v3 (or v4) adds, in this compiler's spelling."""
    if any(token.startswith('/clang:') for token in entry_tokens) or any(token == '/nologo' for token in entry_tokens):
        return ('/clang:-march=x86-64-' + level, '/clang:-ffp-contract=off')
    return ('-march=x86-64-' + level, '-ffp-contract=off')


@unittest.skipUnless(TOOLS, 'cmake and ninja are required')
class X86LevelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mag-x86-level-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def configure(self, name, *options, expect_failure=False):
        build = self.tmp / name
        result = subprocess.run(['cmake', '-S', str(FIXTURE), '-B', str(build), '-G', 'Ninja',
                                 '-DCMAKE_BUILD_TYPE=Release', f'-DMAG_REPO={ROOT.as_posix()}', *options],
                                capture_output=True, text=True, timeout=300)
        if expect_failure:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return build

    def commands(self, build):
        """{source file: argv} with the build directory replaced, so two builds compare."""
        commands = {}
        for entry in json.loads((build / 'compile_commands.json').read_text()):
            argv = entry.get('arguments') or shlex.split(entry['command'], posix=os.name != 'nt')
            commands[Path(entry['file']).name] = [token.replace(build.as_posix(), '<build>').replace(str(build), '<build>')
                                                  for token in argv]
        return commands

    def level(self, build):
        return (build / 'x86-level.txt').read_text()

    def test_unknown_level_is_rejected(self):
        result = self.configure('bad', '-DMAG_X86_LEVEL=v5', expect_failure=True)
        self.assertIn('MAG_X86_LEVEL must be auto, v4, v3 or baseline', result.stderr)

    @unittest.skipIf(X86_HOST, 'checks hosts where the option does not apply')
    def test_other_hosts_are_unchanged(self):
        old = self.commands(self.configure('old', '-DMAG_FIXTURE_WITHOUT_MODULE=ON'))
        for level in ('auto', 'baseline'):
            build = self.configure(level, f'-DMAG_X86_LEVEL={level}')
            self.assertEqual(self.commands(build), old)
            self.assertEqual(self.level(build), '')
        for level in ('v3', 'v4'):
            result = self.configure(level, f'-DMAG_X86_LEVEL={level}', expect_failure=True)
            self.assertIn('applies only to x86-64 Linux and Windows builds', result.stderr)

    @unittest.skipUnless(X86_HOST, 'x86-64 Linux or Windows host required')
    def test_baseline_keeps_the_old_flags(self):
        old = self.commands(self.configure('old', '-DMAG_FIXTURE_WITHOUT_MODULE=ON'))
        build = self.configure('baseline', '-DMAG_X86_LEVEL=baseline')
        self.assertEqual(self.level(build), 'baseline')
        self.assertEqual(self.commands(build), old)
        self.assertEqual(set(old), {'runtime.c', 'mag_source.cpp', 'lifted_ppu.cpp', 'lifted_spu.c', 'fp_contract.c'})

    @unittest.skipUnless(X86_HOST, 'x86-64 Linux or Windows host required')
    def test_v3_and_v4_add_both_flags_to_every_target_class(self):
        old = self.commands(self.configure('baseline', '-DMAG_X86_LEVEL=baseline'))
        for level in ('v3', 'v4'):
            build = self.configure(level, f'-DMAG_X86_LEVEL={level}')
            self.assertEqual(self.level(build), level)
            new = self.commands(build)
            self.assertEqual(set(new), set(old))
            for name, argv in new.items():
                flags = v3_flags(argv, level)
                for flag in flags:
                    self.assertIn(flag, argv, f'{level} {name}: {flag} missing')
                # Apart from the two options, every command is the baseline one.
                self.assertEqual([t for t in argv if t not in flags], [t for t in old[name] if t not in flags], name)
            # The level comes before the per-source options, so a lifted unit's own -O level still wins.
            for name in ('lifted_ppu.cpp', 'lifted_spu.c'):
                argv = new[name]
                last_opt = max(i for i, t in enumerate(argv) if t[:2] in ('-O', '/O'))
                self.assertLess(argv.index(v3_flags(argv, level)[0]), last_opt, name)

    @unittest.skipUnless(X86_HOST, 'x86-64 Linux or Windows host required')
    def test_auto_follows_the_build_cpu(self):
        build = self.configure('auto')
        level = self.level(build)
        self.assertIn(level, ('v4', 'v3', 'baseline'))
        expected = self.commands(self.configure('explicit', f'-DMAG_X86_LEVEL={level}'))
        self.assertEqual(self.commands(build), expected)

    @unittest.skipUnless(X86_HOST, 'x86-64 Linux or Windows host required')
    def test_v3_and_v4_never_fuse_a_multiply_and_add(self):
        auto = self.level(self.configure('probe'))
        if auto not in ('v3', 'v4'):
            self.skipTest('the build CPU lacks x86-64-v3, so v3 code cannot run here')
        program = 'fp_contract.exe' if os.name == 'nt' else 'fp_contract'

        def run(name, *options, level='v3'):
            build = self.configure(name, f'-DMAG_X86_LEVEL={level}', *options)
            subprocess.run(['cmake', '--build', str(build), '--target', 'fp_contract'],
                           check=True, capture_output=True, text=True, timeout=300)
            return subprocess.run([str(build / program)], capture_output=True, text=True, timeout=30)

        for level in ('v3', 'v4') if auto == 'v4' else ('v3',):
            result = run(level, level=level)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('fused', result.stdout)
        # The control proves the program can see a fused result: the same build with contraction
        # forced on fuses at least the single expression.
        control = run('control', '-DMAG_FIXTURE_CONTRACT_CONTROL=ON')
        self.assertIn('double-expression fused', control.stdout)

    @unittest.skipUnless(X86_HOST and os.environ.get('MAG_TEST_SDK'), 'x86-64 host and MAG_TEST_SDK required')
    def test_v3_reaches_every_real_runtime_unit(self):
        sdk = Path(os.environ['MAG_TEST_SDK']).resolve()
        if os.name != 'nt' and not self.runtime_finds_sdl2():
            self.skipTest('SDL2 development files not found: the runtime requires them to configure off Windows')
        build = self.configure('sdk', '-DMAG_X86_LEVEL=v3', f'-DMAG_FIXTURE_SDK={sdk.as_posix()}')
        runtime = 0
        for entry in json.loads((build / 'compile_commands.json').read_text()):
            argv = entry.get('arguments') or shlex.split(entry['command'], posix=os.name != 'nt')
            if not Path(entry['file']).resolve().is_relative_to(sdk):
                continue
            runtime += 1
            for flag in v3_flags(argv):
                self.assertIn(flag, argv, entry['file'])
        self.assertGreater(runtime, 50)


    def runtime_finds_sdl2(self):
        """The runtime's own SDL2 lookup (find_package(SDL2 QUIET)), alone, so a missing host
        dependency skips the real-runtime check instead of failing it; any other configure
        failure still fails."""
        probe = self.tmp / 'sdl2-probe'
        probe.mkdir()
        (probe / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(sdl2_probe C)\nfind_package(SDL2 QUIET)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/sdl2-found.txt" "${SDL2_FOUND}")\n')
        result = subprocess.run(['cmake', '-S', str(probe), '-B', str(probe / 'build'), '-G', 'Ninja'],
                                capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (probe / 'build/sdl2-found.txt').read_text().upper() in ('1', 'ON', 'TRUE', 'YES')


class X86LevelWiringTests(unittest.TestCase):
    """The real build applies the level before any target exists and records it."""

    def test_the_level_is_applied_before_the_runtime_and_the_executable(self):
        text = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MagX86Level.cmake")', text)
        apply = text.index('\nmag_x86_level_apply()')
        self.assertLess(apply, text.index('add_subdirectory("${PS3RECOMP_DIR}"'))
        self.assertLess(apply, text.index('add_executable(${PROJECT_NAME}'))
        self.assertEqual(text.count('add_executable('), 1)
        self.assertEqual(text.count('add_library('), 0)

    def test_the_receipt_records_the_level(self):
        text = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('--x86-level "${MAG_X86_LEVEL_SELECTED}"', text)


if __name__ == '__main__':
    unittest.main()
