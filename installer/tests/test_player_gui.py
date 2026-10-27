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
