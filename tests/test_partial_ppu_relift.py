"""Synthetic inputs only; Git tool objects are supplied by --sdk."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('partial_relift', ROOT/'tools/partial_ppu_relift.py')
u=importlib.util.module_from_spec(spec);spec.loader.exec_module(u)
SDK=Path(sys.argv.pop(sys.argv.index('--sdk')+1)) if '--sdk' in sys.argv else None
if '--sdk' in sys.argv:sys.argv.remove('--sdk')

class PartialRelift(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SDK is None:raise RuntimeError('--sdk with pinned Git objects required')
        cls.real=json.loads((ROOT/'profiles/retail-v1.0-ppu-ctr64-partial-relift.json').read_text())
        cls.old,cls.new,decode=u.load_tools(SDK,cls.real)
        cls.decode=staticmethod(decode)

    def setUp(self):
        # Synthetic MTSPR CTR, MFSPR CTR and BLR; no original title bytes.
        mtspr=(31<<26)|(10<<21)|(9<<16)|(467<<1)
        mfspr=(31<<26)|(9<<21)|(9<<16)|(339<<1)
        blr=(19<<26)|(20<<21)|(16<<1)
        code=struct.pack('>III',mtspr,mfspr,blr)
        elf=bytearray(0x100+len(code));elf[:7]=b'\x7fELF\x02\x02\x01'
        struct.pack_into('>Q',elf,32,64);struct.pack_into('>HH',elf,54,56,1)
        struct.pack_into('>IIQQQQQQ',elf,64,1,5,0x100,0x1000,0x1000,len(code),len(code),16)
        elf[0x100:]=code;self.elf=bytes(elf)
        ins=self.decode(code,0x1000)
        before='\n'.join(self.old.PPULifter().lift_function(ins,0x1000,0x100c).body_lines)
        after='\n'.join(self.new.PPULifter().lift_function(ins,0x1000,0x100c).body_lines)
        self.body='void func_00001000(ppu_context* ctx) {\n'+before+'\n}\n'
        self.newbody=self.body.replace('ctx->ctr = (uint32_t)ctx->gpr[10];','ctx->ctr = ctx->gpr[10];')
        self.source=('// untouched prefix\n'+self.body+'\nvoid sentinel(ppu_context* ctx) {}\n').encode()
        self.output=self.source.replace(self.body.encode(),self.newbody.encode())
        self.p=copy.deepcopy(self.real)
        self.p.update(elf_sha256=u.sha(self.elf),input_sha256=u.sha(self.source),output_sha256=u.sha(self.output))
        self.p['function']=dict(name='func_00001000',start='0x1000',end='0x100c',instruction_count=3,bytes_sha256=u.sha(code),before_body_sha256=u.sha(self.body.encode()),after_body_sha256=u.sha(self.newbody.encode()),mtctr_register=10,mtctr=[['0x1000','r10']],mfctr=[['0x1004','r9']])

    def call(self,**kw):
        args=dict(source=self.source,elf=self.elf,profile=self.p,old=self.old,new=self.new,decode=self.decode);args.update(kw)
        return u.propose(**args)

    def test_complete_lift_and_other_bytes(self):
        out,r=self.call();self.assertEqual(out,self.output);self.assertFalse(r['already_applied'])
    def test_verify_existing_without_prepare(self):
        out,r=self.call(source=self.output,verify_existing=True);self.assertEqual(out,self.output);self.assertTrue(r['already_applied'])
        with self.assertRaises(ValueError):self.call(source=self.output)
    def test_source_changes_rejected(self):
        with self.assertRaises(ValueError):self.call(source=self.source+b' ')
    def test_elf_changes_rejected(self):
        with self.assertRaises(ValueError):self.call(elf=self.elf[:-1]+b'X')
    def test_body_proof_not_just_file_hash(self):
        bad=self.source.replace(b'// 0x',b'// other',1)
        # Change meaningful lift text even if profile whole-file hash follows it.
        bad=bad.replace(b'ctx->gpr[9] = ctx->ctr;',b'ctx->gpr[9] = 7;')
        p=copy.deepcopy(self.p);p['input_sha256']=u.sha(bad)
        a=bad.decode().index('void func_');b=bad.decode().index('\nvoid sentinel')
        p['function']['before_body_sha256']=u.sha(bad[a:b])
        with self.assertRaises(ValueError):self.call(source=bad,profile=p)
    def test_more_than_mtctr_rejected(self):
        class Changed:
            class PPULifter:
                def lift_function(s,*args):
                    f=self.new.PPULifter().lift_function(*args);f.body_lines.append('ctx->lr=9;');return f
        with self.assertRaises(ValueError):self.call(new=Changed)
    def test_wrong_revision_or_blob(self):
        for field,value in [('revision','master'),('lifter_sha256','0'*64),('revision','f'*40)]:
            p=copy.deepcopy(self.p);p['tools']['before'][field]=value
            with self.assertRaises((ValueError,subprocess.CalledProcessError)):u.load_tools(SDK,p)
    def test_layout_bounds_rejected(self):
        for offset,value,fmt in [(54,1,'>H'),(32,0xffff,'>Q'),(96,1000000,'>Q')]:
            elf=bytearray(self.elf);struct.pack_into(fmt,elf,offset,value)
            with self.assertRaises(ValueError):u.function_bytes(bytes(elf),0x1000,0x100c)
    def test_instruction_identity_rejected(self):
        for field,value in [('mtctr',[['0x1004','r10']]),('instruction_count',4),('bytes_sha256','0'*64)]:
            p=copy.deepcopy(self.p);p['function'][field]=value
            with self.assertRaises(ValueError):self.call(profile=p)
    def test_fresh_output_and_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'out.cpp';receipt=u.write_fresh(out,self.output,{'verified':True})
            self.assertEqual(out.read_bytes(),self.output);self.assertEqual(json.loads(receipt.read_text()),{'verified':True})
            with self.assertRaises(ValueError):u.write_fresh(out,b'poison',{})
            self.assertEqual(out.read_bytes(),self.output)
    def test_reused_receipt_and_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'out.cpp';out.with_suffix('.cpp.receipt.json').write_text('{}')
            with self.assertRaises(ValueError):u.write_fresh(out,b'x',{})
            self.assertFalse(out.exists());out.with_suffix('.cpp.receipt.json').unlink()
            out.symlink_to(Path(d)/'nonexistent')
            with self.assertRaises(ValueError):u.write_fresh(out,b'x',{})
    def test_decoder_pin_rejected(self):
        p=copy.deepcopy(self.p);p['tools']['decoder_sha256']='0'*64
        with self.assertRaises(ValueError):u.load_tools(SDK,p)

if __name__=='__main__':unittest.main()
