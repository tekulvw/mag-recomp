import tempfile
import unittest
from pathlib import Path
from mag_installer.gui import LocalBuildController
from mag_installer.model import InstallerError
from mag_installer.safety import Cancelled

class LocalBuildControllerTests(unittest.TestCase):
    def test_check_is_read_only_then_build_uses_explicit_local_config(self):
        calls=[]
        def check(config, output, token=None):
            calls.append(('check', config, output))
            return {'experimental':True}
        def build(config, output, **kwargs):
            calls.append(('build', config, output))
            return output
        c=LocalBuildController(preflight=check, installer=build)
        with tempfile.TemporaryDirectory() as temp:
            config=Path(temp)/'recipe.json'; output=Path(temp)/'new'
            c.start(config,output)
            c.worker.join(2); c.poll()
            self.assertEqual(c.state,'ready')
            self.assertEqual([x[0] for x in calls],['check'])
            c.start(config,output,install=True)
            c.worker.join(2); c.poll()
            self.assertEqual(c.state,'complete')
            self.assertEqual(c.logs,output/'logs')
            self.assertEqual(calls[-1],('build',config,output))

    def test_failure_retains_workspace_and_requires_new_destination(self):
        def build(config,output,**kwargs):
            output.mkdir()
            raise InstallerError('tool failed: output/logs/build.log')
        c=LocalBuildController(preflight=lambda *a,**kw:{},installer=build)
        with tempfile.TemporaryDirectory() as temp:
            output=Path(temp)/'retained'
            c.start(Path(temp)/'recipe.json',output,install=True)
            c.worker.join(2); c.poll()
            self.assertEqual(c.state,'failed')
            self.assertIn('tool failed',c.error)
            self.assertTrue(output.is_dir())
            with self.assertRaisesRegex(InstallerError,'new'):
                c.start(Path(temp)/'recipe.json',output,install=True)

    def test_local_completion_is_shown_once_by_controller(self):
        from mag_installer.gui import visible_progress_event
        self.assertFalse(visible_progress_event({'event':'experimental-local-build-complete'}))

    def test_cancel_preserves_worker_lifecycle(self):
        import threading
        entered=threading.Event()
        def build(config,output,**kwargs):
            entered.set()
            kwargs['token']._event.wait(2)
            kwargs['token'].check()
        c=LocalBuildController(preflight=lambda *a,**kw:{},installer=build)
        with tempfile.TemporaryDirectory() as temp:
            c.start(Path(temp)/'recipe.json',Path(temp)/'new',install=True)
            self.assertTrue(entered.wait(1))
            self.assertFalse(c.request_close())
            c.worker.join(2);c.poll()
            self.assertEqual(c.state,'cancelled')
            self.assertTrue(c.request_close())

    def test_build_delegates_preflight_once_to_installer_and_check_gets_token(self):
        calls=[]
        def check(config,output,token=None):
            calls.append(('check',token))
            return {}
        def build(config,output,**kwargs):
            calls.append(('build',kwargs['token']))
            return output
        c=LocalBuildController(preflight=check,installer=build)
        with tempfile.TemporaryDirectory() as temp:
            config=Path(temp)/'recipe.json';output=Path(temp)/'new'
            c.start(config,output)
            c.worker.join(2);c.poll()
            self.assertIsNotNone(calls[0][1])
            c.start(config,output,install=True)
            c.worker.join(2);c.poll()
            self.assertEqual([call[0] for call in calls],['check','build'])
