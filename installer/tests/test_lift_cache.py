"""Reuse of generation: what is observed, what keys an entry, and when results are not kept.

Synthetic only: a made-up "generator" script stands in for the lift tools."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

from mag_installer import lift_cache
from mag_installer.build_cache import BuildCache
from mag_installer.pipeline import run_command
from mag_installer.safety import Cancellation

GENERATOR = r'''
import os, subprocess, sys
from pathlib import Path
workspace = Path(sys.argv[1]); output = Path(sys.argv[2])
data = (workspace / 'tools/data.txt').read_text()
game = (workspace / 'local/game/input.bin').read_bytes()
listing = sorted(os.listdir(workspace / 'local/game'))
try:
    optional = (workspace / 'local/optional.txt').read_text()
except FileNotFoundError:
    optional = 'absent'
extra = os.environ.get('SYNTHETIC_EXTRA')
if extra:
    optional += open(extra).read()
if os.environ.get('SYNTHETIC_PROGRAM'):
    subprocess.run([os.environ['SYNTHETIC_PROGRAM']], check=True, stdout=subprocess.DEVNULL)
if os.environ.get('SYNTHETIC_CHILD'):
    subprocess.run([sys.executable, '-c', 'open(%r).read()' % os.environ['SYNTHETIC_CHILD']], check=True)
output.mkdir(parents=True, exist_ok=True)
(output / 'out.cpp').write_text(f'// {data} {game.hex()} {listing} {optional}\n')
(output / 'provenance.json').write_text('{"command": "%s"}\n' % sys.argv[0])
(output / 'empty').mkdir(exist_ok=True)
with open(Path(sys.argv[3]), 'a') as counter:
    counter.write('ran\n')
'''

PINS = {'ps3recomp': {role: {'tag': 'mag-recomp/' + role, 'commit': 'a' * 40, 'path': 'local/sdk-' + role}
                      for role in ('runtime', 'lifter-spu', 'lifter-ppu', 'spu-programs')}}


class GenerationCacheTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.temp = Path(temp.name).resolve()
        self.cache_root = self.temp / 'user-cache/build-cache'
        self.counter = self.temp / 'counter.txt'
        self.inputs = self.temp / 'inputs'
        for name in ('eboot.elf', 'engine.elf', 'firmware.prx'):
            path = self.inputs / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(name.encode())

    def build_folder(self, name, game=b'\x01\x02', data='recipe-1'):
        build = self.temp / name
        workspace = build / 'build-work/workspace'
        (workspace / 'tools').mkdir(parents=True)
        (workspace / 'tools/generate.py').write_text(GENERATOR)
        (workspace / 'tools/data.txt').write_text(data)
        (workspace / 'profiles').mkdir()
        (workspace / 'profiles/framework.json').write_text(json.dumps(PINS))
        (workspace / 'src').mkdir()
        (workspace / 'src/main.cpp').write_text('int main() {}\n')
        (workspace / 'local/game').mkdir(parents=True)
        (workspace / 'local/game/input.bin').write_bytes(game)
        return build, workspace

    def generate(self, build, workspace, env_extra=None):
        events = []
        config = {'python': sys.executable, 'git': shutil.which('git') or '/usr/bin/git',
                  'eboot_elf': str(self.inputs / 'eboot.elf'), 'engine_elf': str(self.inputs / 'engine.elf'),
                  'firmware': str(self.inputs / 'firmware.prx'),
                  'build_cache': {'root': str(self.cache_root), 'base_dir': str(build), 'ccache': '/unused'}}
        logs = build / 'build-work/logs'; logs.mkdir(parents=True, exist_ok=True)
        stages = [{'id': 'lift-synthetic', 'argv': [sys.executable, str(workspace / 'tools/generate.py'), str(workspace),
                                                    str(workspace / 'generated/synthetic'), str(self.counter)]}]
        env = {key: value for key, value in os.environ.items() if key in ('PATH', 'SYSTEMROOT', 'SystemRoot', 'TEMP', 'TMP')}
        env.update(PYTHONNOUSERSITE='1', PYTHONPYCACHEPREFIX=str(build / 'build-work' / 'home' / 'pycache'), **(env_extra or {}))
        token = Cancellation()
        def run_stage(stage, stage_env=None):
            run_command(stage['argv'], workspace, logs / (stage['id'] + '.log'), token, stage_env or env, None, stage['id'])
        result = lift_cache.run_generation(config, workspace, stages, run_stage, env, token, events.append)
        return result, events

    def runs(self):
        return self.counter.read_text().count('ran') if self.counter.exists() else 0

    def test_generation_again_in_the_same_location_is_restored_and_elsewhere_runs(self):
        first, workspace = self.build_folder('MAG-build')
        result, events = self.generate(first, workspace)
        self.assertIsNone(result)
        self.assertEqual([e['event'] for e in events if e['event'].startswith('build-cache')],
                         ['build-cache-miss', 'build-cache-stored'], events)
        record = BuildCache(self.cache_root).entries()[0]
        # Reads beyond tools/ and profiles/ are part of the entry: the game file, a listing, an absence.
        self.assertIn('local/game/input.bin', record['reads'])
        self.assertIsNone(record['reads']['local/optional.txt'])
        self.assertIn('local/game', record['lists'])
        self.assertNotIn('tools/data.txt', record['reads'])
        self.assertNotIn('src/main.cpp', record['reads'])
        self.assertEqual(sorted(record['files']), ['generated/synthetic/out.cpp', 'generated/synthetic/provenance.json'])
        self.assertIn('generated/synthetic/empty', record['folders'])
        self.assertEqual(self.runs(), 1)
        expected = {name: (workspace / name).read_bytes() for name in record['files']}

        # The player deletes the build folder and builds again in the same place.
        shutil.rmtree(first)
        first, workspace = self.build_folder('MAG-build')
        result, events = self.generate(first, workspace)
        self.assertIsNotNone(result)
        self.assertEqual(self.runs(), 1)   # not generated again
        self.assertEqual(events[-1]['event'], 'build-cache-hit')
        self.assertEqual({name: (workspace / name).read_bytes() for name in record['files']}, expected)
        self.assertTrue((workspace / 'generated/synthetic/empty').is_dir())

        # Another location generates again (its records would name the new folder).
        other, elsewhere = self.build_folder('Elsewhere/MAG-build')
        self.assertIsNone(self.generate(other, elsewhere)[0])
        self.assertEqual(self.runs(), 2)
        self.assertEqual(json.loads((elsewhere / 'generated/synthetic/provenance.json').read_text()),
                         {'command': str(elsewhere / 'tools/generate.py')})

    def test_any_change_to_what_generation_read_generates_again(self):
        def again(**change):
            build = self.temp / 'MAG-build'
            if build.exists():
                shutil.rmtree(build)
            return self.build_folder('MAG-build', **change)
        build, workspace = again()
        self.generate(build, workspace)
        for label, change in {'game file': dict(game=b'\x09'), 'recipe': dict(data='recipe-2')}.items():
            with self.subTest(label):
                build, workspace = again(**change)
                before = self.runs()
                result, events = self.generate(build, workspace)
                self.assertIsNone(result)
                self.assertEqual(self.runs(), before + 1)
        build, workspace = again(data='recipe-2')
        (workspace / 'local/optional.txt').write_text('now present')
        self.assertIsNone(self.generate(build, workspace)[0])
        build, workspace = again(data='recipe-2')
        (workspace / 'local/game/another.bin').write_bytes(b'')   # a listed folder changed
        self.assertIsNone(self.generate(build, workspace)[0])
        # A source file generation never read does not matter.
        build, workspace = again(data='recipe-2')
        (workspace / 'local/game/another.bin').write_bytes(b'')
        (workspace / 'src/main.cpp').write_text('int main() { return 1; }\n')
        before = self.runs()
        self.assertIsNotNone(self.generate(build, workspace)[0])
        self.assertEqual(self.runs(), before)

    def test_results_are_not_kept_when_generation_reads_or_starts_something_unaccounted(self):
        outside = self.temp / 'elsewhere.txt'; outside.write_text('outside')
        cases = {'outside read': {'SYNTHETIC_EXTRA': str(outside)},
                 'other program': {'SYNTHETIC_PROGRAM': shutil.which('true') or shutil.which('hostname') or 'hostname'},
                 'traced child read': {'SYNTHETIC_CHILD': str(outside)}}
        for index, (label, extra) in enumerate(cases.items()):
            with self.subTest(label):
                build, workspace = self.build_folder(f'{index}/MAG-build')
                result, events = self.generate(build, workspace, extra)
                self.assertIsNone(result)
                skipped = [e for e in events if e['event'] == 'build-cache-skipped']
                self.assertEqual(len(skipped), 1, events)
                self.assertEqual(BuildCache(self.cache_root).entries(), [])


if __name__ == '__main__':
    unittest.main()
