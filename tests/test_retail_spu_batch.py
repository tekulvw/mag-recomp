"""Neutral ELF fixtures; no game bytes or generated original code."""
import copy, hashlib, json, os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path
# Works both beside the private candidate and under MAG/tests after review.
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import retail_spu_batch as b

def elf(code=b'\x40\x80\x02\x82\0\0\0\0'):
 out=bytearray(0x100+len(code)); ident=b'\x7fELF\x01\x02\x01'+bytes(9)
 struct.pack_into('>16sHHIIIIIHHHHHH',out,0,ident,2,23,1,0x3000,52,0x80,0,52,32,1,40,2,0)
 struct.pack_into('>IIIIIIII',out,52,1,0x100,0x3000,0x3000,len(code),len(code),5,16)
 struct.pack_into('>IIIIIIIIII',out,0xA8,0,1,6,0x3000,0x100,len(code),0,0,16,0)
 out[0x100:]=code;return bytes(out)
def fixture():
 data=elf();carrier=b'HEAD'+data
 image=dict(scope='retail',carrier='neutral',file_offset='0x4',size=len(data),sha256=b.sha(data),entry='0x3000',executable_end='0x3008',runtime_fingerprint=hex(b.fnv(data)),registration=None)
 inventory=dict(schema_version=1,sources=[dict(name='neutral',scope='retail',sha256=b.sha(carrier),bytes=len(carrier))],images=[image],summary={'retail_occurrences':1,'retail_unique_ELFs':1})
 return data,inventory,{'neutral':carrier}
