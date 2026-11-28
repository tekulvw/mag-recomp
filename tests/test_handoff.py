import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from tools.handoff import capture_handoff

class HandoffTests(unittest.TestCase):
    def run_fixture(self, root, code, request=True, token='a'*32, seconds=2, limit=8192, child='print("engine")'):
        run=Path(root)
        mapping={'token':'a'*32,'request':str(run/'request.json'),
                 'guest_path':'/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self',
                 'command':[sys.executable,'-c',child]}
        record={'version':1,'token':token,'guest_path':mapping['guest_path'],'kind':'mage_g'}
        script='import json,sys,time; '
        if request: script+=f'open({mapping["request"]!r},"w").write({json.dumps(record)!r}); '
        script+=f'sys.exit({code})'
        env=os.environ.copy();env['MAG_LOCAL_ACCOUNT_NAME']='Player';env['PS3_VFS_ROOT']=root
        with patch('tools.handoff.validate_mapping',return_value={'sdk_revision':'engine'}) as validate:
            result=capture_handoff([sys.executable,'-c',script],run,env,run/'console.log',seconds,mapping,limit)
        return result,validate

    def test_exact_request_starts_fresh_engine_with_shared_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            r,v=self.run_fixture(tmp,90,child='import os; assert "MAG_LOCAL_ACCOUNT_NAME" not in os.environ; assert os.environ["PS3_BOOT_PATH"].endswith("mage_g.self"); print(os.environ["PS3_VFS_ROOT"])')
            self.assertTrue(r['engine_process_started']);self.assertEqual(r['returncode'],0)
            self.assertEqual(r['engine_identity'],dict(sdk_revision='engine',spu_registered=False,firmware_registered=False))
            self.assertIn(tmp,Path(r['final_log']).read_text());v.assert_called_once()

    def test_crash_reserved_exit_missing_record_and_stale_token_never_start(self):
        for code,request,token in [(1,True,'a'*32),(90,False,'a'*32),(90,True,'b'*32)]:
            with self.subTest(code=code,request=request,token=token),tempfile.TemporaryDirectory() as tmp:
                r,v=self.run_fixture(tmp,code,request,token)
                self.assertFalse(r['engine_process_started']);v.assert_not_called()

    def test_child_uses_remaining_deadline_and_log_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            r,_=self.run_fixture(tmp,90,seconds=.3,child='import time; time.sleep(20)')
            self.assertEqual(r['stop_reason'],'timeout');self.assertLess(r['elapsed_seconds'],1)
        with tempfile.TemporaryDirectory() as tmp:
            r,_=self.run_fixture(tmp,90,limit=4096,child='while True: print("x"*4096,flush=True)')
            self.assertEqual(r['stop_reason'],'log_limit')
            self.assertLessEqual(sum(p.stat().st_size for p in Path(tmp).glob('*console.log')),4096)

    def test_changed_mapping_stops_before_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch('tools.handoff.validate_mapping',side_effect=ValueError('changed')):
                # Own capture call here so the fixture does not replace the failing validator.
                run=Path(tmp);req=run/'r.json';token='a'*32;guest='/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self'
                req.write_text(json.dumps(dict(version=1,token=token,guest_path=guest,kind='mage_g')))
                r=capture_handoff([sys.executable,'-c','exit(90)'],run,os.environ.copy(),run/'console.log',2,
                    dict(token=token,request=str(req),guest_path=guest,command=['/no/child']))
                self.assertFalse(r['engine_process_started']);self.assertIn('changed',r['handoff_error'])

