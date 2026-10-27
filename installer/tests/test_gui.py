import queue
import tempfile
import threading
import unittest
from pathlib import Path
from mag_installer import gui
from mag_installer.model import InstallerError, Profile
from mag_installer.safety import Cancelled

ROOT = Path(__file__).resolve().parents[1]

class ControllerTests(unittest.TestCase):
    def controller(self, synthetic=True, **kwargs):
        profile = Profile.load(ROOT / 'profiles' / ('synthetic-demonstration.json' if synthetic else 'mag-unconfigured.json'))
        return gui.WizardController(profile, synthetic=synthetic, **kwargs)

    def drain(self, controller):
        controller.worker.join(3)
        self.assertFalse(controller.worker.is_alive())
        return controller.poll()

    def test_blocked_profile_explains_host_and_input(self):
        controller = self.controller(False)
        self.assertTrue(controller.blocked)
        self.assertIn('MAG', controller.expected_input)
        with self.assertRaises(InstallerError):
            controller.start(Path('/tmp'), Path('/tmp'), Path('/tmp/cache'))

    def test_preflight_requires_explicit_install_and_existing_output_is_refused(self):
        called = []
        controller = self.controller(preflight=lambda *args: {'estimate_complete': False}, installer=lambda *args, **kwargs: called.append(kwargs))
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            controller.start(parent, parent, parent / 'cache')
            self.drain(controller)
            self.assertEqual(controller.state, 'ready')
            self.assertEqual(called, [])
            controller.start(parent, parent, parent / 'cache', install=True)
            self.drain(controller)
            self.assertEqual(controller.state, 'complete')
            (parent / 'MAG Demo').mkdir()
            with self.assertRaisesRegex(InstallerError, 'already exists'):
                controller.start(parent, parent, parent / 'cache')

    def test_error_retains_logs_and_explicit_retry(self):
        attempts = []
        def failing(*args, **kwargs):
            attempts.append(kwargs['resume'])
            raise InstallerError('precise failure')
        controller = self.controller(preflight=lambda *args: {}, installer=failing)
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            stage = parent / '.MAG Demo.mag-staging'
            stage.mkdir()
            (stage / 'journal.json').write_text('{}')
            self.assertTrue(controller.has_journal(parent))
            with self.assertRaisesRegex(InstallerError, 'retry'):
                controller.start(parent, parent, parent/'cache', install=True)
            controller.start(parent, parent, parent/'cache', install=True, resume=True)
            self.drain(controller)
            self.assertEqual(controller.state, 'failed')
            self.assertEqual(controller.error, 'precise failure')
            self.assertEqual(controller.logs, stage/'logs')
            self.assertEqual(attempts, [True])

    def test_cancel_and_close_wait_for_worker(self):
        entered = threading.Event()
        def waiting(*args, **kwargs):
            entered.set()
            kwargs['token']._event.wait(3)
            kwargs['token'].check()
        controller = self.controller(preflight=lambda *args: {}, installer=waiting)
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            controller.start(parent,parent,parent/'cache',install=True)
            self.assertTrue(entered.wait(1))
            self.assertFalse(controller.request_close())
            self.drain(controller)
            self.assertEqual(controller.state, 'cancelled')
            self.assertTrue(controller.request_close())

    def test_real_synthetic_worker_preserves_input_and_reports_output(self):
        controller = self.controller()
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            source = parent/'input'
            source.mkdir()
            (source/'SYNTHETIC-INPUT.txt').write_bytes(b'This is synthetic installer input.\n')
            controller.start(source,parent,parent/'cache',install=True)
            self.drain(controller)
            self.assertEqual(controller.state, 'complete', controller.error)
            self.assertIn('SYNTHETIC', (controller.output/'SYNTHETIC-RESULT.txt').read_text())
            self.assertTrue((source/'SYNTHETIC-INPUT.txt').is_file())
            self.assertTrue((controller.logs/'events.jsonl').is_file())

    def test_display_output_follows_parent_after_failed_or_checked_run(self):
        controller = self.controller()
        controller.output = Path('/old/MAG Demo')
        for state in ('failed', 'ready', 'idle', 'cancelled'):
            controller.state = state
            self.assertEqual(controller.display_output(Path('/new')), Path('/new/MAG Demo'))
        controller.state = 'complete'
        self.assertEqual(controller.display_output(Path('/new')), Path('/old/MAG Demo'))

    def test_blocked_notice_is_player_facing_and_exact_reason_is_retained(self):
        controller = self.controller(False)
        self.assertEqual(controller.notice, 'This preview cannot install MAG yet. The game build recipe is not available for this platform.')
        self.assertIn('blocked:', controller.blocked)
        self.assertNotIn(controller.blocked, controller.notice)

    def test_completed_journal_does_not_offer_retry_but_unknown_journal_does(self):
        controller = self.controller()
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            stage = parent/'.MAG Demo.mag-staging'
            stage.mkdir()
            journal = stage/'journal.json'
            journal.write_text('{"state":"complete"}')
            self.assertFalse(controller.has_journal(parent))
            journal.write_text('{"state":"failed"}')
            self.assertTrue(controller.has_journal(parent))
            journal.write_text('unknown old data')
            self.assertTrue(controller.has_journal(parent))

    def test_pipeline_completion_is_rendered_only_at_controller_completion(self):
        self.assertFalse(gui.visible_progress_event({'event': 'synthetic-complete'}))
        self.assertFalse(gui.visible_progress_event({'event': 'local-build-complete'}))
        self.assertTrue(gui.visible_progress_event({'event': 'stage-start', 'stage': 'build'}))
