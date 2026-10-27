import unittest,sys,struct,tempfile,pathlib,subprocess
from verified_spu_jobs import executable_sections,emit_registry,sdk_identity,fresh_output,copy_baseline
def synthetic_elf():
 b=bytearray(208);b[:7]=b'\x7fELF\x01\x02\x01'
 struct.pack_into('>H',b,18,23);struct.pack_into('>I',b,32,128)
 struct.pack_into('>HHH',b,46,40,2,0)
 b[96:104]=bytes.fromhex('4080000335000000')
 struct.pack_into('>IIIIIIIIII',b,168,0,1,6,0x4000,96,8,0,0,16,0)
 return bytes(b)
class Tests(unittest.TestCase):
 def setUp(self): self.b=synthetic_elf()
 def check_bad(self,b):
  with self.assertRaises(ValueError): executable_sections(b)
 def test_valid(self): self.assertEqual(executable_sections(self.b),[(0x4000,bytes.fromhex('4080000335000000'))])
 def test_short(self):self.check_bad(self.b[:30])
 def test_machine(self):
  b=bytearray(self.b);struct.pack_into('>H',b,18,21);self.check_bad(b)
 def test_endian(self):
  b=bytearray(self.b);b[5]=1;self.check_bad(b)
 def test_table_bounds(self):
  b=bytearray(self.b);struct.pack_into('>I',b,32,len(b));self.check_bad(b)
 def test_writable(self):
  b=bytearray(self.b);o=struct.unpack_from('>I',b,32)[0];struct.pack_into('>I',b,o+40+8,7);self.check_bad(b)
 def test_ls_bounds(self):
  b=bytearray(self.b);o=struct.unpack_from('>I',b,32)[0];struct.pack_into('>I',b,o+40+12,0x40000);self.check_bad(b)
 def test_file_bounds(self):
  b=bytearray(self.b);o=struct.unpack_from('>I',b,32)[0];struct.pack_into('>I',b,o+40+16,len(b));self.check_bad(b)
 def test_registry(self):
  c=emit_registry([(104,'test_',[(0x4000,b'\0'*16)])]);self.assertIn('spu_verified_overlay_register(101,104',c);self.assertIn('abort()',c);self.assertNotIn('spu_workload_register',c);self.assertNotIn('spu_overlay_register_region',c)
class SDKPins(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.p=pathlib.Path(self.tmp.name)
  for name in ['tools/spu_lifter.py','runtime/spu/spu_verified_overlay.h']:
   f=self.p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('synthetic')
  def git(*args):return subprocess.check_output(['git','-C',str(self.p),*args],stderr=subprocess.DEVNULL,text=True).strip()
  self.git=git;git('init');git('add','.');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-m','synthetic');self.rev=git('rev-parse','HEAD')
 def tearDown(self):self.tmp.cleanup()
 def test_clean(self):self.assertEqual(sdk_identity(self.p,self.rev)['revision'],self.rev)
 def test_wrong_revision(self):
  with self.assertRaises(ValueError):sdk_identity(self.p,'0'*40)
 def test_tracked_dirty(self):
  (self.p/'tools/spu_lifter.py').write_text('changed')
  with self.assertRaises(ValueError):sdk_identity(self.p,self.rev)
class OutputPrivacy(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.tmp.name);self.repo=self.root/'repo';self.repo.mkdir()
  subprocess.run(['git','-C',str(self.repo),'init'],check=True,capture_output=True)
  (self.repo/'.gitignore').write_text('private/\n')
 def tearDown(self):self.tmp.cleanup()
 def test_outside_git(self):
  p=fresh_output(self.root/'outside');self.assertEqual(p.stat().st_mode&0o777,0o700)
 def test_ignored_git(self):
  p=fresh_output(self.repo/'private'/'capture');self.assertEqual(p.stat().st_mode&0o777,0o700)
 def test_unignored_git(self):
  p=self.repo/'source-output'
  with self.assertRaises(ValueError):fresh_output(p)
  self.assertFalse(p.exists())
 def test_git_metadata(self):
  p=self.repo/'.git'/'capture'
  with self.assertRaises(ValueError):fresh_output(p)
  self.assertFalse(p.exists())
 def test_symlink_into_git_metadata(self):
  alias=self.root/'alias';alias.symlink_to(self.repo/'.git',target_is_directory=True)
  with self.assertRaises(ValueError):fresh_output(alias/'capture')
  self.assertFalse((self.repo/'.git/capture').exists())
 def test_bare_git_metadata(self):
  bare=self.root/'bare';subprocess.run(['git','init','--bare',str(bare)],check=True,capture_output=True)
  with self.assertRaises(ValueError):fresh_output(bare/'capture')
  self.assertFalse((bare/'capture').exists())
 def test_existing(self):
  with self.assertRaises(ValueError):fresh_output(self.repo)
 def test_dangling_symlink(self):
  p=self.root/'link';p.symlink_to(self.root/'missing')
  with self.assertRaises(ValueError):fresh_output(p)
 def test_root_mode_survives_copy(self):
  src=self.root/'baseline';src.mkdir(mode=0o755);(src/'source.c').write_text('synthetic');(src/'nested').mkdir();(src/'nested'/'header.h').write_text('synthetic')
  out=fresh_output(self.root/'out');copy_baseline(src,out)
  self.assertEqual(out.stat().st_mode&0o777,0o700);self.assertEqual((out/'source.c').read_text(),'synthetic');self.assertTrue((out/'nested/header.h').exists())
if __name__=='__main__':unittest.main()
