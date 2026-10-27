import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from tools.prepare_inputs import sha256_file
from tools.provenance import validate_lift, validate_receipt, validate_spu, validate_generated, record_build


class BuildImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        (self.root/'profiles').mkdir()
        (self.root/'src').mkdir()
        (self.root/'CMakeLists.txt').write_text('project(fixture)')
        self.binary=self.root/'runner'; self.binary.write_text('native fixture')
        self.images={}
        self.lifts={}
        for name in ('EBOOT','mage_g'):
            elf=self.root/(name+'.elf'); elf.write_text(name+' ELF')
            self.images[name]={'elf':str(elf),'elf_sha256':sha256_file(elf)}
            lift=self.root/name; lift.mkdir(); src=lift/'ppu_recomp.cpp'; src.write_text(name+' code')
            (lift/'provenance.json').write_text(json.dumps(dict(status='complete',exit_code=0,audit_exit=0,
                elf_sha256=sha256_file(elf),outputs={src.name:sha256_file(src)})))
            self.lifts[name]=lift
        (self.root/'profiles/retail-v1.0.json').write_text(json.dumps({'images':self.images}))
        (self.root/'profiles/framework.json').write_text(json.dumps({'allowed_runtime_revisions':['reviewed']}))
        self.addCleanup(patch.stopall)
        patch('tools.provenance.ROOT',self.root).start()
        # Git only supplies the reviewed SDK revision; all image/lift/receipt IO is real.
        patch('tools.provenance.subprocess.check_output',side_effect=lambda args,**kw: 'reviewed\n' if 'rev-parse' in args else '').start()

    def test_default_build_remains_eboot(self):
        result=record_build(self.binary,self.root,self.lifts['EBOOT'])
        self.assertEqual(result['elf_sha256'],self.images['EBOOT']['elf_sha256'])
        validate_receipt(self.binary,self.images['EBOOT']['elf_sha256'])

    def test_engine_receipt_identifies_selected_image(self):
        result=record_build(self.binary,self.root,self.lifts['mage_g'],image_name='mage_g')
        self.assertEqual(result['image_name'],'mage_g')
        validate_receipt(self.binary,self.images['mage_g']['elf_sha256'])
        with self.assertRaises(ValueError): validate_receipt(self.binary,self.images['EBOOT']['elf_sha256'])

    def test_native_graphics_pins_rust_archive_all_c_adapters_and_private_guards(self):
        paths=['Cargo.toml','Cargo.lock','native/graphics/Cargo.toml',
               'native/graphics/CMakeLists.txt','native/graphics/src/lib.rs',
               'native/graphics/include/native.h','native/graphics/live_heap.c',
               'native/graphics/live_material.c']
        for name in paths:
            source=self.root/name;source.parent.mkdir(parents=True,exist_ok=True)
            source.write_text('synthetic '+name)
        archive=self.root/'native.a';archive.write_bytes(b'synthetic archive')
        heap=self.root/'heap.expected';heap.write_bytes(b'synthetic heap guard')
        material=self.root/'material.expected';material.write_bytes(b'synthetic material guard')
        result=record_build(self.binary,self.root,self.lifts['EBOOT'],
            native_graphics_archive=archive,native_graphics_expected=heap,
            native_material_expected=material)['native_graphics']
        self.assertEqual(result['archive_sha256'],sha256_file(archive))
        self.assertEqual(result['private_expected_registration_sha256'],sha256_file(heap))
        self.assertEqual(result['private_material_registration_sha256'],sha256_file(material))
        for name in paths:
            self.assertEqual(result['sources'][name],sha256_file(self.root/name))

    def test_engine_rejects_launcher_lift(self):
        with self.assertRaisesRegex(ValueError,'selected ELF'):
            record_build(self.binary,self.root,self.lifts['EBOOT'],image_name='mage_g')
        self.assertFalse(self.binary.with_suffix('.receipt.json').exists())

    def module_fixture(self):
        worker=self.root/'worker.elf'; worker.write_bytes(b'complete SPU image')
        elf=Path(self.images['mage_g']['elf']); offset=elf.stat().st_size
        elf.write_bytes(elf.read_bytes()+worker.read_bytes())
        self.images['mage_g']['elf_sha256']=sha256_file(elf)
        (self.root/'profiles/retail-v1.0.json').write_text(json.dumps({'images':self.images}))
        lift_path=self.lifts['mage_g']/'provenance.json'
        lift=json.loads(lift_path.read_text()); lift['elf_sha256']=sha256_file(elf)
        lift_path.write_text(json.dumps(lift))
        dirs={}
        for kind in ('spu','firmware'):
            profile={'image':worker.name,'image_sha256':sha256_file(worker),'image_bytes':worker.stat().st_size}
            (self.root/f'profiles/retail-v1.0-{kind}.json').write_text(json.dumps(profile))
            directory=self.root/('generated-'+kind); directory.mkdir(); src=directory/'module.cpp';src.write_text(kind+' source')
            (directory/'provenance.json').write_text(json.dumps(dict(status='complete',profile=profile,
                unsupported_count=0,executed_todo_policy='halt',outputs={src.name:sha256_file(src)})))
            dirs[kind]=directory
        review=dict(image_name='mage_g',elf_sha256=sha256_file(elf),
            embedded_spu=dict(offset=offset,size=worker.stat().st_size,sha256=sha256_file(worker)),
            shared_profiles={kind:sha256_file(self.root/f'profiles/retail-v1.0-{kind}.json') for kind in dirs})
        review_path=self.root/'profiles/retail-v1.0-engine-modules.json'
        review_path.write_text(json.dumps(review))
        return dirs,review,review_path

    def test_engine_accepts_only_reviewed_shared_modules(self):
        dirs,review,path=self.module_fixture()
        result=record_build(self.binary,self.root,self.lifts['mage_g'],image_name='mage_g',**dirs)
        self.assertEqual(result['module_review'],review)
        self.assertFalse(result['launcher_network_hooks'])
        self.assertIn('firmware_provenance',result)
        self.assertIn('spu_provenance',result)

    def test_engine_rejects_missing_review_before_receipt(self):
        dirs,review,path=self.module_fixture(); path.unlink()
        with self.assertRaisesRegex(ValueError,'review'):
            record_build(self.binary,self.root,self.lifts['mage_g'],image_name='mage_g',**dirs)
        self.assertFalse(self.binary.with_suffix('.receipt.json').exists())

    def test_engine_selects_own_spu_profile_without_changing_launcher(self):
        dirs,review,path=self.module_fixture()
        profile={'image_name':'mage_g','elf_sha256':self.images['mage_g']['elf_sha256'],
                 'executed_todo_policy':'halt','expected_unsupported_count':2,'policy':{'image_id':101}}
        specific=self.root/'profiles/retail-v1.0-engine-spu.json';specific.write_text(json.dumps(profile))
        review['image_profiles']={'spu':{'path':'profiles/retail-v1.0-engine-spu.json','sha256':sha256_file(specific)}}
        path.write_text(json.dumps(review))
        record_path=dirs['spu']/'provenance.json';record=json.loads(record_path.read_text())
        record.update(profile=profile,unsupported_count=2,guarded_todo_count=2,executed_todo_policy='halt')
        record_path.write_text(json.dumps(record))
        result=record_build(self.binary,self.root,self.lifts['mage_g'],image_name='mage_g',**dirs)
        self.assertEqual(result['spu_provenance']['profile'],profile)
        with self.assertRaises(ValueError):
            record_build(self.binary,self.root,self.lifts['EBOOT'],spu=dirs['spu'])
        for key,value in [('guarded_todo_count',1),('executed_todo_policy','ignore')]:
            invalid={**record,key:value};record_path.write_text(json.dumps(invalid))
            with self.assertRaises(ValueError):
                record_build(self.binary,self.root,self.lifts['mage_g'],image_name='mage_g',**dirs)

    def test_engine_rejects_other_image_worker_or_profile(self):
        dirs,review,path=self.module_fixture()
        import copy
        changes=[('elf_sha256','wrong'),('image_name','EBOOT'),
                 ('embedded_spu.offset',0),('embedded_spu.sha256','wrong'),
                 ('embedded_spu.size',1),('shared_profiles.firmware','wrong'),
                 ('shared_profiles.spu','wrong')]
        for key,value in changes:
            with self.subTest(key=key):
                invalid=copy.deepcopy(review); target=invalid;parts=key.split('.')
                for part in parts[:-1]:target=target[part]
                target[parts[-1]]=value;path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    record_build(self.binary,self.root,self.lifts['mage_g'],image_name='mage_g',**dirs)
                self.assertFalse(self.binary.with_suffix('.receipt.json').exists())

    def test_changed_selected_elf_cannot_receive_receipt(self):
        Path(self.images['EBOOT']['elf']).write_text('changed ELF')
        with self.assertRaises(ValueError): record_build(self.binary,self.root,self.lifts['EBOOT'])
        self.assertFalse(self.binary.with_suffix('.receipt.json').exists())