class MappingTests(unittest.TestCase):
    def test_identity_receipt_and_overlay_boundaries(self):
        from tools.handoff import validate_mapping,GUEST_ENGINE
        from tools.prepare_inputs import sha256_file
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();image=root/'PS3_GAME/USRDIR/bin/mage_g.self';image.parent.mkdir(parents=True)
            image.write_bytes(b'self');elf=root/'engine.elf';elf.write_bytes(b'elf');binary=root/'engine';binary.write_bytes(b'binary')
            receipt=dict(image_name='mage_g',elf_sha256=sha256_file(elf),binary_sha256=sha256_file(binary),sdk_revision='strict')
            binary.with_suffix('.receipt.json').write_text(json.dumps(receipt))
            m=dict(vfs=str(root),self=str(image),elf=str(elf),binary=str(binary),guest_path=GUEST_ENGINE,
                   command=[str(binary),str(elf)],strict_revisions=['strict'])
            for key in ['self','elf','binary']:m[key+'_sha256']=sha256_file(Path(m[key]))
            self.assertEqual(validate_mapping(m),receipt)
            for key in ['self','elf','binary']:
                with self.subTest(changed=key):
                    path=Path(m[key]);old=path.read_bytes();path.write_bytes(old+b'changed')
                    with self.assertRaises(ValueError):validate_mapping(m)
                    path.write_bytes(old)
            for key,value in [('self',str(root/'other.self')),('guest_path','/dev_hdd0/engine'),('command',['/other']),('strict_revisions',[])]:
                bad=dict(m);bad[key]=value
                with self.subTest(field=key),self.assertRaises(ValueError):validate_mapping(bad)
            image.unlink();image.symlink_to(elf)
            with self.assertRaises(ValueError):validate_mapping(m)

class RequestTypeTests(unittest.TestCase):
    def test_fifo_request_is_rejected_without_blocking(self):
        import time
        with tempfile.TemporaryDirectory() as tmp:
            run=Path(tmp);request=run/'fifo';os.mkfifo(request)
            started=time.monotonic()
            r=capture_handoff([sys.executable,'-c','exit(90)'],run,os.environ.copy(),run/'console.log',.2,
                dict(request=str(request)))
            self.assertLess(time.monotonic()-started,1)
            self.assertFalse(r['engine_process_started'])
            self.assertIn('Invalid handoff request',r['handoff_error'])

class PreparedFileTypeTests(unittest.TestCase):
    def test_fifo_asset_and_receipt_reject_before_blocking(self):
        from tools.handoff import checked_hash,checked_receipt
        import time
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);asset=root/'asset';os.mkfifo(asset)
            started=time.monotonic()
            with self.assertRaises(ValueError):checked_hash(asset,'0'*64)
            binary=root/'binary';os.mkfifo(binary.with_suffix('.receipt.json'))
            with self.assertRaises(ValueError):checked_receipt(binary,'0'*64)
            self.assertLess(time.monotonic()-started,1)


