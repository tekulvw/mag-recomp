"""cmake/MagControls.cmake: default-off option and the checks on a prepared lift."""
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/cmake_game_controls'


def tool_hashes():
    return {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted((ROOT / 'tools/render_hooks').glob('*.py'))}


@unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'cmake and ninja required')
class GameControlsBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mag-game-controls-'))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.recomp = self.tmp / 'recomp'
        self.recomp.mkdir()
        for name in ('ppu_recomp_000.cpp', 'ppu_recomp_001.cpp'):
            (self.recomp / name).write_text('// synthetic\n')

    def record(self, stages=('controls',), tools=None):
        hooks = dict(schema_version=1, stages=list(stages), changed_outputs=['ppu_recomp_001.cpp'],
                     tools=tool_hashes() if tools is None else tools)
        (self.recomp / 'provenance.json').write_text(json.dumps({'status': 'complete', 'render_adapter_hooks': hooks}))

    def configure(self, *options, image='mage_g', expected=0):
        shutil.rmtree(self.tmp / 'build', ignore_errors=True)
        result = subprocess.run(['cmake', '-S', str(FIXTURE), '-B', str(self.tmp / 'build'), '-G', 'Ninja',
                                 f'-DMAG_REPO={ROOT}', f'-DMAG_IMAGE={image}', f'-DRECOMP_DIR={self.recomp}',
                                 f'-DPS3RECOMP_DIR={self.tmp / "sdk"}', *options], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def report(self):
        return dict(line.split('=', 1) for line in (self.tmp / 'build/controls.txt').read_text().splitlines())

    def test_default_off_adds_nothing_and_reads_no_lift(self):
        self.configure()  # No provenance.json exists: the default must not read it.
        report = self.report()
        self.assertEqual(report['receipt'], '')
        self.assertEqual(report['sources'], str(self.tmp / 'build/fixture.cpp'))
        self.assertEqual(report['lifted'], 'ppu_recomp_000.cpp:NOTFOUND;ppu_recomp_001.cpp:NOTFOUND')
        self.assertIn('MAG_GAME_CONTROLS:BOOL=OFF', (self.tmp / 'build/CMakeCache.txt').read_text())

    def test_launcher_image_is_left_alone(self):
        self.configure('-DMAG_GAME_CONTROLS=ON', image='EBOOT')
        self.assertEqual(self.report()['receipt'], '')

    def test_requires_the_rust_engine_and_a_controls_lift(self):
        self.assertIn('MAG_INPUT_RUST', self.configure('-DMAG_GAME_CONTROLS=ON', expected=1).stderr)
        self.assertIn('--stage controls', self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', expected=1).stderr)
        self.record(stages=['configuration'])
        self.assertIn('without the controls hook stage',
                      self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', expected=1).stderr)
        self.record(tools={name: '0' * 64 for name in tool_hashes()})
        self.assertIn('prepared by a different', self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', expected=1).stderr)

    def test_prepared_lift_adds_the_adapter_guard_and_receipt(self):
        self.record()
        self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON')
        report = self.report()
        sources = report['sources'].split(';')
        self.assertIn(str(ROOT / 'src/game_controls/mag_game_controls_adapter.cpp'), sources)
        self.assertIn(str(ROOT / 'src/game_controls/mag_game_controls_runtime.cpp'), sources)
        self.assertEqual(report['lifted'], 'ppu_recomp_000.cpp:NOTFOUND;ppu_recomp_001.cpp:MAG_GAME_CONTROLS_HOOKS=1')
        names = {entry.split('=', 1)[0] for entry in report['receipt'].split(';')}
        self.assertEqual(names, {'game-controls-sources', 'game-controls-build'})


if __name__ == '__main__':
    unittest.main()
