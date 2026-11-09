"""cmake/MagControls.cmake: default-off option and the checks on a prepared lift."""
import hashlib
import json
import shutil
import subprocess
import sys
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
        # A synthetic stand-in for the ps3recomp runtime directory and target.
        self.sdk = self.tmp / 'sdk'
        (self.sdk / 'libs/input').mkdir(parents=True)
        for name in ('cellPad.c', 'pad_sdl_keyboard.c'):
            (self.sdk / 'libs/input' / name).write_text('/* synthetic */\n')
        (self.sdk / 'CMakeLists.txt').write_text(
            'add_library(ps3recomp_runtime STATIC EXCLUDE_FROM_ALL libs/input/cellPad.c libs/input/pad_sdl_keyboard.c)\n')

    def record(self, stages=('controls',), tools=None):
        hooks = dict(schema_version=1, stages=list(stages), changed_outputs=['ppu_recomp_001.cpp'],
                     tools=tool_hashes() if tools is None else tools)
        (self.recomp / 'provenance.json').write_text(json.dumps({'status': 'complete', 'render_adapter_hooks': hooks}))

    def configure(self, *options, image='mage_g', expected=0):
        shutil.rmtree(self.tmp / 'build', ignore_errors=True)
        result = subprocess.run(['cmake', '-S', str(FIXTURE), '-B', str(self.tmp / 'build'), '-G', 'Ninja',
                                 f'-DMAG_REPO={ROOT}', f'-DMAG_IMAGE={image}', f'-DRECOMP_DIR={self.recomp}',
                                 f'-DPS3RECOMP_DIR={self.sdk}', *options], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def report(self):
        return dict(line.split('=', 1) for line in (self.tmp / 'build/controls.txt').read_text().splitlines())

    def runtime_sources(self, report):
        return {Path(entry).name for entry in report['runtime'].split(';')}

    SDL_SEAMS = {'player_input_sdl_pad.c', 'player_input_sdl_keyboard.c'}

    def test_default_off_adds_nothing_and_reads_no_lift(self):
        self.configure()  # No provenance.json exists: the default must not read it.
        report = self.report()
        self.assertEqual(report['receipt'], '')
        self.assertEqual(report['sources'], str(self.tmp / 'build/fixture.cpp'))
        self.assertEqual(report['lifted'], 'ppu_recomp_000.cpp:NOTFOUND;ppu_recomp_001.cpp:NOTFOUND')
        self.assertIn('MAG_GAME_CONTROLS:BOOL=OFF', (self.tmp / 'build/CMakeCache.txt').read_text())
        self.assertEqual(self.runtime_sources(report), {'cellPad.c', 'pad_sdl_keyboard.c'})
        self.assertEqual(report['sdk:cellPad.c'], 'NOTFOUND')
        self.assertEqual(report['sdk:pad_sdl_keyboard.c'], 'NOTFOUND')

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
        names = {Path(entry).name for entry in sources}
        if sys.platform == 'darwin':
            # macOS: the player settings module brings the desktop backend and SDK seams.
            self.assertFalse(names & {'player_input_desktop_sdl.cpp', 'mag_game_controls_desktop_none.cpp'})
            self.assertEqual(report['sdk:cellPad.c'], 'NOTFOUND')
        elif sys.platform == 'win32':
            self.assertIn('mag_game_controls_desktop_none.cpp', names)
            self.assertNotIn('player_input_desktop_sdl.cpp', names)
        else:
            self.assertIn('player_input_desktop_sdl.cpp', names)
            self.assertNotIn('mag_game_controls_desktop_none.cpp', names)

    def test_sdl_desktop_backend_renames_only_the_sdk_seams(self):
        self.record()
        self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', '-DMAG_GAME_CONTROLS_DESKTOP=sdl')
        report = self.report()
        names = {Path(entry).name for entry in report['sources'].split(';')}
        self.assertTrue({'player_desktop_capture_core.cpp', 'player_input_desktop_sdl.cpp'} <= names)
        self.assertNotIn('mag_game_controls_desktop_none.cpp', names)
        self.assertEqual(self.runtime_sources(report), {'cellPad.c', 'pad_sdl_keyboard.c'} | self.SDL_SEAMS)
        controller = dict(entry.split('=', 1) for entry in report['sdk:cellPad.c'].split(';'))
        self.assertEqual(controller, {
            'SDL_GameControllerGetButton': 'mag_player_controller_button',
            'SDL_GameControllerGetAxis': 'mag_player_controller_axis',
            'SDL_GameControllerUpdate': 'mag_player_controller_update',
            'SDL_GameControllerOpen': 'mag_player_controller_open',
            'SDL_GameControllerClose': 'mag_player_controller_close',
            'SDL_GameControllerGetAttached': 'mag_player_controller_attached'})
        keyboard = dict(entry.split('=', 1) for entry in report['sdk:pad_sdl_keyboard.c'].split(';'))
        self.assertEqual(keyboard, {'ps3recomp_pad_sdl_keyboard_sample': 'mag_sdk_pad_sdl_keyboard_sample',
                                    'ps3recomp_pad_sdl_keyboard_state': 'mag_sdk_pad_sdl_keyboard_state'})

    def test_sdl_desktop_backend_needs_the_window_keyboard_seam(self):
        self.record()
        result = self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', '-DMAG_GAME_CONTROLS_DESKTOP=sdl',
                                '-DPS3RECOMP_PAD_SDL_KEYBOARD=OFF', expected=1)
        self.assertIn('PS3RECOMP_PAD_SDL_KEYBOARD=ON', result.stderr)

    def test_hosts_without_a_backend_refuse_capture_and_leave_the_sdk_alone(self):
        self.record()
        self.configure('-DMAG_GAME_CONTROLS=ON', '-DMAG_INPUT_RUST=ON', '-DMAG_GAME_CONTROLS_DESKTOP=none')
        report = self.report()
        names = {Path(entry).name for entry in report['sources'].split(';')}
        self.assertIn('mag_game_controls_desktop_none.cpp', names)
        self.assertNotIn('player_input_desktop_sdl.cpp', names)
        self.assertEqual(self.runtime_sources(report), {'cellPad.c', 'pad_sdl_keyboard.c'})
        self.assertEqual(report['sdk:cellPad.c'], 'NOTFOUND')


if __name__ == '__main__':
    unittest.main()