class ProvenanceTests(unittest.TestCase):
    def test_engine_requires_separate_training_census_even_if_aggregate_matches(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);src=root/'task.c';src.write_text('synthetic generated code')
            profile=dict(executed_todo_policy='halt',expected_unsupported_count=4,
                         policy=dict(expected_unsupported_count=4),
                         training_task=dict(expected_unsupported_count=0),queued_training_task=dict(expected_unsupported_count=0))
            record=dict(status='complete',profile=profile,unsupported_count=4,
                        guarded_todo_count=4,executed_todo_policy='halt',
                        policy_unsupported_count=4,training_unsupported_count=0,queued_training_unsupported_count=0,
                        outputs={src.name:sha256_file(src)})
            path=root/'provenance.json';path.write_text(json.dumps(record));validate_spu(root,profile)
            for policy_count,training_count in [(3,1),(0,4),(True,3)]:
                bad={**record,'policy_unsupported_count':policy_count,'training_unsupported_count':training_count}
                path.write_text(json.dumps(bad))
                with self.subTest(policy=policy_count,training=training_count),self.assertRaises(ValueError):
                    validate_spu(root,profile)
            for queued_count in [1,-1,True,None]:
                bad={**record,'queued_training_unsupported_count':queued_count};path.write_text(json.dumps(bad))
                with self.subTest(queued=queued_count),self.assertRaises(ValueError):validate_spu(root,profile)
            bad=record.copy();del bad['queued_training_unsupported_count'];path.write_text(json.dumps(bad))
            with self.assertRaises(ValueError):validate_spu(root,profile)
            bad=record.copy();del bad['training_unsupported_count'];path.write_text(json.dumps(bad))
            with self.assertRaises(ValueError):validate_spu(root,profile)

    def test_firmware_requires_halt_policy_and_all_compiled_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);src=root/'firmware.cpp';src.write_text('guarded code')
            record=dict(status='complete',profile={'image_sha256':'firmware'},
                        outputs={src.name:sha256_file(src)})
            def save(): (root/'provenance.json').write_text(json.dumps(record))
            save()
            with self.assertRaises(ValueError): validate_generated(root,record['profile'],'firmware')
            record['executed_todo_policy']='halt';save()
            validate_generated(root,record['profile'],'firmware')
            (root/'foreign.cpp').write_text('unrecorded')
            with self.assertRaises(ValueError): validate_generated(root,record['profile'],'firmware')

    def test_spu_manifest_binds_nested_outputs_and_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'worker').mkdir()
            src=root/'worker/spu_recomp.c'; src.write_text('lifted worker')
            profile={'image_sha256':'image','image_id':100,'code_end':43696}
            record=dict(status='complete',profile=profile,unsupported_count=0,
                        outputs={'worker/spu_recomp.c':sha256_file(src)})
            (root/'provenance.json').write_text(json.dumps(record))
            validate_spu(root,profile)
            src.write_text('modified')
            with self.assertRaises(ValueError): validate_spu(root,profile)
            src.write_text('lifted worker')
            with self.assertRaises(ValueError): validate_spu(root,{**profile,'image_sha256':'other'})
            (root/'unrecorded.c').write_text('foreign')
            with self.assertRaises(ValueError): validate_spu(root,profile)

    def test_spu_rejects_incomplete_or_unsupported_lift(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);src=root/'worker.c';src.write_text('code')
            for status,count in [('failed',0),('complete',1)]:
                (root/'provenance.json').write_text(json.dumps(dict(status=status,profile={},
                    unsupported_count=count,outputs={'worker.c':sha256_file(src)})))
                with self.assertRaises(ValueError): validate_spu(root,{})

    def test_failed_or_incomplete_audit_cannot_create_a_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); src=root/'ppu_recomp_0.cpp'; src.write_text('original')
            for status,exit_code,audit_exit in [('failed',0,1),('complete',0,1),
                                               ('complete',1,0),(None,0,None)]:
                with self.subTest(status=status,exit_code=exit_code,audit_exit=audit_exit):
                    (root/'provenance.json').write_text(json.dumps(dict(elf_sha256='expected',
                        outputs={src.name:sha256_file(src)},status=status,exit_code=exit_code,audit_exit=audit_exit)))
                    with self.assertRaises(ValueError): validate_lift(root,'expected')

    def test_modified_generated_source_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); src=root/'ppu_recomp_0.cpp'; src.write_text('original')
            (root/'provenance.json').write_text(json.dumps(dict(elf_sha256='expected',status='complete',exit_code=0,audit_exit=0,
                outputs={src.name:sha256_file(src)})))
            validate_lift(root,'expected')
            src.write_text('changed')
            with self.assertRaises(ValueError): validate_lift(root,'expected')

    def test_unlisted_generated_source_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); src=root/'ppu_recomp_0.cpp'; src.write_text('original')
            (root/'provenance.json').write_text(json.dumps(dict(elf_sha256='expected',status='complete',exit_code=0,audit_exit=0,
                outputs={src.name:sha256_file(src)})))
            (root/'foreign.cpp').write_text('wrong build')
            with self.assertRaises(ValueError): validate_lift(root,'expected')

    def test_changed_binary_or_image_cannot_reuse_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'runner';binary.write_text('old')
            binary.with_suffix('.receipt.json').write_text(json.dumps(dict(
                binary_sha256=sha256_file(binary),elf_sha256='expected')))
            validate_receipt(binary,'expected')
            with self.assertRaises(ValueError): validate_receipt(binary,'other')
            binary.write_text('new')
            with self.assertRaises(ValueError): validate_receipt(binary,'expected')
