import hashlib
import struct
import tempfile
import unittest
from pathlib import Path
from tools import v212_launcher_profile as launcher


def elf_image(payload=b'fixture identity'):
    blob=bytearray(0x100+len(payload))
    blob[:16]=b'\x7fELF\x02\x02\x01'+bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH',blob,16,2,21,1,0x10000,64,0,0,64,56,1,0,0,0)
    struct.pack_into('>IIQQQQQQ',blob,64,1,5,0x100,0x10000,0x10000,len(payload),len(payload)+8,0x100)
    blob[0x100:]=payload
    return bytes(blob)


class LauncherProfileTests(unittest.TestCase):
    def test_virtual_ranges_use_file_backed_load_segments(self):
        data=elf_image()
        self.assertEqual(launcher.virtual_bytes(data,0x10000,7),b'fixture')
        for address,length in [(0x10010,1),(0xffff,2),(0xffffffff,2),(0x10000,-1)]:
            with self.assertRaises(ValueError):launcher.virtual_bytes(data,address,length)
        with self.assertRaises(ValueError):launcher.virtual_bytes(b'not elf',0x10000,1)

    def test_pinned_input_rejects_synthetic_elf_without_extracting(self):
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'fixture.elf';image.write_bytes(elf_image())
            with self.assertRaisesRegex(ValueError,'identity'):
                launcher.verify_launcher(image)

    def test_metadata_contains_only_2_12_source_bound_known_mappings(self):
        profile=launcher.load_profile()
        self.assertEqual(profile['target'],'retail-v2.12')
        self.assertEqual(profile['symbols']['ticket_use'],0x26fe00)
        self.assertEqual(profile['symbols']['account_setter'],0x594c8)
        self.assertEqual(profile['layout']['effective_ticket'],0x2289)
        self.assertEqual(profile['handoff']['guest_path'],'/dev_hdd0/game/BCUS98110/USRDIR/bin/mage_g.self')
        self.assertEqual(profile['handoff']['availability_nid'],'0xF042B14F')
        self.assertEqual(profile['handoff']['key_source']['address'],0x2c0828)
        self.assertFalse(profile['handoff']['path_selection']['fallback_accepted'])
        self.assertFalse(profile['runtime_accepted'])
        self.assertTrue(profile['guard_spans'])
        for span in profile['guard_spans']:
            self.assertEqual(len(span['sha256']),64)
            self.assertNotIn('bytes',span)

    def test_guard_digest_rejects_modified_span(self):
        data=elf_image()
        guard={'address':0x10000,'length':7,'sha256':hashlib.sha256(b'fixture').hexdigest()}
        launcher.verify_guard_spans(data,[guard])
        with self.assertRaisesRegex(ValueError,'guard'):
            launcher.verify_guard_spans(elf_image(b'changed identity'),[guard])
