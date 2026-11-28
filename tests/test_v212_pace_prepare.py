"""Synthetic bounds and identity tests for original guest libpace preparation."""
import struct
import unittest
from pathlib import Path
import tempfile
from tools.v212_pace_prepare import generate
from types import SimpleNamespace
from tools.v212_pace_prepare import relocate_prx, validate_placement, import_stubs


class PacePreparationTests(unittest.TestCase):
    def test_unknown_module_is_rejected_before_sdk_or_output_creation(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            source=root/'unknown.prx';source.write_bytes(b'synthetic unreviewed module')
            output=root/'out'
            with self.assertRaisesRegex(ValueError, 'identity mismatch'):
                generate(source,output,sdk=root/'missing-sdk')
            self.assertFalse(output.exists())

    def prx(self, entries):
        payload = bytearray(0x100)
        for offset, value_segment, address_segment, kind, ptr in entries:
            payload.extend(struct.pack('>QHBBIQ', offset, 0, value_segment, address_segment, kind, ptr))
        return SimpleNamespace(data=bytes(payload), loads=[(0, 0, 0x80, 0x100)],
                               rela=(0x100, len(entries)*24))

    def test_addr64_writes_full_big_endian_value_and_preserves_source(self):
        prx = self.prx([(0x20, 0, 0, 38, 0x1234)])
        original = prx.data
        image, counts = relocate_prx(prx, 0x21200000)
        self.assertEqual(struct.unpack_from('>Q', image, 0x20)[0], 0x21201234)
        self.assertEqual(counts, {38: 1})
        self.assertEqual(prx.data, original)

    def test_reviewed_addr32_hi_ha_lo_relocations_remain_correct(self):
        prx = self.prx([(0x20, 0, 0, 1, 0xABCD), (0x24, 0, 0, 4, 0xABCD),
                        (0x26, 0, 0, 5, 0xABCD), (0x28, 0, 0, 6, 0xABCD)])
        image, counts = relocate_prx(prx, 0x21200000)
        self.assertEqual(struct.unpack_from('>IHHH', image, 0x20), (0x2120ABCD, 0xABCD, 0x2120, 0x2121))
        self.assertEqual(sum(counts.values()), 4)

    def test_unknown_relocation_bad_segment_and_out_of_bounds_fail(self):
        for entry in [(0x20, 0, 0, 99, 0), (0x20, 1, 0, 1, 0),
                      (0x20, 0, 1, 1, 0), (0xFC, 0, 0, 38, 0)]:
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                relocate_prx(self.prx([entry]), 0x21200000)

    def test_fixed_module_reservation_rejects_size_or_base_change(self):
        for base, size in [(0x21200000, 0), (0x21000000, 0x100), (0x21200000, 0x100001)]:
            with self.subTest(base=base, size=size), self.assertRaises(ValueError):
                validate_placement(base, size)
        validate_placement(0x21200000, 0x40648)

    def test_import_stub_addresses_remain_the_original_executable_entries(self):
        image = bytearray(0x100)
        struct.pack_into('>II', image, 0x40, 0x21200020, 0x21200080)
        stubs = import_stubs(bytes(image), {'sysPrxForUser': {'0x12345678': '0x21200040'}}, 0x21200000)
        self.assertEqual(stubs, [{'library': 'sysPrxForUser', 'nid': '0x12345678', 'stub': '0x21200040'}])


class PacePreparedCheckTests(unittest.TestCase):
    def setUp(self):
        import contextlib
        import json
        from unittest.mock import patch
        from tools import v212_pace_prepare as prepare
        from tools.targets import get_target
        from tools.prepare_inputs import sha256_file
        self.prepare=prepare
        self.sha=sha256_file
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.output=self.root/'prepared';self.output.mkdir()
        self.sdk=self.root/'sdk'
        (self.root/'profiles').mkdir()
        profile=json.loads((prepare.ROOT/'profiles/retail-v2.12-pace.json').read_text())
        image=b'synthetic relocated image'
        profile['variants']['libpace_g']['image_size']=len(image)
        profile['variants']['libpace_g']['relocated_image_sha256']=__import__('hashlib').sha256(image).hexdigest()
        for relative in profile['sdk_tools']:
            path=self.sdk/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('synthetic SDK source '+relative)
            profile['sdk_tools'][relative]=self.sha(path)
        self.profile=profile
        (self.root/'profiles/retail-v2.12-pace.json').write_text(json.dumps(profile))
        self.framework={'ps3recomp':{'lifter-ppu-v212':{'commit':'reviewed-sdk','tag':'reviewed-tag','path':'sdk'}}}
        self.framework_path=self.root/'profiles/framework.json';self.framework_path.write_text(json.dumps(self.framework))
        images={Path(n).stem:{'source_sha256':pair[0],'elf_sha256':pair[1],
                              'load_segments':[{'vaddr':0x10000,'memsz':0x1000}]}for n,pair in get_target('retail-v2.12').executables.items()}
        (self.root/'profiles/retail-v2.12.json').write_text(json.dumps(dict(target='retail-v2.12',title_id='BCUS98110',app_version='02.12',images=images)))
        for name in ('pace_module.cpp','pace_image.c','mag_pace_recomp.h','ppu_recomp_000.cpp'):
            (self.output/name).write_text('// synthetic prepared source\n')
        inputs=self.output/'inputs';inputs.mkdir()
        (inputs/'pace_image.bin').write_bytes(image)
        for name in ('pace_functions.json','pace_exports.json','pace_imports.json','hle-stubs.json'):
            (inputs/name).write_text('{}')
        self.record=dict(status='complete',target='retail-v2.12',variant='libpace_g',
                         elf_sha256=profile['variants']['libpace_g']['elf_sha256'],
                         self_sha256=profile['variants']['libpace_g']['self_sha256'],
                         profile_sha256=self.sha(self.root/'profiles/retail-v2.12-pace.json'),
                         generator_sha256=self.sha(Path(prepare.__file__)),framework_sha256=self.sha(self.framework_path),
                         sdk_revision='reviewed-sdk',lifter_role='lifter-ppu-v212',sdk_tools=dict(profile['sdk_tools']),
                         base=0x21200000,image_size=len(image),executed_unknown_policy='halt',runtime_verified=False,
                         todo_by_mnemonic={},
                         outputs={p.name:self.sha(p)for p in self.output.iterdir()if p.is_file()},
                         inputs={p.relative_to(self.output).as_posix():self.sha(p)for p in inputs.iterdir()})
        self.write_record()
        self.stack=contextlib.ExitStack();self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(prepare,'ROOT',self.root))
        self.stack.enter_context(patch.object(prepare,'WORKSPACE',self.root))
        self.stack.enter_context(patch.object(prepare,'data_path',side_effect=lambda relative:self.root/relative))
        self.stack.enter_context(patch.object(prepare,'load_framework',return_value=(self.framework,self.framework_path)))
        self.sdk_check=self.stack.enter_context(patch.object(prepare,'check_sdk'))

    def write_record(self):
        import json
        (self.output/'provenance.json').write_text(json.dumps(self.record))

    def test_complete_primary_preparation_returns_exact_sources_and_cmake_paths(self):
        receipt,sources=self.prepare.check(self.output,self.sdk)
        self.assertEqual(receipt,self.record)
        self.assertEqual({p.name for p in sources},{'pace_image.c','pace_module.cpp','ppu_recomp_000.cpp'})
        destination=self.root/'pace-sources.cmake'
        self.prepare.write_sources(destination,self.output,sources)
        text=destination.read_text()
        for name in ('MAG_V212_PACE_SOURCES','MAG_V212_PACE_INCLUDE_DIRS','MAG_V212_PACE_PROVENANCE'):
            self.assertIn(name,text)
        self.assertIn(self.output.as_posix(),text)
        self.sdk_check.assert_called_once_with(self.sdk,expected_revision='reviewed-sdk',role='lifter-ppu-v212')

    def test_wrong_identity_policy_or_placement_is_rejected(self):
        for field,value in [('status','failed'),('target','retail-v1.0'),('variant','libpacePrx_g'),
                            ('elf_sha256','0'*64),('self_sha256','0'*64),('profile_sha256','0'*64),
                            ('generator_sha256','0'*64),('framework_sha256','0'*64),
                            ('sdk_revision','foreign'),('lifter_role','runtime'),
                            ('base',0x21000000),('image_size',0x100001),
                            ('executed_unknown_policy','ignore'),('runtime_verified',True)]:
            original=self.record[field];self.record[field]=value;self.write_record()
            with self.subTest(field=field),self.assertRaises(ValueError):
                self.prepare.check(self.output,self.sdk)
            self.record[field]=original
        self.write_record()

    def test_changed_or_missing_output_or_unlisted_injected_source_is_rejected(self):
        path=self.output/'ppu_recomp_000.cpp';original=path.read_bytes()
        path.write_text('// changed')
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)
        path.unlink()
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)
        path.write_bytes(original)
        (self.output/'foreign.cpp').write_text('// injected')
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)
        # Even an edited receipt cannot authorize an unknown source filename.
        self.record['outputs']['foreign.cpp']=self.sha(self.output/'foreign.cpp');self.write_record()
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)

    def test_symlink_output_is_rejected_even_with_matching_hash(self):
        path=self.output/'pace_image.c';copy=self.root/'outside.c';copy.write_bytes(path.read_bytes())
        path.unlink();path.symlink_to(copy)
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)

    def test_sdk_source_and_relocated_image_tampering_is_rejected(self):
        path=self.sdk/'tools/ppu_lifter.py';original=path.read_bytes();path.write_text('changed SDK source')
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)
        path.write_bytes(original)
        image=self.output/'inputs/pace_image.bin';image.write_bytes(b'x'*self.record['image_size'])
        self.record['inputs']['inputs/pace_image.bin']=self.sha(image);self.write_record()
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)

    def test_rehashed_source_cannot_remove_executed_unknown_guard(self):
        path=self.output/'ppu_recomp_000.cpp';path.write_text('/* TODO: .word 0x00000000 */;\n')
        self.record['outputs'][path.name]=self.sha(path)
        self.record['todo_by_mnemonic']={'.word':1};self.write_record()
        with self.assertRaises(ValueError):self.prepare.check(self.output,self.sdk)

    def test_sources_output_cannot_overwrite_a_validated_source(self):
        receipt,sources=self.prepare.check(self.output,self.sdk)
        path=self.output/'pace_module.cpp';original=path.read_bytes()
        with self.assertRaises(ValueError):self.prepare.write_sources(path,self.output,sources)
        self.assertEqual(path.read_bytes(),original)


if __name__ == '__main__':
    unittest.main()
