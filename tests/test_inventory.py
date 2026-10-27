import unittest
from tools.inventory import summarize_functions, restrict_to_code


class InventoryTests(unittest.TestCase):
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
