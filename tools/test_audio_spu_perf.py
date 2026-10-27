import os,pathlib,subprocess,sys,tempfile,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import audio_spu_perf as m
from geometry_spu_perf import registry_source as geometry_registry
class Contract(unittest.TestCase):
 def body(self,start='4000',end='4008',guard=True):
  gate=f'if (!spu_verified_code_enter_span(ctx, 104, 0x{start}u, 0x{end}u)) return;' if guard else ''
  return 'void mag_audio_span_perf_spu_func_00004000(spu_context* ctx) {\n'+gate+'\n}\n'
 def test_valid_span(self):self.assertEqual(m.guard_census(self.body(),[(0x4000,b'\0'*8)]),1)
 def test_missing_guard(self):
  with self.assertRaises(ValueError):m.guard_census(self.body(guard=False),[(0x4000,b'\0'*8)])
 def test_wrong_entry(self):
  with self.assertRaises(ValueError):m.guard_census(self.body(start='4004'),[(0x4000,b'\0'*8)])
 def test_outside_map(self):
  with self.assertRaises(ValueError):m.guard_census(self.body(end='400c'),[(0x4000,b'\0'*8)])
 def test_spans_cannot_cross_hole(self):
  with self.assertRaises(ValueError):m.guard_census(self.body(end='4014'),[(0x4000,b'\0'*8),(0x4010,b'\0'*4)])
 def test_no_functions(self):
  with self.assertRaises(ValueError):m.guard_census('',[(0x4000,b'\0'*8)])
 def test_duplicate_functions(self):
  with self.assertRaises(ValueError):m.guard_census(self.body()+self.body(),[(0x4000,b'\0'*8)])
 def test_exclude_uncovered_bounds(self):
  kept,excluded=m.covered_functions([(0x4000,0x4014),(0x4080,0x4088),(0x4088,0x4100)],[(0x4000,b'\0'*16),(0x4080,b'\0'*16)],0x4090)
  self.assertEqual(kept,[(0x4080,0x4088),(0x4088,0x4090)]);self.assertEqual(excluded,[(0x4000,0x4014)])
 def test_unexpected_source_rejected(self):
  with tempfile.TemporaryDirectory() as td:
   root=pathlib.Path(td);(root/'known.c').write_text('fixture')
   manifest={'known.c':m.digest(root/'known.c')};m.verify_sources(root,manifest)
   (root/'extra').mkdir();(root/'extra/spu_recomp.c').write_text('unexpected')
   with self.assertRaises(ValueError):m.verify_sources(root,manifest)
 def test_independent_constructor_switches(self):
  with tempfile.TemporaryDirectory() as td:
   p=pathlib.Path(td)
   (p/'spu_verified_overlay.h').write_text('#include <stdint.h>\ntypedef struct {uint32_t a,n; const uint8_t *p;} spu_verified_code_segment;\nint spu_verified_overlay_register(int,int,const spu_verified_code_segment*,int);\n')
   c=geometry_registry([(0x4000,b'\0'*8)])+'\n'+m.registry_source([(0x4000,b'\0'*8)])
   c+='''\nstatic int selected, mask;\nvoid spu_begin_image(int n){selected=n;}\nint spu_verified_overlay_register(int p,int n,const spu_verified_code_segment*s,int count){return p==101 && count==1 && s->a==0x4000 && (n==104||n==105);}\nvoid mag_edgegeom_perf_spu_recomp_register(void){if(selected!=105)abort();mask|=1;}\nvoid mag_audio_span_perf_spu_recomp_register(void){if(selected!=104)abort();mask|=2;}\nint main(void){printf("%d\\n",mask);return selected!=0;}\n'''
   # Repeated includes are normal in concatenated registry; use a guarded fixture header.
   h=p/'spu_verified_overlay.h';h.write_text('#ifndef TEST_HEADER\n#define TEST_HEADER\n'+h.read_text()+'#endif\n')
   (p/'registry.c').write_text(c);subprocess.run(['cc','-std=c11','-Werror',str(p/'registry.c'),'-I',str(p),'-o',str(p/'registry')],check=True,capture_output=True)
   for geo,audio,expected in [('0','0','0'),('1','0','1'),('0','1','2'),('1','1','3'),('yes','yes','0')]:
    env=dict(os.environ,MAG_COMPILED_GEOMETRY=geo,MAG_COMPILED_AUDIO=audio)
    self.assertEqual(subprocess.check_output([str(p/'registry')],env=env,text=True).strip(),expected)
if __name__=='__main__':unittest.main()