class V212HandoffTests(unittest.TestCase):
    def setUp(self):
        from dataclasses import replace
        from tools import targets
        import hashlib
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.vfs=self.root/'disc';self.hdd=self.root/'hdd'
        self.vfs.mkdir();self.hdd.mkdir();(self.root/'profiles').mkdir()
        self.source=self.hdd/'game/BCUS98110/USRDIR/bin/mage_g.self'
        self.source.parent.mkdir(parents=True);self.source.write_bytes(b'synthetic engine SELF')
        self.engine=self.root/'engine.elf';self.engine.write_bytes(b'synthetic engine ELF')
        self.launcher=self.root/'launcher.elf';self.launcher.write_bytes(bytes(range(16)))
        self.binary=self.root/'engine';self.binary.write_bytes(b'synthetic native engine')
        digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        self.images={'EBOOT':dict(elf=str(self.launcher),elf_sha256=digest(self.launcher),source_sha256='a'*64),
                     'mage_g':dict(elf=str(self.engine),elf_sha256=digest(self.engine),source_sha256=digest(self.source))}
        self.profile=dict(target='retail-v2.12',title_id='BCUS98110',app_version='02.12',images=self.images)
        (self.root/'profiles/retail-v2.12.json').write_text(json.dumps(self.profile))
        self.receipt=dict(image_name='mage_g',target='retail-v2.12',experimental=True,not_runtime_verified=True,
                          sdk_revision='strict',elf_sha256=digest(self.engine),binary_sha256=digest(self.binary))
        self.save_receipt()
        identities={'EBOOT.BIN':('a'*64,digest(self.launcher)),
                    'bin/mage_g.self':(digest(self.source),digest(self.engine))}
        selected=replace(targets.get_target('retail-v2.12'),executables=identities)
        self.addCleanup(patch.stopall)
        patch('tools.targets._TARGETS',{**targets._TARGETS,'retail-v2.12':selected}).start()
        patch('tools.handoff.data_path',side_effect=lambda rel:self.root/rel).start()
        patch('tools.handoff.load_framework',return_value=({'strict_runtime_revisions':['strict'],
              'strict_hle_runtime_revisions':['strict']},None)).start()
        patch('tools.handoff.validate_ppu_elf',return_value={'load_segments':[dict(vaddr=0x2c0828,offset=0,filesz=16)]}).start()
        patch('tools.v212_launcher_profile.verify_launcher',return_value={}).start()
        patch('tools.v212_launcher_profile.load_profile',return_value={'handoff':{'key_source':dict(address=0x2c0828,length=16,sha256=digest(self.launcher))}}).start()

    def save_receipt(self):
        self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))

    def provision(self,**kw):
        from tools.handoff import provision_handoff
        return provision_handoff(self.vfs,self.root,self.binary,target='retail-v2.12',hdd0_root=self.hdd,**kw)

    def test_hdd_handoff_pins_pair_target_and_key_without_v1_path(self):
        from tools.handoff import validate_mapping
        mapping,env=self.provision()
        self.assertEqual(mapping['target'],'retail-v2.12')
        self.assertEqual(mapping['hdd0_root'],str(self.hdd.resolve()))
        self.assertEqual(mapping['guest_path'],'/dev_hdd0/game/BCUS98110/USRDIR/bin/mage_g.self')
        self.assertEqual(mapping['self'],str(self.source.resolve()))
        self.assertEqual(mapping['launcher_elf_sha256'],self.images['EBOOT']['elf_sha256'])
        self.assertEqual(env['MAG_ENGINE_KEY_SHA256'],mapping['key_sha256'])
        self.assertEqual(validate_mapping(mapping),self.receipt)
        self.launcher.write_bytes(b'changed launcher')
        with self.assertRaises(ValueError):validate_mapping(mapping)

    def test_explicit_hdd_root_and_pinned_complete_pair_required(self):
        from tools.handoff import provision_handoff
        with self.assertRaisesRegex(ValueError,'HDD'):
            provision_handoff(self.vfs,self.root,self.binary,target='retail-v2.12')
        self.profile['images'].pop('EBOOT')
        (self.root/'profiles/retail-v2.12.json').write_text(json.dumps(self.profile))
        with self.assertRaises(ValueError):self.provision()

    def test_v212_candidate_transition_revalidates_exact_source_manifest(self):
        from tools.handoff import validate_mapping
        from tools.prepare_inputs import sha256_file
        manifest=self.root/'candidate.json';manifest.write_text('synthetic candidate')
        candidate=dict(base_runtime_revision='strict',manifest_path=str(manifest),
                       manifest_sha256=sha256_file(manifest))
        self.receipt.update(sdk_revision='candidate',sdk_path=str(self.root/'sdk'),runtime_candidate=candidate)
        self.save_receipt()
        actual=dict(sdk_revision='candidate',runtime_candidate=candidate)
        with patch('tools.provenance.runtime_identity',return_value=actual) as check:
            mapping,_=self.provision()
            self.assertEqual(validate_mapping(mapping),self.receipt)
            self.assertEqual(check.call_count,2)
            manifest.write_text('modified')
            with self.assertRaisesRegex(ValueError,'identity'):
                validate_mapping(mapping)
        manifest.write_text('synthetic candidate')
        with patch('tools.provenance.runtime_identity',return_value={**actual,'sdk_revision':'other'}):
            with self.assertRaisesRegex(ValueError,'changed'):
                validate_mapping(mapping)
        candidate['base_runtime_revision']='unreviewed';self.save_receipt()
        with self.assertRaisesRegex(ValueError,'strict'):
            validate_mapping(mapping)

    def test_wrong_target_receipt_or_pair_or_fallback_path_rejected(self):
        from tools.handoff import validate_mapping
        for changes in ({'target':'retail-v1.0'},{'experimental':False},{'not_runtime_verified':False}):
            with self.subTest(changes=changes):
                saved=self.receipt.copy();self.receipt.update(changes);self.save_receipt()
                with self.assertRaises(ValueError):self.provision()
                self.receipt=saved;self.save_receipt()
        mapping,_=self.provision()
        for changes in ({'launcher_elf_sha256':'b'*64},{'elf_sha256':'b'*64},
                        {'self_sha256':'b'*64},{'launcher_source_sha256':'b'*64},
                        {'target':'retail-v1.0'},
                        {'guest_path':'/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self'}):
            with self.subTest(changes=changes),self.assertRaises(ValueError):validate_mapping({**mapping,**changes})

    def test_legacy_three_argument_provision_keeps_original_mapping_shape(self):
        from tools.handoff import provision_handoff
        source=self.vfs/'PS3_GAME/USRDIR/bin/mage_g.self'
        source.parent.mkdir(parents=True);source.write_bytes(b'legacy SELF fixture')
        self.engine.write_bytes(b'legacy ELF fixture');self.launcher.write_bytes(b'legacy key bytes')
        import hashlib
        images=json.loads(json.dumps(self.images))
        images['EBOOT'].update(source_sha256='c'*64,elf_sha256=hashlib.sha256(self.launcher.read_bytes()).hexdigest())
        images['mage_g'].update(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),elf_sha256=hashlib.sha256(self.engine.read_bytes()).hexdigest())
        self.receipt['elf_sha256']=images['mage_g']['elf_sha256']
        (self.root/'profiles/retail-v1.0.json').write_text(json.dumps({'images':images}))
        for key in ('target','experimental','not_runtime_verified'):self.receipt.pop(key)
        self.save_receipt()
        with patch('tools.handoff.validate_ppu_elf',return_value={'load_segments':[dict(vaddr=0x32a660,offset=0,filesz=16)]}):
            mapping,env=provision_handoff(self.vfs,self.root,self.binary)
        self.assertEqual(set(mapping),{'version','token','request','vfs','guest_path','self','elf','binary',
            'self_sha256','elf_sha256','binary_sha256','key_sha256','strict_revisions','command'})
        self.assertEqual(mapping['guest_path'],'/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self')
        self.assertNotIn('PS3_HDD0_ROOT',env)

    def test_launcher_change_and_key_identity_mismatch_reject(self):
        from tools.handoff import validate_mapping
        mapping,_=self.provision()
        self.launcher.write_bytes(b'changed launcher')
        with self.assertRaises(ValueError):validate_mapping(mapping)
        self.launcher.write_bytes(bytes(range(16)))
        with patch('tools.v212_launcher_profile.load_profile',return_value={'handoff':{'key_source':{'address':0x2c0828,'sha256':'0'*64}}}):
            with self.assertRaisesRegex(ValueError,'key identity'):self.provision()

    def test_v212_engine_transition_preserves_roots_and_strips_launcher_identity(self):
        from tools.handoff import capture_handoff
        mapping,_=self.provision()
        req=dict(version=1,token=mapping['token'],guest_path=mapping['guest_path'],kind='mage_g')
        Path(mapping['request']).write_text(json.dumps(req))
        env=dict(os.environ,PS3_HDD0_ROOT=str(self.hdd),PS3_VFS_ROOT=str(self.vfs),
                 MAG_LOCAL_NO_TICKET='1',MAG_LOCAL_ACCOUNT_NAME='FixtureUser',MAG_TRACE_NETWORK='1',PS3_ARGV='fixture')
        calls=[]
        def capture(command,cwd,child_env,log_path,seconds,limit):
            calls.append(child_env);log_path.write_text('fixture')
            return dict(returncode=90 if len(calls)==1 else 0,stop_reason=None)
        with patch('tools.boot.capture_process',side_effect=capture):
            result=capture_handoff(['fixture'],self.root,env,self.root/'console.log',2,mapping)
        self.assertTrue(result['engine_process_started'])
        self.assertEqual(calls[1]['PS3_BOOT_PATH'],mapping['guest_path'])
        self.assertEqual(calls[1]['PS3_HDD0_ROOT'],str(self.hdd.resolve()))
        self.assertEqual(calls[1]['PS3_VFS_ROOT'],str(self.vfs.resolve()))
        for key in ('MAG_LOCAL_NO_TICKET','MAG_LOCAL_ACCOUNT_NAME','MAG_TRACE_NETWORK','PS3_ARGV'):
            self.assertNotIn(key,calls[1])