class BatchTests(unittest.TestCase):
 def test_dedup_all_verified_occurrences(self):
  _,i,c=fixture();i['images']*=2;i['summary']={'retail_occurrences':2,'retail_unique_ELFs':1}
  p=b.plan(i,c);self.assertEqual(len(p),1);self.assertEqual(len(p[0]['occurrences']),2)
 def test_conflicting_duplicate_code_end_rejected(self):
  _,i,c=fixture();i['images'].append(dict(i['images'][0],executable_end='0x3004'))
  with self.assertRaises(ValueError):b.plan(i,c)
 def test_bad_carrier_hash(self):
  _,i,c=fixture();c['neutral']=b'FAIL'+c['neutral'][4:]
  with self.assertRaises(ValueError):b.plan(i,c)
 def test_bad_image_hash(self):
  _,i,c=fixture();i['images'][0]['sha256']='0'*64
  with self.assertRaises(ValueError):b.plan(i,c)
 def test_bad_fingerprint(self):
  _,i,c=fixture();i['images'][0]['runtime_fingerprint']='0x1'
  with self.assertRaises(ValueError):b.plan(i,c)
 def test_missing_carrier(self):
  _,i,c=fixture()
  with self.assertRaises(ValueError):b.plan(i,{})
 def test_negative_or_outside_slice(self):
  for field,value in [('file_offset','-1'),('size',99999),('size',0)]:
   _,i,c=fixture();i['images'][0][field]=value
   with self.assertRaises(ValueError):b.plan(i,c)
 def test_entry_and_end_pins(self):
  for field,value in [('entry','0x3004'),('executable_end','0x40000')]:
   _,i,c=fixture();i['images'][0][field]=value
   with self.assertRaises(ValueError):b.plan(i,c)
 def test_machine_header_and_table_bounds(self):
  for off,data in [(18,b'\0\x14'),(5,b'\1'),(28,b'\xff\xff\xff\xff'),(42,b'\0\x01'),(46,b'\0\x01')]:
   raw,i,c=fixture();raw=bytearray(raw);raw[off:off+len(data)]=data;raw=bytes(raw);c['neutral']=b'HEAD'+raw;i['sources'][0]['sha256']=b.sha(c['neutral']);i['images'][0]['sha256']=b.sha(raw);i['images'][0]['runtime_fingerprint']=hex(b.fnv(raw))
   with self.assertRaises(ValueError):b.plan(i,c)
 def test_LS_and_backing_bounds(self):
  for off,value in [(60,0x3FFFC),(68,99999),(72,0x40001)]:
   raw,i,c=fixture();raw=bytearray(raw);struct.pack_into('>I',raw,off,value);raw=bytes(raw);c['neutral']=b'HEAD'+raw;i['sources'][0]['sha256']=b.sha(c['neutral']);i['images'][0]['sha256']=b.sha(raw);i['images'][0]['runtime_fingerprint']=hex(b.fnv(raw))
   with self.assertRaises(ValueError):b.plan(i,c)
 def test_inventory_counts_and_empty(self):
  _,i,c=fixture();i['summary']['retail_unique_ELFs']=2
  with self.assertRaises(ValueError):b.plan(i,c)
  with self.assertRaises(ValueError):b.plan({'sources':[],'images':[]},{})
 def test_classification_not_link_or_enable(self):
  self.assertEqual(b.classify(0,0,2,True),'unsupported');self.assertEqual(b.classify(0,0,0,True),'compiled')
  for args in [(1,None,0,False),(0,1,0,True),(0,0,0,False)]:self.assertEqual(b.classify(*args),'failed')
 def test_failure_and_timeout(self):
  r=b.run_command([sys.executable,'-c','raise SystemExit(7)'],2);self.assertEqual(r['exit'],7)
  r=b.run_command([sys.executable,'-c','import time;time.sleep(2)'],0.01);self.assertTrue(r['timeout']);self.assertNotEqual(r['exit'],0)
 def test_fresh_output_and_symlink(self):
  with tempfile.TemporaryDirectory()as t:
   p=Path(t);f=p/'input';f.write_bytes(b'abc');l=p/'link';l.symlink_to(f)
   with self.assertRaises(ValueError):b.read_regular(l)
   with self.assertRaises(ValueError):b.fresh_output(p)
 def test_real_sdk_neutral_ELF_end_to_end(self):
  sdk=os.environ.get('SPU_BATCH_TEST_SDK')
  if not sdk:self.skipTest('set SPU_BATCH_TEST_SDK for real lifter/compiler neutral smoke')
  with tempfile.TemporaryDirectory()as t:
   d=Path(t);_,i,c=fixture();ip=d/'inventory.json';ip.write_text(json.dumps(i));cp=d/'carrier';cp.write_bytes(c['neutral']);out=d/'generated'
   rev=subprocess.check_output(['git','-C',sdk,'rev-parse','HEAD'],text=True).strip()
   args=[sys.executable,str(Path(b.__file__)),'--inventory',str(ip),'--inventory-sha256',b.sha(ip.read_bytes()),'--carrier','neutral='+str(cp),'--sdk',sdk,'--sdk-revision',rev,'--output',str(out)]
   q=subprocess.run(args,capture_output=True,text=True,timeout=30);self.assertEqual(q.returncode,0,q.stdout+q.stderr)
   report=json.loads((out/'report.json').read_text());self.assertEqual(report['status'],'complete');self.assertEqual(report['images'][0]['status'],'compiled');self.assertEqual(report['images'][0]['runtime_activation'],'not_enabled');self.assertEqual(report['images'][0]['link_status'],'not_linked');self.assertTrue((out/report['images'][0]['object']).is_file())
   # Existing output must never be reused as successful stale generated code.
   q=subprocess.run(args,capture_output=True,text=True,timeout=30);self.assertNotEqual(q.returncode,0)
 def test_global_identity_failure_has_no_output(self):
  with tempfile.TemporaryDirectory()as t:
   d=Path(t);_,i,c=fixture();ip=d/'i.json';ip.write_text(json.dumps(i));cp=d/'carrier';cp.write_bytes(c['neutral']);out=d/'output'
   q=subprocess.run([sys.executable,str(Path(b.__file__)),'--inventory',str(ip),'--inventory-sha256','0'*64,'--carrier','neutral='+str(cp),'--sdk',str(d/'does-not-exist'),'--sdk-revision','0'*40,'--output',str(out)],capture_output=True,text=True)
   self.assertEqual(q.returncode,2);self.assertFalse(out.exists())
 def test_float_counts_and_duplicate_source_fail(self):
  _,i,c=fixture();i['images'][0]['size']=264.5
  with self.assertRaises(ValueError):b.plan(i,c)
  _,i,c=fixture();i['sources']*=2
  with self.assertRaises(ValueError):b.plan(i,c)
 def test_original_outputs_must_be_outside_git_or_ignored(self):
  with tempfile.TemporaryDirectory()as t:
   d=Path(t);subprocess.run(['git','init','-q',str(d)],check=True)
   with self.assertRaises(ValueError):b.fresh_output(d/'generated')
   (d/'.gitignore').write_text('private-output/\n')
   self.assertTrue(b.fresh_output(d/'private-output').is_dir())
   with self.assertRaises(ValueError):b.fresh_output(d/'.git'/'payload')
 def test_partial_batch_continues_and_stays_unlinked(self):
  # A controlled external generator exercises orchestration failure; the real
  # SDK smoke separately checks the actual generator/compiler integration.
  with tempfile.TemporaryDirectory()as t:
   d=Path(t);sdk=d/'sdk';(sdk/'tools').mkdir(parents=True);(sdk/'runtime/spu').mkdir(parents=True);(sdk/'include').mkdir()
   for n in ['tools/find_spu_functions.py','tools/spu_disasm.py','runtime/spu/spu_helpers.h','runtime/spu/spu_context.h']:(sdk/n).write_text('')
   generator="""import argparse,pathlib
p=argparse.ArgumentParser();p.add_argument('--auto-functions');p.add_argument('--code-end');p.add_argument('--symbol-prefix');p.add_argument('--output');a=p.parse_args();data=pathlib.Path(a.auto_functions).read_bytes()
if data[-1]==7:raise SystemExit(7)
o=pathlib.Path(a.output);o.mkdir();(o/'spu_recomp.h').write_text('');(o/'spu_recomp.c').write_text('void '+a.symbol_prefix+'spu_func_00003000(void* p) {(void)p;}\\n')
"""
   (sdk/'tools/spu_lifter.py').write_text(generator)
   for cmd in [['git','init','-q',str(sdk)],['git','-C',str(sdk),'add','.'],['git','-C',str(sdk),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','neutral tool fixture']]:subprocess.run(cmd,check=True)
   rev=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
   first,i,c=fixture();second=elf(b'\x40\x80\x02\x82\0\0\0\7');blob=c['neutral']+second;i['sources'][0].update(bytes=len(blob),sha256=b.sha(blob));item=dict(i['images'][0],file_offset=hex(len(c['neutral'])),sha256=b.sha(second),runtime_fingerprint=hex(b.fnv(second)));i['images'].append(item);i['summary'].update(retail_occurrences=2,retail_unique_ELFs=2)
   ip=d/'i.json';ip.write_text(json.dumps(i));cp=d/'carrier';cp.write_bytes(blob);out=d/'output'
   q=subprocess.run([sys.executable,str(Path(b.__file__)),'--inventory',str(ip),'--inventory-sha256',b.sha(ip.read_bytes()),'--carrier','neutral='+str(cp),'--sdk',str(sdk),'--sdk-revision',rev,'--output',str(out)],capture_output=True,text=True,timeout=15)
   self.assertEqual(q.returncode,1,q.stdout+q.stderr);report=json.loads((out/'report.json').read_text());self.assertEqual(report['status'],'completed_with_failures');self.assertEqual(len(report['images']),2);self.assertEqual(report['counts'],dict(compiled=1,unsupported=0,failed=1));self.assertTrue(all(x['runtime_activation']=='not_enabled'and x['link_status']=='not_linked'for x in report['images']))
if __name__=='__main__':unittest.main()
