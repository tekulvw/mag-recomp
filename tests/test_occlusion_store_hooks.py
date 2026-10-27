import hashlib,re,unittest
from tools.occlusion_store_hooks import instrument_source
class Hooks(unittest.TestCase):
 def test_original_store_order_and_idempotence(self):
  s='void func_AAAAAAAA(ppu_context* ctx) {\nloc_00CFDB20:\n synthetic_compare();\n vm_write32(synthetic_address,synthetic_value);\n original_after();\n}'
  pins={'AAAAAAAA':{'sha256':hashlib.sha256(s.encode()).hexdigest(),'labels':['00CFDB20']}}
  t=instrument_source(s,pins);self.assertEqual(t,instrument_source(t,pins))
  self.assertLess(t.index('vm_write32('),t.index('mag_graphics_occlusion_store(ctx,'));self.assertLess(t.index('mag_graphics_occlusion_store(ctx,'),t.index('original_after();'))
  restored=re.sub(r'\n        mag_graphics_occlusion_store\([^\n]*\);','',t).replace('#include "mag_graphics_trace.h"\n','');self.assertEqual(s,restored)
 def test_unknown_shape_fails_closed(self):
  with self.assertRaises(ValueError):instrument_source('void func_AAAAAAAA(ppu_context* ctx) {\n}',{'AAAAAAAA':{'sha256':'0'*64,'labels':['00CFDB20']}})
