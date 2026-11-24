"""The per-user build cache: keys, stored entries, eviction, clearing, and the compile cache's reports.

Synthetic only: trees, logs and "compilers" are made up here."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from mag_installer import build_cache, compile_cache
from mag_installer.build_cache import BuildCache, GIB
from mag_installer.model import InstallerError


def tree(root, files):
    for name, data in files.items():
        path = Path(root) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
    return Path(root)


class KeyTests(unittest.TestCase):
    def test_keys_change_with_every_part_and_not_with_order(self):
        base = {'recipe': ['cmake', '-DX=1'], 'toolchain': {'clang': 'abc'}, 'host': 'macos-arm64'}
        key = build_cache.digest(base)
        self.assertEqual(key, build_cache.digest(dict(reversed(list(base.items())))))
        for change in ({'recipe': ['cmake', '-DX=2']}, {'toolchain': {'clang': 'abd'}}, {'host': 'linux-x86_64'}):
            self.assertNotEqual(key, build_cache.digest({**base, **change}))

    def test_switch_and_sizes(self):
        self.assertTrue(build_cache.enabled({}))
        self.assertTrue(build_cache.enabled({'MAG_BUILD_CACHE': '1'}))
        self.assertFalse(build_cache.enabled({'MAG_BUILD_CACHE': '0'}))
        with self.assertRaisesRegex(InstallerError, 'must be 0 or 1'):
            build_cache.enabled({'MAG_BUILD_CACHE': 'off'})
        self.assertEqual(build_cache.parse_size('20'), 20 * GIB)
        self.assertEqual(build_cache.parse_size('15GiB'), 15 * GIB)
        self.assertEqual(build_cache.parse_size('10GB'), 10 * 1000 ** 3)
        for bad in ('1GiB', 'lots', '-3G', '5 parsecs'):
            with self.assertRaises(InstallerError):
                build_cache.parse_size(bad)


class EntryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.temp = Path(temp.name).resolve()
        self.events = []
        self.cache = BuildCache(self.temp / 'cache/build-cache', events=self.events.append)

    def test_store_and_restore_round_trip_with_links_modes_and_shared_files(self):
        old = self.temp / 'old-build/prefix'
        tree(old, {'lib/libfoo.so.1': b'\x7fELF\0' + str(old).encode(), 'include/foo.h': b'int foo;\n',
                   'bin/foo-config': f'#!/bin/sh\necho {old}/include\n', 'share/same.txt': b'int foo;\n'})
        (old / 'bin/foo-config').chmod(0o755)
        os.symlink('libfoo.so.1', old / 'lib/libfoo.so')
        key = build_cache.digest({'n': 1})
        record = self.cache.store('tools', key, old, ['lib', 'include', 'bin', 'share'], {'n': 1}, 12.5,
                                  replacements={'prefix': str(old)})
        self.assertEqual(record['files']['lib/libfoo.so'], {'link': 'libfoo.so.1'})
        self.assertTrue(record['files']['bin/foo-config']['template'])
        # Binary files keep any path they hold; identical files are stored once.
        self.assertNotIn('template', record['files']['lib/libfoo.so.1'])
        self.assertEqual(record['files']['include/foo.h']['sha256'], record['files']['share/same.txt']['sha256'])
        new = self.temp / 'new-build/prefix'
        restored = self.cache.restore('tools', key, new, replacements={'prefix': str(new)})
        self.assertEqual(restored['seconds'], 12.5)
        self.assertEqual((new / 'bin/foo-config').read_text(), f'#!/bin/sh\necho {new}/include\n')
        self.assertTrue(os.access(new / 'bin/foo-config', os.X_OK))
        self.assertEqual((new / 'lib/libfoo.so.1').read_bytes(), b'\x7fELF\0' + str(old).encode())
        self.assertEqual(os.readlink(new / 'lib/libfoo.so'), 'libfoo.so.1')
        self.assertEqual((new / 'include/foo.h').read_bytes(), b'int foo;\n')
        self.assertIsNone(self.cache.restore('tools', build_cache.digest({'n': 2}), self.temp / 'other'))
        self.assertFalse((self.temp / 'other').exists())

    def test_json_and_forward_slash_spellings_of_the_build_folder_are_replaced(self):
        old = self.temp / 'old'
        windows = 'C:\\Games\\Player\\MAG-build'
        tree(old, {'provenance.json': json.dumps({'command': [windows + '\\python.exe', 'C:/Games/Player/MAG-build/ws']})})
        key = build_cache.digest({'json': 1})
        self.cache.store('lift', key, old, ['provenance.json'], {}, 1, replacements={'build': windows})
        new = self.temp / 'new'
        self.cache.restore('lift', key, new, replacements={'build': 'D:\\MAG2-build'})
        self.assertEqual(json.loads((new / 'provenance.json').read_text()),
                         {'command': ['D:\\MAG2-build\\python.exe', 'D:/MAG2-build/ws']})

    def test_damaged_entries_are_removed_and_miss(self):
        source = tree(self.temp / 'src', {'a.txt': b'alpha', 'b/c.txt': b'gamma'})
        key = build_cache.digest({'damaged': 1})
        record = self.cache.store('lift', key, source, ['a.txt', 'b'], {}, 3)
        blob = self.cache.root / 'blobs' / record['files']['b/c.txt']['sha256'][:2] / record['files']['b/c.txt']['sha256']
        blob.chmod(0o644); blob.write_bytes(b'tampered')
        target = self.temp / 'target'
        self.assertIsNone(self.cache.restore('lift', key, target))
        self.assertFalse((target / 'a.txt').exists())   # partial restores are undone
        self.assertIsNone(self.cache.lookup('lift', key))
        self.assertEqual(self.events[-1]['event'], 'build-cache-damaged')

    def test_restore_refused_by_check_writes_nothing(self):
        source = tree(self.temp / 'src', {'a.txt': b'alpha'})
        key = build_cache.digest({'check': 1})
        self.cache.store('lift', key, source, ['a.txt'], {}, 3, extra={'reads': {'x': 'y'}})
        self.assertIsNone(self.cache.restore('lift', key, self.temp / 'out', check=lambda record: False))
        self.assertFalse((self.temp / 'out').exists())
        self.assertIsNotNone(self.cache.lookup('lift', key))

    def test_least_recently_used_entries_are_evicted_to_the_limit(self):
        megabyte = 1024 * 1024
        sizes = {}
        with patch.object(BuildCache, 'max_bytes', return_value=10 * megabyte), \
             patch.object(BuildCache, 'objects_max_bytes', return_value=4 * megabyte):
            for index in range(4):   # 2 MiB each; 6 MiB of room
                source = tree(self.temp / f'src{index}', {'payload.bin': os.urandom(2 * megabyte)})
                key = build_cache.digest({'index': index})
                sizes[index] = key
                self.cache.store('lift', key, source, ['payload.bin'], {}, index)
                if index == 1:
                    time.sleep(0.01)
                    self.assertIsNotNone(self.cache.restore('lift', sizes[0], self.temp / 'touch0'))   # 0 is now newer than 1
                time.sleep(0.01)
            kept = {index for index, key in sizes.items() if self.cache.lookup('lift', key)}
        self.assertEqual(kept, {0, 2, 3})
        self.assertEqual(self.events[-1]['event'], 'build-cache-evicted')
        # Files no entry needs are gone with their entry.
        blobs = list((self.cache.root / 'blobs').glob('*/*'))
        self.assertEqual(len(blobs), 3)

    def test_clear_keeps_settings_and_downloads_and_refuses_while_a_build_runs(self):
        downloads = self.temp / 'cache/abc.blob'
        downloads.parent.mkdir(parents=True); downloads.write_bytes(b'download')
        self.cache.set_max_bytes(5 * GIB)
        source = tree(self.temp / 'src', {'a.txt': b'alpha'})
        self.cache.store('lift', build_cache.digest({'c': 1}), source, ['a.txt'], {}, 1)
        (self.cache.objects_directory() / 'ab').mkdir()
        (self.cache.objects_directory() / 'ab' / 'object').write_bytes(b'x' * 100)
        usage = self.cache.usage()
        self.assertEqual((usage['lift_entries'], usage['max_bytes']), (1, 5 * GIB))
        self.assertGreater(usage['bytes'], 100)
        other = BuildCache(self.cache.root)
        with other.session():
            with self.assertRaisesRegex(InstallerError, 'build is using the build cache'):
                build_cache.clear(self.temp / 'cache')
        freed = build_cache.clear(self.temp / 'cache')
        self.assertGreater(freed, 100)
        self.assertTrue(downloads.is_file())
        self.assertEqual(self.cache.usage()['lift_entries'], 0)
        self.assertFalse((self.cache.objects_directory() / 'ab').exists())
        self.assertEqual(self.cache.max_bytes(), 5 * GIB)

    def test_a_session_of_a_build_that_ended_does_not_block_clearing(self):
        stale = self.cache.root / 'sessions' / '1-dead.lock'
        self.cache.objects_directory()
        stale.write_text('')
        build_cache.clear(self.temp / 'cache')
        self.assertFalse(stale.exists())

    def test_usage_of_a_missing_cache_creates_nothing(self):
        usage = build_cache.usage(self.temp / 'nothing')
        self.assertEqual((usage['bytes'], usage['max_bytes']), (0, build_cache.DEFAULT_MAX_BYTES))
        self.assertFalse((self.temp / 'nothing').exists())
        self.assertEqual(build_cache.clear(self.temp / 'nothing'), 0)


class ManagedPrefixTests(unittest.TestCase):
    """Managed dependency prefixes: built once, then restored for later builds elsewhere."""

    def test_prefix_is_built_once_and_restored_into_another_build_folder(self):
        from mag_installer import managed_tools
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp).resolve()
            cache = BuildCache(temp / 'user-cache/build-cache')
            identity = {'source-sdl2': 'a' * 64}
            built = []
            def prefix_for(name):
                prefix = temp / name / 'managed-tools/prefix'
                def run_stages():
                    built.append(name)
                    tree(prefix, {'lib/libSDL2.dylib': b'\0binary', 'lib/cmake/SDL2/SDL2Config.cmake': f'set(P "{prefix}")\n',
                                  'include/SDL2/SDL.h': b'/* SDL */\n'})
                return prefix, run_stages
            material = {'host': 'macos-arm64', 'toolchain': {'clang': 'x'}, 'recipe': [{'id': 'configure-sdl2'}]}
            events = []
            prefix, run = prefix_for('one')
            required = [prefix / 'include/SDL2/SDL.h']
            managed_tools._dependency_prefix(prefix, identity, [], required, run, cache.token, events.append, cache, material)
            self.assertEqual(built, ['one'])
            self.assertEqual(events[-1]['event'], 'build-cache-stored')
            again, run = prefix_for('two')
            hit = managed_tools._dependency_prefix(again, identity, [], [again / 'include/SDL2/SDL.h'], run, cache.token,
                                                   events.append, cache, material)
            self.assertEqual(built, ['one'])
            self.assertEqual(events[-1]['event'], 'build-cache-hit')
            self.assertIsNotNone(hit)
            self.assertEqual((again / 'lib/cmake/SDL2/SDL2Config.cmake').read_text(), f'set(P "{again}")\n')
            self.assertEqual(json.loads((again / 'MANAGED-DEPENDENCIES.json').read_text()), identity)
            # A Retry of the same build keeps its verified prefix.
            self.assertIsNone(managed_tools._dependency_prefix(again, identity, [], [], run, cache.token, events.append,
                                                               cache, material))
            # Another toolchain, recipe or pinned source is another entry.
            for change in ({'toolchain': {'clang': 'y'}}, {'recipe': [{'id': 'configure-sdl2', 'argv': ['-DX=1']}]}):
                other, run = prefix_for('three-' + str(len(built)))
                managed_tools._dependency_prefix(other, identity, [], [], run, cache.token, events.append, cache,
                                                 {**material, **change})
            other, run = prefix_for('four')
            managed_tools._dependency_prefix(other, {'source-sdl2': 'b' * 64}, [], [], run, cache.token, events.append, cache, material)
            self.assertEqual(len(built), 4)

    def test_macos_ffmpeg_libraries_use_rpath_install_names(self):
        from mag_installer.managed_tools import dependency_plan
        paths = {'cmake': '/m/cmake', 'ninja': '/m/ninja', **{f'source-{k}': f'/m/{k}' for k in ('sdl2', 'spirv-cross', 'glslang', 'ffmpeg')}}
        configure = next(s for s in dependency_plan(paths, Path('/m/prefix')) if s['id'] == 'configure-ffmpeg')
        self.assertIn('--install-name-dir=@rpath', configure['argv'])
        configure = next(s for s in dependency_plan({**paths, 'host': 'linux-x86_64'}, Path('/m/prefix')) if s['id'] == 'configure-ffmpeg')
        self.assertNotIn('--install-name-dir=@rpath', configure['argv'])


class CompileCacheTests(unittest.TestCase):
    def test_launchers_for_each_host_language_and_none_without_the_cache(self):
        config = {'build_cache': {'root': '/c', 'ccache': '/t/ccache', 'base_dir': '/b'}}
        self.assertEqual(compile_cache.cmake_arguments(config, str),
                         [f'-DCMAKE_{lang}_COMPILER_LAUNCHER=/t/ccache' for lang in ('C', 'CXX', 'OBJC', 'OBJCXX')])
        self.assertEqual(compile_cache.cmake_arguments({**config, 'host': 'linux-x86_64'}, str),
                         ['-DCMAKE_C_COMPILER_LAUNCHER=/t/ccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=/t/ccache'])
        self.assertEqual(compile_cache.cmake_arguments({}, str), [])
        self.assertEqual(compile_cache.cmake_arguments({'build_cache': {'root': '/c', 'base_dir': '/b'}}, str), [])

    def test_pins_cover_every_host_with_official_release_assets(self):
        for host, spec in compile_cache.CCACHE_LOCKS.items():
            self.assertTrue(spec['url'].startswith('https://github.com/ccache/ccache/releases/download/v' + compile_cache.CCACHE_VERSION + '/'))
            self.assertRegex(spec['sha256'], '^[0-9a-f]{64}$')
            self.assertTrue(spec['executable'].startswith(Path(spec['url']).name.split('.tar')[0].split('.zip')[0] + '/'))
        # Windows builds do not use the compile cache: clang-cl's /clang: options defeat ccache.
        self.assertEqual(set(compile_cache.CCACHE_LOCKS), {'macos-arm64', 'linux-x86_64'})
        self.assertEqual(compile_cache.SUPPORTED_HOSTS, ('macos-arm64', 'linux-x86_64'))

    def test_environment_reads_only_our_configuration(self):
        with tempfile.TemporaryDirectory() as temp:
            config = {'build_cache': {'root': str(Path(temp) / 'build-cache'), 'ccache': '/t/ccache', 'base_dir': '/b'}}
            env = compile_cache.environment(config, 'f' * 64, Path(temp) / 'stats.log')
            self.assertEqual(env['CCACHE_COMPILERCHECK'], 'string:' + 'f' * 64)
            self.assertEqual(env['CCACHE_BASEDIR'], '/b')
            self.assertEqual(Path(env['CCACHE_CONFIGPATH']).read_text(), f'max_size = {6 * 1024}MiB\n')
            self.assertEqual(Path(env['CCACHE_DIR']), Path(temp) / 'build-cache/objects')

    def test_compiler_identity_follows_the_real_compiler_file_and_version(self):
        with tempfile.TemporaryDirectory() as temp:
            compiler = Path(temp) / 'clang'
            compiler.write_text('#!/bin/sh\necho "clang version 1"\n'); compiler.chmod(0o755)
            config = {'host': 'linux-x86_64', 'linux_toolchain': {'cc': str(compiler), 'cxx': str(compiler)}}
            if os.name == 'nt':
                self.skipTest('POSIX shell compiler stand-in')
            first = compile_cache.compiler_identity(config, {'PATH': '/usr/bin:/bin'})
            self.assertEqual(first, compile_cache.compiler_identity(config, {'PATH': '/usr/bin:/bin'}))
            compiler.write_text('#!/bin/sh\necho "clang version 2"\n')
            self.assertNotEqual(first, compile_cache.compiler_identity(config, {'PATH': '/usr/bin:/bin'}))

    def test_hit_rate_and_time_saved_from_the_stats_log_and_ninja_log(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            build = temp / 'build'; build.mkdir()
            timings = temp / 'times.json'
            def run(results, durations):
                stats = temp / 'stats.log'
                stats.write_text(''.join(f'# {source}\n{line}\n' for source, line in results.items()))
                (build / '.ninja_log').write_text('# ninja log v7\n' + ''.join(
                    f'0\t{ms}\t0\t{output}\tabc\n' for output, ms in durations.items()))
                mapping = {os.path.normcase(os.path.normpath(str(build / '../src' / name))): 'obj/' + name + '.o'
                           for name in ('a.c', 'b.c', 'c.c')}
                with patch.object(compile_cache, 'compile_outputs', return_value=mapping):
                    return compile_cache.summarize('build-engine', build, stats, 'ninja', {}, timings, 42)
            cold = run({'../src/a.c': 'cache_miss', '../src/b.c': 'cache_miss', '../src/c.c': 'cache_miss'},
                       {'obj/a.c.o': 30000, 'obj/b.c.o': 20000, 'obj/c.c.o': 1000})
            self.assertEqual((cold['hits'], cold['misses'], cold['saved_seconds']), (0, 3, 0))
            warm = run({'../src/a.c': 'direct_cache_hit', '../src/b.c': 'preprocessed_cache_hit', '../src/c.c': 'cache_miss'},
                       {'obj/a.c.o': 100, 'obj/b.c.o': 200, 'obj/c.c.o': 1100})
            self.assertEqual((warm['hits'], warm['misses'], warm['hit_rate']), (2, 1, 66.7))
            self.assertEqual(warm['saved_seconds'], 49.7)
            self.assertIn('2 of 3 compilations reused (67%)', compile_cache.message(warm))



class CommandLineTests(unittest.TestCase):
    def run_cli(self, *argv):
        import io
        from contextlib import redirect_stdout
        from mag_installer.__main__ import main
        output = io.StringIO()
        with redirect_stdout(output):
            code = main(['--json', 'build-cache', *argv])
        return code, [json.loads(line) for line in output.getvalue().splitlines() if line.strip()]

    def test_usage_limit_and_clear(self):
        with tempfile.TemporaryDirectory() as temp:
            code, events = self.run_cli('--cache', temp)
            self.assertEqual((code, events[-1]['event'], events[-1]['max_bytes']), (0, 'build-cache-usage', 20 * GIB))
            code, events = self.run_cli('--cache', temp, '--max-size', '8GiB')
            self.assertEqual(events[-1]['max_bytes'], 8 * GIB)
            (BuildCache.for_cache(temp).objects_directory() / 'object').write_bytes(b'x' * 10)
            code, events = self.run_cli('--cache', temp, '--clear')
            self.assertEqual((code, events[-1]['event']), (0, 'build-cache-cleared'))
            self.assertGreaterEqual(events[-1]['freed_bytes'], 10)
            code, events = self.run_cli('--cache', temp, '--max-size', 'huge')
            self.assertEqual(code, 2)


if __name__ == '__main__':
    unittest.main()
