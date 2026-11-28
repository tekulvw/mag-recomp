"""Synthetic strict 2.12 build provenance with the v1 default left intact."""
from dataclasses import replace
import json
from pathlib import Path
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.prepare_inputs import sha256_file
from tools import provenance
from tools import targets


class V212ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'profiles').mkdir()
        (self.root / 'src').mkdir()
        (self.root / 'CMakeLists.txt').write_text('project(synthetic)')
        self.images, identities = {}, {}
        for name, source_name in [('EBOOT', 'EBOOT.BIN'), ('mage_g', 'bin/mage_g.self')]:
            elf = self.root / (name + '.elf')
            elf.write_text('synthetic ' + name)
            pair = ('a' * 64, sha256_file(elf))
            identities[source_name] = pair
            self.images[name] = dict(elf=str(elf), elf_sha256=pair[1], source_sha256=pair[0])
        self.profile = dict(target='retail-v2.12', title_id='BCUS98110', app_version='02.12', images=self.images)
        (self.root / 'profiles/retail-v2.12.json').write_text(json.dumps(self.profile))
        framework = dict(allowed_runtime_revisions=['runtime'], ps3recomp={'lifter-ppu-v212': {'commit': 'lifter212'}, 'runtime': {'commit': 'runtime'}})
        (self.root / 'profiles/framework.json').write_text(json.dumps(framework))
        self.lift = self.root / 'lift'
        self.lift.mkdir()
        source = self.lift / 'ppu_recomp.cpp'
        source.write_text('void synthetic() {}')
        self.record = dict(status='complete', exit_code=0, audit_exit=0, target='retail-v2.12',
                           sdk_revision='lifter212', lifter_role='lifter-ppu-v212',
                           elf_sha256=self.images['EBOOT']['elf_sha256'], outputs={source.name: sha256_file(source)})
        self.save_lift()
        self.binary = self.root / 'runner'
        self.binary.write_text('synthetic executable')
        selected = replace(targets.get_target('retail-v2.12'), executables=identities)
        self.addCleanup(patch.stopall)
        patch('tools.targets._TARGETS', {**targets._TARGETS, 'retail-v2.12': selected}).start()
        patch('tools.provenance.ROOT', self.root).start()
        patch('tools.prepare_inputs.ROOT', self.root).start()
        patch('tools.prepare_inputs.WORKSPACE', self.root).start()
        patch('tools.provenance.subprocess.check_output', side_effect=lambda args, **kw: 'runtime\n' if 'rev-parse' in args else '').start()

    def save_lift(self):
        (self.lift / 'provenance.json').write_text(json.dumps(self.record))

    def test_v212_build_binds_profile_pair_and_reviewed_lifter(self):
        image, lift = provenance.validate_build_image(self.lift, target='retail-v2.12')
        self.assertEqual(image, self.images['EBOOT'])
        self.assertEqual(lift['sdk_revision'], 'lifter212')
        self.record['sdk_revision'] = 'old-v1-lifter'
        self.save_lift()
        with self.assertRaises(ValueError):
            provenance.validate_build_image(self.lift, target='retail-v2.12')

    def test_only_reviewed_nonlocal_record_survives_build_validation(self):
        review=dict(target='retail-v2.12',profile_sha256='b'*64)
        self.record.update(nonlocal_control_flow=review,nonlocal_profile_sha256='b'*64)
        self.save_lift();seen=[]
        module=SimpleNamespace(validate_record=lambda record,elf_hash:seen.append((record,elf_hash)))
        with patch.dict(sys.modules,{'tools.v212_nonlocal_profile':module}):
            provenance.validate_lift(self.lift,self.images['EBOOT']['elf_sha256'],target='retail-v2.12')
            self.assertEqual(seen,[(review,self.images['EBOOT']['elf_sha256'])])
            self.record['nonlocal_profile_sha256']='c'*64;self.save_lift()
            with self.assertRaises(ValueError):
                provenance.validate_lift(self.lift,self.images['EBOOT']['elf_sha256'],target='retail-v2.12')
            self.record.update(nonlocal_profile_sha256='b'*64,nonlocal_foreign=False);self.save_lift()
            with self.assertRaises(ValueError):
                provenance.validate_lift(self.lift,self.images['EBOOT']['elf_sha256'],target='retail-v2.12')

    def test_missing_target_stale_hooks_and_incomplete_pair_reject(self):
        for changes in [dict(target='retail-v1.0'), dict(target=None),
                        dict(render_adapter_hooks={'stages': ['draw']}), dict(nonlocal_control_flow={'old': True})]:
            with self.subTest(changes=changes):
                bad = {**self.record, **changes}
                (self.lift / 'provenance.json').write_text(json.dumps(bad))
                with self.assertRaises(ValueError):
                    provenance.validate_build_image(self.lift, target='retail-v2.12')
        self.save_lift()
        self.profile['images'].pop('mage_g')
        (self.root / 'profiles/retail-v2.12.json').write_text(json.dumps(self.profile))
        with self.assertRaises(ValueError):
            provenance.validate_build_image(self.lift, target='retail-v2.12')

    def test_v212_rejects_another_allowlisted_runtime_revision(self):
        path = self.root / 'profiles/framework.json'
        framework = json.loads(path.read_text())
        framework['allowed_runtime_revisions'] = ['runtime', 'older-reviewed']
        path.write_text(json.dumps(framework))
        with patch('tools.provenance.subprocess.check_output', side_effect=lambda args, **kw: 'older-reviewed\n' if 'rev-parse' in args else ''):
            with self.assertRaises(ValueError):
                provenance.record_build(self.binary, self.root, self.lift, target='retail-v2.12')
        self.assertFalse(self.binary.with_suffix('.receipt.json').exists())

    def test_native_engine_requires_reviewed_nonlocal_flow(self):
        self.record['elf_sha256']=self.images['mage_g']['elf_sha256'];self.save_lift()
        with self.assertRaisesRegex(ValueError,'nonlocal'):
            provenance.validate_build_image(self.lift,image_name='mage_g',target='retail-v2.12')

    def test_v212_firmware_validation_uses_explicit_shared_profile_without_title_review(self):
        profile = dict(image_sha256='synthetic-firmware-identity')
        (self.root / 'profiles/retail-v1.0-firmware.json').write_text(json.dumps(profile))
        directory = self.root / 'firmware'
        directory.mkdir()
        source = directory / 'firmware.cpp'
        source.write_text('void synthetic_firmware() {}')
        record = dict(status='complete', profile=profile, executed_todo_policy='halt',
                      outputs={source.name: sha256_file(source)})
        (directory / 'provenance.json').write_text(json.dumps(record))
        result = provenance.validate_modules(self.images['EBOOT'], 'EBOOT', None, directory, target='retail-v2.12')
        self.assertEqual(result['firmware_provenance']['profile'], profile)
        self.assertFalse(result['launcher_network_hooks'])
        source.write_text('changed')
        with self.assertRaises(ValueError):
            provenance.validate_modules(self.images['EBOOT'], 'EBOOT', None, directory, target='retail-v2.12')

    def test_v212_receipt_is_explicit_experimental_and_never_runtime_acceptance(self):
        receipt = provenance.record_build(self.binary, self.root, self.lift, target='retail-v2.12')
        self.assertEqual(receipt['target'], 'retail-v2.12')
        self.assertTrue(receipt['experimental'])
        self.assertTrue(receipt['not_runtime_verified'])
        self.assertFalse(receipt['launcher_network_hooks'])
        provenance.validate_receipt(self.binary, self.images['EBOOT']['elf_sha256'], target='retail-v2.12')
        with self.assertRaises(ValueError):
            provenance.validate_receipt(self.binary, self.images['EBOOT']['elf_sha256'])


if __name__ == '__main__':
    unittest.main()
