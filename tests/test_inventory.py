import unittest
from pathlib import Path
from unittest.mock import patch
from tools.inventory import summarize_functions, restrict_to_code, check_sdk


class InventoryTests(unittest.TestCase):
    def test_named_sdk_role_is_verified_without_widening_legacy_lifter_policy(self):
        framework = {'allowed_lifter_revisions': ['legacy-lifter'], 'ps3recomp': {
            'lifter-ppu-v212': {'commit': 'reviewed-target-lifter', 'tag': 'reviewed-tag'}}}
        with patch('tools.inventory.load_framework', return_value=(framework, Path('framework.json'))), \
             patch('tools.inventory.subprocess.check_output', side_effect=['reviewed-target-lifter', '', 'reviewed-target-lifter']) as git:
            check_sdk(Path('sdk'), expected_revision='reviewed-target-lifter', role='lifter-ppu-v212')
        self.assertIn('refs/tags/reviewed-tag^{commit}', git.call_args.args[0])
        self.assertEqual(framework['allowed_lifter_revisions'], ['legacy-lifter'])

    def test_named_sdk_role_rejects_revision_tag_or_dirty_checkout(self):
        framework = {'allowed_lifter_revisions': ['legacy-lifter'], 'ps3recomp': {
            'lifter-ppu-v212': {'commit': 'reviewed-target-lifter', 'tag': 'reviewed-tag'}}}
        for expected, observed, dirty, tag in [
                ('foreign', 'foreign', '', 'foreign'),
                ('reviewed-target-lifter', 'foreign', '', 'reviewed-target-lifter'),
                ('reviewed-target-lifter', 'reviewed-target-lifter', ' M tools/ppu_lifter.py', 'reviewed-target-lifter'),
                ('reviewed-target-lifter', 'reviewed-target-lifter', '', 'foreign')]:
            with self.subTest(expected=expected, observed=observed, dirty=dirty, tag=tag), \
                 patch('tools.inventory.load_framework', return_value=(framework, Path('framework.json'))), \
                 patch('tools.inventory.subprocess.check_output', side_effect=[observed, dirty, tag]):
                with self.assertRaises(ValueError):
                    check_sdk(Path('sdk'), expected_revision=expected, role='lifter-ppu-v212')

    def test_data_in_executable_segment_is_not_lifted(self):
        rows = [{'start':'0x10000','end':'0x10200'}, {'start':'0x10120','end':'0x10200'}]
        self.assertEqual(restrict_to_code(rows, [(0x10000,0x10100)]),
                         [{'start':'0x10000','end':'0x10100'}])

    def test_multiple_tocs_remain_attached_to_separate_images(self):
        rows = [{'start':'0x10000','end':'0x10020','toc':'0x20000'},
                {'start':'0x10020','end':'0x10040','toc':'0x30000'}]
        first = summarize_functions('EBOOT', rows)
        second = summarize_functions('mage_g', [{'start':'0x10000','end':'0x10020','toc':'0x40000'}])
        self.assertEqual(first['tocs'], ['0x20000', '0x30000'])
        self.assertEqual(second['tocs'], ['0x40000'])
        self.assertEqual(first['image'], 'EBOOT')
        self.assertEqual(second['image'], 'mage_g')

    def test_missing_toc_not_reported_as_known(self):
        result = summarize_functions('EBOOT', [{'start':'0x10000','end':'0x10004'}])
        self.assertEqual(result['tocs'], [])
        self.assertEqual(result['functions_without_toc'], 1)
