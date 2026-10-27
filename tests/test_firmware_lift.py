import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from tools import firmware_lift

class FirmwareLiftTests(unittest.TestCase):
    def test_profile_revision_mismatch_stops_before_processing_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'builds').mkdir()
            (root/'builds/retail-v1.0-firmware.json').write_text(json.dumps(dict(
                lifter_repository='/unused/sdk',lifter_revision='expected',image='input',image_sha256='hash')))
            with patch.object(firmware_lift,'ROOT',root), patch.object(firmware_lift,'check_sdk'), \
                 patch.object(firmware_lift.subprocess,'check_output',return_value='different\n'), \
                 patch.object(firmware_lift,'verify_hash',side_effect=AssertionError('input processing reached')):
                with self.assertRaisesRegex(ValueError,'revision'):
                    firmware_lift.generate(root/'out')
            self.assertFalse((root/'out').exists())

class FirmwareGeneratorPinTests(unittest.TestCase):
    def test_clean_reviewed_revision_is_allowed_but_foreign_or_dirty_is_not(self):
        from tools import inventory
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'builds').mkdir()
            (root/'builds/framework.json').write_text(json.dumps({'allowed_lifter_revisions':['reviewed']}))
            with patch.object(inventory,'ROOT',root):
                for head,dirty,expected,accepted in [('reviewed','','reviewed',True),
                    ('foreign','','foreign',False),('reviewed','modified','reviewed',False),
                    ('wrong','','reviewed',False)]:
                    with self.subTest(head=head,dirty=dirty), patch.object(inventory.subprocess,
                        'check_output',side_effect=[head,dirty]):
                        if accepted: inventory.check_sdk(root/'sdk',expected_revision=expected)
                        else:
                            with self.assertRaises(ValueError):
                                inventory.check_sdk(root/'sdk',expected_revision=expected)

class FirmwareBridgeTests(unittest.TestCase):
    def test_bridge_replaces_only_named_function_not_surrounding_code(self):
        source = ('void mag_sre_func_3000B0D0(ppu_context* ctx) {\n'
                  '    original_helper(ctx);\n}\n'
                  'void mag_sre_func_3000B0D8(ppu_context* ctx) {\n'
                  '    mag_sre_func_3000B0D0(ctx);\n}\n')
        result = firmware_lift.bridge_function(source, '0x3000B0D0', '0xB9BC6207')
        self.assertIn('ps3_hle_call(0xB9BC6207u, ctx);', result)
        self.assertNotIn('original_helper(ctx)', result)
        self.assertIn('    mag_sre_func_3000B0D0(ctx);', result)
        self.assertIn('void mag_sre_func_3000B0D8', result)

    def test_bridge_requires_exactly_one_definition(self):
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            firmware_lift.bridge_function('no definition', '0x3000B0D0', '0xB9BC6207')


class FirmwarePlacementTests(unittest.TestCase):
    def setUp(self):
        self.profile=json.loads((firmware_lift.ROOT/'builds/retail-v1.0-firmware.json').read_text())

    def test_reviewed_profile_avoids_guest_gcm_and_runtime_injection_windows(self):
        base=self.profile['base']
        # The first interval is the observed retail GCM IO buffer; the second
        # contains the runtime's labels, FIFO control, and offset tables.
        for start,end in ((0x30000000,0x32300000),(0x20000000,0x20008000)):
            self.assertFalse(base < end and base+205632 > start)

    def test_old_overlap_and_other_unreviewed_placements_are_rejected(self):
        for base in (0x30000000,0x3003184C,0x20000000,0x210F0000,0x40000000):
            profile=dict(self.profile,base=base)
            with self.subTest(base=hex(base)), self.assertRaisesRegex(ValueError,'placement'):
                firmware_lift.validate_placement(profile,205632)

    def test_relocated_profile_and_full_image_fit_reserved_window(self):
        firmware_lift.validate_placement(self.profile,205632)
        with self.assertRaisesRegex(ValueError,'window'):
            firmware_lift.validate_placement(self.profile,0x100001)

    def test_stale_absolute_addresses_are_rejected(self):
        for field in ('toc','code_end'):
            profile=dict(self.profile);profile[field]+=0x10000
            with self.subTest(field=field), self.assertRaisesRegex(ValueError,'address'):
                firmware_lift.validate_placement(profile,205632)
        import copy
        profile=copy.deepcopy(self.profile)
        profile['hle_bridges'][0]['stub']='0x3000B0D0'
        with self.assertRaisesRegex(ValueError,'bridge'):
            firmware_lift.validate_placement(profile,205632)

    def test_runtime_hooks_and_fixture_use_relocated_entries(self):
        base=self.profile['base']
        source=(firmware_lift.ROOT/'src/stubs.cpp').read_text()
        fixture=(firmware_lift.ROOT/'tests/edge_queue_fixture.cpp').read_text()
        for offset in (0x127CC,0x171B8,0x174F8,0x178D8):
            self.assertIn(f'mag_sre_func_{base+offset:08X}',source)
            self.assertIn(f'ppu_register_function(0x{base+offset:08X}',source)
        for offset in (0x17134,0x171B8,0x174F8):
            self.assertIn(f'mag_sre_func_{base+offset:08X}',fixture)
        self.assertIn(f'c.gpr[2]=0x{self.profile["toc"]:08X}',fixture)
