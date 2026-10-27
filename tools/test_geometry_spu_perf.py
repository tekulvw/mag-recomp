import importlib.util,pathlib,struct,sys,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
spec=importlib.util.spec_from_file_location('geometry_spu_perf',str(pathlib.Path(__file__).with_name('geometry_spu_perf.py')));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Contract(unittest.TestCase):
 def test_explicit_runtime_toggle(self):
  s=m.registry_source([(0x4000,b'\0'*8)])
  self.assertIn('MAG_COMPILED_GEOMETRY',s);self.assertIn('strcmp(enabled,"1")',s)
  self.assertIn('spu_verified_overlay_register(101,105',s)
  self.assertLess(s.index('strcmp(enabled,"1")'),s.index('spu_begin_image(105)'))
 def test_guard_census(self):
  self.assertEqual(m.guard_census('void mag_edgegeom_perf_spu_func_00004000(spu_context* c) {\n if (!spu_verified_code_enter_span(ctx, 105, 0x4000u, 0x4008u)) {}\n}',True),1)
 def test_mismatched_census(self):
  with self.assertRaises(ValueError):m.guard_census('void mag_edgegeom_perf_spu_func_00004000(spu_context* c) {}',True)
 def test_invalid_body_range(self):
  with self.assertRaises(ValueError):m.guard_census('void mag_edgegeom_perf_spu_func_00004000(spu_context* c) {\n if (!spu_verified_code_enter_span(ctx, 105, 0x4000u, 0x4000u)) {}\n}',True)
if __name__=='__main__':unittest.main()
