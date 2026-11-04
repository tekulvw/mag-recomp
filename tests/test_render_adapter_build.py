"""cmake/MagRenderAdapter.cmake: default-off options and the checks on a prepared lift."""
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.render_hooks import prepare

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/cmake_render_adapter'
RENDER_FIXTURE = ROOT / 'tests/fixtures/cmake_rust_render'


@unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'cmake and ninja required')
class RenderAdapterBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mag-render-adapter-'))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.recomp = self.tmp / 'recomp'
        self.recomp.mkdir()
        for name in ('ppu_recomp_000.cpp', 'ppu_recomp_001.cpp'):
            (self.recomp / name).write_text('// synthetic\n')
        self.sdk = self.tmp / 'sdk'
        (self.sdk / 'libs/video').mkdir(parents=True)

    def record(self, stages=('configuration',), tools=None, **overrides):
        hooks = dict(schema_version=1, stages=list(stages), changed_outputs=['ppu_recomp_001.cpp'],
                     tools=tools if tools is not None else
                     {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in prepare.tool_files()})
        hooks.update(overrides)
        (self.recomp / 'provenance.json').write_text(json.dumps({'status': 'complete', 'render_adapter_hooks': hooks}))

    def configure(self, *options, expected=0):
        shutil.rmtree(self.tmp / 'build', ignore_errors=True)
        command = ['cmake', '-S', str(FIXTURE), '-B', str(self.tmp / 'build'), '-G', 'Ninja', f'-DMAG_REPO={ROOT}',
                   '-DMAG_IMAGE=mage_g', f'-DRECOMP_DIR={self.recomp}', f'-DPS3RECOMP_DIR={self.sdk}', *options]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def report(self):
        return dict(line.split('=', 1) for line in (self.tmp / 'build/adapter.txt').read_text().splitlines())

    def test_default_off_adds_nothing_and_reads_no_lift(self):
        # No provenance.json exists: the default must not read it.
        self.configure()
        report = self.report()
        self.assertEqual(report['receipt'], '')
        self.assertEqual(report['sources'], str(self.tmp / 'build/fixture.cpp'))
        self.assertEqual(report['lifted'], 'ppu_recomp_000.cpp:NOTFOUND;ppu_recomp_001.cpp:NOTFOUND')

    def test_default_off_render_frontend_receipt_is_unchanged(self):
        directory = self.tmp / 'render'
        shutil.copytree(RENDER_FIXTURE, directory)
        (directory / 'cmake').mkdir()
        for name in ('MagRender.cmake', 'MagRenderAdapter.cmake'):
            shutil.copy2(ROOT / 'cmake' / name, directory / 'cmake')
        result = subprocess.run(['cmake', '-S', str(directory), '-B', str(directory / 'build'), '-G', 'Ninja'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((directory / 'build/receipt-extra.txt').read_text(), '')
        cache = (directory / 'build/CMakeCache.txt').read_text()
        for option in ('MAG_RENDER_ADAPTER', 'MAG_RENDER_ADAPTER_SHADOW_VIEW', 'MAG_RENDER_ADAPTER_COHORT',
                       'MAG_RENDER_ADAPTER_UI_HANDOFF'):
            self.assertIn(f'{option}:BOOL=OFF', cache)

    def test_prepared_lift_adds_bridge_guards_and_receipts(self):
        self.record(stages=['configuration', 'shadow-view'])
        self.configure('-DMAG_RENDER_ADAPTER=ON', '-DMAG_RENDER_ADAPTER_SHADOW_VIEW=ON')
        report = self.report()
        self.assertEqual(report['sources'], str(self.tmp / 'build/fixture.cpp') + ';' + str(ROOT / 'src/render_adapter/mag_render_frontend_bridge.cpp'))
        # Only the rewritten lifted file gets the stage guards.
        self.assertEqual(report['lifted'], 'ppu_recomp_000.cpp:NOTFOUND;ppu_recomp_001.cpp:'
                         'MAG_RENDER_ADAPTER_CONFIGURATION=1;MAG_RENDER_ADAPTER_SHADOW_VIEW=1')
        names = {entry.split('=', 1)[0] for entry in report['receipt'].split(';')}
        self.assertIn('render-adapter-bridges', names)
        self.assertEqual({name for name in names if name.startswith('render-adapter-hooks-')},
                         {'render-adapter-hooks-' + Path(tool).stem for tool in prepare.tool_files()})

    def test_unprepared_or_stale_lifts_are_refused(self):
        self.assertIn('prepared with the render hooks', self.configure('-DMAG_RENDER_ADAPTER=ON', expected=1).stderr)
        self.record(stages=['configuration'])
        self.assertIn('without the shadow-view render hook stage', self.configure(
            '-DMAG_RENDER_ADAPTER=ON', '-DMAG_RENDER_ADAPTER_SHADOW_VIEW=ON', expected=1).stderr)
        tools = {name: '0' * 64 for name in prepare.tool_files()}
        self.record(tools=tools)
        self.assertIn('prepared by a different', self.configure('-DMAG_RENDER_ADAPTER=ON', expected=1).stderr)
        tools = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in prepare.tool_files()[1:]}
        self.record(tools=tools)
        self.assertIn('different set of render hook tools', self.configure('-DMAG_RENDER_ADAPTER=ON', expected=1).stderr)
        self.record(schema_version=2)
        self.assertIn('unsupported schema', self.configure('-DMAG_RENDER_ADAPTER=ON', expected=1).stderr)

    def located_sdk(self):
        (self.sdk / 'libs/video/cellGcmSys.h').write_text('int cellGcmNativeReadLocatedRange(unsigned, unsigned);\n')

    def test_cohort_needs_the_sdk_located_range_and_its_stages(self):
        self.record(stages=['configuration', 'update-loop', 'cohort'])
        options = ('-DMAG_RENDER_ADAPTER=ON', '-DMAG_RENDER_ADAPTER_COHORT=ON')
        self.assertIn('needs cellGcmNativeReadLocatedRange', self.configure(*options, expected=1).stderr)
        self.located_sdk()
        self.configure(*options)
        report = self.report()
        bridges = ROOT / 'src/render_adapter'
        self.assertEqual(report['sources'].split(';')[1:], [str(bridges / name) for name in (
            'mag_render_frontend_bridge.cpp', 'mag_render_update_loop.cpp', 'mag_render_cohort_bridge.cpp',
            'mag_render_texture_bridge.cpp')])
        self.assertTrue(report['lifted'].endswith('ppu_recomp_001.cpp:MAG_RENDER_ADAPTER_CONFIGURATION=1;'
                                                  'MAG_RENDER_ADAPTER_UPDATE_LOOP=1;MAG_RENDER_ADAPTER_COHORT=1'))
        self.record(stages=['configuration', 'cohort'])
        self.assertIn('without the update-loop render hook stage', self.configure(*options, expected=1).stderr)

    def test_ui_handoff_needs_the_sdk_handoff_header_first(self):
        self.record(stages=['configuration', 'update-loop', 'cohort', 'ui-handoff'])
        self.located_sdk()
        options = ('-DMAG_RENDER_ADAPTER=ON', '-DMAG_RENDER_ADAPTER_COHORT=ON', '-DMAG_RENDER_ADAPTER_UI_HANDOFF=ON')
        self.assertIn('needs libs/video/rsx_native_handoff.h', self.configure(*options, expected=1).stderr)
        (self.sdk / 'libs/video/rsx_native_handoff.h').write_text('#pragma once\n')
        self.assertIn('its hook bridge is not in this repository',
                      self.configure(*options, expected=1).stderr)

    def test_option_dependencies_are_checked(self):
        for options, message in ((['-DMAG_RENDER_ADAPTER_COHORT=ON'], 'requires MAG_RENDER_ADAPTER'),
                                 (['-DMAG_RENDER_ADAPTER=ON', '-DMAG_RENDER_ADAPTER_UI_HANDOFF=ON'],
                                  'requires MAG_RENDER_ADAPTER_COHORT'),
                                 (['-DMAG_RENDER_ADAPTER=ON', '-DMAG_IMAGE=EBOOT'], 'only to the mage_g')):
            self.record()
            self.assertIn(message, self.configure(*options, expected=1).stderr)


if __name__ == '__main__':
    unittest.main()
