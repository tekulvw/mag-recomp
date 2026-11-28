"""Metadata and synthetic guards; no executable payload fixtures."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from tools import v212_nonlocal_profile as tool


class V212NonlocalProfileTests(unittest.TestCase):
    def setUp(self):
        self.profile = tool.ROOT / 'profiles/retail-v2.12-nonlocal.json'
        self.record = json.loads(self.profile.read_text())
        self.record['profile_sha256'] = tool.PROFILE_SHA256

    def test_exact_metadata_is_accepted(self):
        self.assertEqual(tool.validate_record(self.record, tool.ELF_SHA256), self.record)

    def test_unknown_or_altered_review_metadata_is_rejected(self):
        for field, value in [('target', 'retail-v1.0'), ('image_name', 'EBOOT'),
                             ('sdk_role', 'lifter-ppu'), ('sdk_revision', 'f'*40),
                             ('elf_sha256', '0'*64), ('configuration_sha256', '0'*64),
                             ('lifter_sha256', '0'*64), ('configuration', '../foreign.json'),
                             ('expected_changed_functions', []), ('guard_spans', []),
                             ('direct_linked_call_census', {}), ('profile_sha256', '0'*64),
                             ('configuration_sha256', '0'*64), ('lifter_sha256', '0'*64),
                             ('expected_changed_functions', ['func_00CE0BAC']),
                             ('unknown', True)]:
            with self.subTest(field=field):
                record = copy.deepcopy(self.record); record[field] = value
                with self.assertRaises(ValueError): tool.validate_record(record, tool.ELF_SHA256)
        for field in self.record:
            record = copy.deepcopy(self.record); del record[field]
            with self.assertRaises(ValueError): tool.validate_record(record, tool.ELF_SHA256)
        with self.assertRaises(ValueError): tool.validate_record(self.record, '0'*64)

    def test_config_or_profile_substitution_stops_before_sdk(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); profile = root/self.profile.name
            config_name = self.record['configuration']
            config = root/config_name
            profile.write_bytes(self.profile.read_bytes())
            config.write_bytes((self.profile.parent/config_name).read_bytes())
            elf = root/'mage_g.elf'; elf.write_bytes(b'synthetic executable identity')
            with patch.object(tool, 'check_sdk') as sdk:
                with self.assertRaisesRegex(ValueError, 'ELF identity'):
                    tool.validate_profile(profile, root/'sdk', elf)
                sdk.assert_not_called()
            for bad_profile, bad_config in [(True, False), (False, True)]:
                profile.write_bytes(self.profile.read_bytes() + (b' ' if bad_profile else b''))
                config.write_bytes((self.profile.parent/config_name).read_bytes() + (b' ' if bad_config else b''))
                with patch.object(tool, 'check_sdk') as sdk:
                    with self.assertRaisesRegex(ValueError, 'review identity|configuration identity'):
                        tool.validate_profile(profile, root/'sdk', elf)
                    sdk.assert_not_called()

    def test_wrong_image_name_is_rejected_before_sdk(self):
        with patch.object(tool, 'check_sdk') as sdk:
            with self.assertRaisesRegex(ValueError, 'image|mage_g'):
                tool.validate_profile(self.profile, Path('unused-sdk'), Path('absent'), 'EBOOT')
            sdk.assert_not_called()

    def test_local_regular_files_are_required(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); p = root/self.profile.name; p.symlink_to(self.profile)
            with self.assertRaises(ValueError): tool.validate_profile(p, root/'sdk', root/'absent')
            p.unlink();p.write_bytes(self.profile.read_bytes())
            c = root/self.record['configuration']; c.symlink_to(self.profile.parent/c.name)
            with self.assertRaises(ValueError): tool.validate_profile(p, root/'sdk', root/'absent')

    def test_configuration_contains_only_exact_restore_and_continuation(self):
        config = json.loads((self.profile.parent/self.record['configuration']).read_text())
        self.assertEqual(config, {'nonlocal_return_functions':['0x00AF96E0'],
                                 'continuations':[{'function':'0x007BF600','address':'0x007BF63C'}]})
