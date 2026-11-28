import hashlib
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import v212_engine_profile as engine
from tests.test_v212_launcher_profile import elf_image


class EngineProfileTests(unittest.TestCase):
    def test_metadata_is_independent_engine_identity_and_indirect_settings_entry(self):
        profile = engine.load_profile()
        self.assertEqual(profile['image_name'], 'mage_g')
        self.assertEqual(profile['symbols']['settings_reader'], 0x449f88)
        self.assertEqual(profile['symbols']['account_setter'], 0x109fa8)
        self.assertEqual(profile['layout']['config_root'], 0x163c178)
        self.assertEqual(profile['dispatch']['settings_opd'], 0x15b9fa8)
        self.assertEqual(profile['dispatch']['direct_branch_references'], 0)
        self.assertFalse(profile['policy']['rewrite_effective_ticket'])
        self.assertFalse(profile['runtime_accepted'])
        self.assertEqual(profile['symbols']['final_connect'], 0x156cb4)
        self.assertEqual(profile['dispatch']['connect']['opd'], 0x15a1e68)
        self.assertEqual(profile['dispatch']['connect']['vtable_reference'], 0x153ea48)
        self.assertEqual(profile['authored_lua']['table'], 'MAGCharacterData')
        self.assertEqual(profile['authored_lua']['field'], 'PlayerName')

    def test_exact_image_and_span_tamper_are_rejected(self):
        data = elf_image()
        profile = engine.load_profile()
        profile['elf_sha256'] = hashlib.sha256(data).hexdigest()
        profile['dispatch']['connect']['code_start'] = 0x10000
        profile['dispatch']['connect']['code_end'] = 0x10004
        profile['guard_spans'] = [dict(name='fixture', address=0x10000, length=7,
            sha256=hashlib.sha256(b'fixture').hexdigest())]
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp)/'engine.elf'
            image.write_bytes(data)
            with patch.object(engine, 'load_profile', return_value=profile):
                self.assertEqual(engine.verify_engine(image)['guard_spans_verified'], 1)
                image.write_bytes(elf_image(b'changed identity'))
                with self.assertRaisesRegex(ValueError, 'identity'):
                    engine.verify_engine(image)
                profile['elf_sha256'] = hashlib.sha256(image.read_bytes()).hexdigest()
                with self.assertRaisesRegex(ValueError, 'guard'):
                    engine.verify_engine(image)

    def test_connect_branch_census_rejects_direct_bypass(self):
        target=0x10010
        for instruction in [0x48000010,0x48000011,0x48010012,0x41820010]:
            data=elf_image(struct.pack('>I',instruction))
            self.assertEqual(engine.direct_branch_references(data,0x10000,0x10004,target),[0x10000])
        data=elf_image(struct.pack('>I',0x60000000))
        self.assertEqual(engine.direct_branch_references(data,0x10000,0x10004,target),[])
