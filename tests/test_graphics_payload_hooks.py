import unittest,hashlib
from tools.graphics_payload_hooks import instrument_source
class PayloadHooks(unittest.TestCase):
 def test_retains_original_call_and_drain(self):
  source="void mag_wws_policy_spu_func_00003190(spu_context* ctx) {\n ctx->host_depth++; spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260); ctx->host_depth--;\n}"
  pin=hashlib.sha256(source.encode()).hexdigest();new=instrument_source(source,pin)
  self.assertIn('spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260);',new)
  self.assertLess(new.index('mag_graphics_payload_begin'),new.index('spu_indirect_branch'))
  self.assertGreater(new.index('mag_graphics_payload_end'),new.index('spu_drain_call'))
  self.assertEqual(new,instrument_source(new,pin))
 def test_mismatch_closed(self):
  with self.assertRaises(ValueError):instrument_source('void mag_wws_policy_spu_func_00003190(spu_context* ctx) {\n return;\n}')
