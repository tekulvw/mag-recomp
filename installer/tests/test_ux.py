import unittest
from pathlib import Path
from mag_installer import ux

class PresentationTests(unittest.TestCase):
    def test_defaults_and_child_destinations(self):
        self.assertTrue(ux.default_cache().is_absolute())
        self.assertTrue(ux.default_parent().is_absolute())
        self.assertEqual(ux.destination(Path('/tmp')), Path('/tmp/MAG'))
        self.assertEqual(ux.destination(Path('/tmp'), True), Path('/tmp/MAG Demo'))

    def test_readable_events_and_uncertain_space(self):
        self.assertIn('build', ux.format_event({'event': 'stage-activity', 'stage': 'build', 'elapsed': 12, 'log': '/tmp/build.log'}))
        self.assertIn('demonstration', ux.format_event({'event': 'synthetic-complete'}).lower())
        self.assertIn('unknown', ux.space_summary({'download_bytes': 0, 'required_bytes': 35, 'free_bytes': 100, 'estimate_complete': False}).lower())
