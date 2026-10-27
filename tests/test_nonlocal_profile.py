import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from tools import lift
from tools.prepare_inputs import sha256_file

class NonlocalProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);(self.root/'profiles').mkdir()
        self.elf=self.root/'mage_g.elf';self.elf.write_bytes(b'verified-image')
        self.config=self.root/'config.json';self.config.write_text('{}')
        self.profile=self.root/'profile.json'
        self.record={'image_name':'mage_g','elf_sha256':sha256_file(self.elf),'sdk_revision':'reviewed',
                     'configuration':'config.json','configuration_sha256':sha256_file(self.config)}
        self.profile.write_text(json.dumps(self.record))
        (self.root/'profiles/framework.json').write_text(json.dumps({'allowed_lifter_revisions':['reviewed']}))
    def check(self, revision='reviewed', dirty=''):
        with patch('tools.lift.ROOT',self.root),patch('tools.lift.subprocess.check_output',side_effect=[revision,dirty]):
            return lift.validate_nonlocal_profile(self.profile,self.root/'sdk',self.elf,'mage_g')
    def test_accepts_exact_reviewed_profile(self):
        config,record=self.check();self.assertEqual(config,self.config);self.assertEqual(record,self.record)
    def test_substituted_config_stops_before_generation(self):
        self.config.write_text('{"foreign":"configuration"}')
        with self.assertRaises(ValueError):self.check()
    def test_wrong_image_or_revision_is_rejected(self):
        for field,value in [('image_name','EBOOT'),('elf_sha256','0'*64),('sdk_revision','unreviewed')]:
            with self.subTest(field=field):
                record=dict(self.record);record[field]=value;self.profile.write_text(json.dumps(record))
                with self.assertRaises(ValueError):self.check()
    def test_dirty_or_different_generator_is_rejected(self):
        for revision,dirty in [('foreign',''),('reviewed',' M tools/ppu_lifter.py')]:
            with self.assertRaises(ValueError):self.check(revision,dirty)
    def test_configuration_must_be_local_regular_file(self):
        for name in ['../config.json','/tmp/config.json']:
            record=dict(self.record);record['configuration']=name;self.profile.write_text(json.dumps(record))
            with self.assertRaises(ValueError):self.check()

    def test_symlink_configuration_is_rejected(self):
        target=self.root/'target.json';target.write_bytes(self.config.read_bytes())
        self.config.unlink();self.config.symlink_to(target)
        with self.assertRaises(ValueError):self.check()
