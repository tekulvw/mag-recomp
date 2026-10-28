import tempfile
import unittest
from pathlib import Path
from mag_installer.gui import PlayerBuildController
from mag_installer.model import InstallerError

class PlayerControllerTests(unittest.TestCase):
    def test_one_action_delegates_entire_build_with_selected_game_root(self):
        calls=[]
        def build(root,source,output,cache,token=None,events=None):
            calls.append((root,source,output,cache));return output
        c=PlayerBuildController(Path('/kit'), readiness=lambda root:{'ready':True,'reason':''},installer=build)
        with tempfile.TemporaryDirectory() as temp:
            parent=Path(temp)
            c.start(parent/'disc',parent,parent/'cache')
            c.worker.join(2);c.poll()
            self.assertEqual(c.state,'complete')
            self.assertEqual(calls,[(Path('/kit'),parent/'disc',parent/'MAG',parent/'cache')])

    def test_missing_maintained_kit_blocks_build_without_local_workaround(self):
        c=PlayerBuildController(Path('/kit'),readiness=lambda root:{'ready':False,'reason':'Maintained MAG build kit is missing.'})
        self.assertIn('build kit',c.blocked)
        with self.assertRaises(InstallerError):c.start(Path('/disc'),Path('/new'),Path('/cache'))

    def test_failed_build_can_be_retried_using_same_destination(self):
        import json
        calls = []
        def build(root, source, output, cache, **kwargs):
            calls.append(kwargs.get('retry', False))
            if len(calls) == 1:
                output.mkdir()
                (output / 'build.json').write_text(json.dumps({'schema_version': 2, 'state': 'building'}))
                raise InstallerError('synthetic failure')
            return output
        c = PlayerBuildController(Path('/kit'), readiness=lambda root: {'ready': True}, installer=build)
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            c.start(parent / 'disc', parent, parent / 'cache')
            c.worker.join(2); c.poll()
            reopened = PlayerBuildController(Path('/kit'), readiness=lambda root: {'ready': True}, installer=build)
            self.assertTrue(reopened.has_journal(parent))
            self.assertTrue(c.has_journal(parent))
            c.start(parent / 'disc', parent, parent / 'cache', retry=True)
            c.worker.join(2); c.poll()
            self.assertEqual(c.state, 'complete')
            self.assertEqual(calls, [False, True])
