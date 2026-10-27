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
