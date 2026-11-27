"""The wizard's preparation guide: its controller headless, and its window only where windows may open.

The window test opens a real Tk window, so it runs only with MAG_INSTALLER_WINDOW_TESTS=1 (CI sets it;
developer machines leave it unset so no window appears)."""
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from mag_installer import gui
from mag_installer.rpcs3_detect import Detection, GameCandidate

from test_input_guide import FIRMWARE, REVIEWED, prepared
from test_rpcs3_detect import write


def ready(root):
    return {'ready': True, 'reason': ''}


def wait_for(predicate, pump=lambda: None, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        pump()
        if predicate():
            return True
        time.sleep(0.02)
    return False


class GuideControllerTests(unittest.TestCase):
    def test_survey_finds_once_checks_each_choice_and_keeps_only_the_newest_result(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            disc = prepared(root / 'MAG')
            calls = []
            def detector():
                calls.append(1)
                return Detection('linux', games=[GameCandidate(disc, 'RPCS3 games list', 'BCUS98110', '01.00', 'DG')])
            controller = gui.PlayerBuildController(root, readiness=ready, detector=detector, reviewed=REVIEWED)
            controller.survey()
            controller.guide_worker.join(5)
            events = [e for e in controller.poll_guide() if e['event'] == 'guide']
            self.assertEqual(len(events), 1)
            self.assertEqual(events[0]['game_path'], disc)
            self.assertIn('Prepare MAG\'s files', events[0]['text'])
            firmware = write(root / 'libsre.prx', FIRMWARE)
            controller.survey(str(disc), str(firmware))
            controller.guide_worker.join(5)
            event = [e for e in controller.poll_guide() if e['event'] == 'guide'][-1]
            self.assertEqual(event['firmware'][0].code, 'firmware-ok')
            self.assertEqual(len(calls), 1)  # RPCS3 is looked for once, and again only on request
            controller.survey(search=True)
            controller.guide_worker.join(5)
            self.assertEqual(len(calls), 2)
            first = controller.survey()
            second = controller.survey()
            controller.guide_worker.join(5)
            self.assertTrue(all(e['request'] == second for e in controller.poll_guide()))
            self.assertNotEqual(first, second)

    def test_chosen_firmware_reaches_the_build(self):
        calls = []
        def build(root, source, output, cache, **kwargs):
            calls.append(kwargs.get('firmware'))
            return output
        controller = gui.PlayerBuildController(Path('/kit'), readiness=ready, installer=build)
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            controller.start(parent / 'disc', parent, parent / 'cache', firmware=str(parent / 'libsre.prx'))
            controller.worker.join(5); controller.poll()
            (parent / 'other').mkdir()
            controller.start(parent / 'disc', parent / 'other', parent / 'cache')
            controller.worker.join(5); controller.poll()
        self.assertEqual(calls, [parent / 'libsre.prx', None])


@unittest.skipUnless(os.environ.get('MAG_INSTALLER_WINDOW_TESTS') == '1',
                     'opens an installer window; set MAG_INSTALLER_WINDOW_TESTS=1 (CI does)')
class GuideWindowTests(unittest.TestCase):
    def test_window_shows_the_guide_fills_found_paths_and_checks_them(self):
        import tkinter as tk
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            disc = prepared(root / 'MAG')
            firmware = write(root / 'rpcs3/dev_flash/sys/external/libsre.prx', FIRMWARE)
            found = Detection('linux', rpcs3=[('rpcs3',)], firmware_decrypted=[firmware],
                              games=[GameCandidate(disc, 'RPCS3 games list', 'BCUS98110', '01.00', 'DG')])
            controller = gui.PlayerBuildController(root, readiness=ready, detector=lambda: found, reviewed=REVIEWED)
            home = root / 'home'; home.mkdir()
            with patch.dict(os.environ, {'HOME': str(home), 'USERPROFILE': str(home), 'XDG_CACHE_HOME': str(home / 'cache'),
                                         'LOCALAPPDATA': str(home / 'local')}), \
                    patch.object(gui.ux, 'default_cache', return_value=root / 'cache'):
                window = tk.Tk()
                try:
                    widgets = gui.open_window(window, root, controller=controller)
                    self.assertTrue(wait_for(lambda: 'Prepare MAG' in widgets['guide'].get('1.0', 'end'), window.update))
                    self.assertEqual(widgets['source'].get(), str(disc))
                    self.assertEqual(widgets['firmware'].get(), str(firmware))
                    self.assertTrue(wait_for(lambda: widgets['checks']['Firmware file'].get().startswith('Checked'), window.update))
                    self.assertIn('found by the installer', widgets['checks']['Game folder'].get())
                    self.assertIn('click Build MAG', widgets['guide_status'].get())
                    # A file of the player's own is checked at once and never replaced by a found one.
                    bad = write(root / 'chosen.prx', b'SCE\x00 container')
                    widgets['firmware'].set(str(bad)); widgets['survey']()
                    self.assertTrue(wait_for(lambda: widgets['checks']['Firmware file'].get().startswith('Problem'), window.update))
                    self.assertIn('still encrypted', widgets['checks']['Firmware file'].get())
                    self.assertEqual(widgets['firmware'].get(), str(bad))
                    self.assertEqual(str(widgets['guide'].cget('state')), 'disabled')
                finally:
                    window.destroy()


if __name__ == '__main__':
    unittest.main()
