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
 def test_promoted_call_site_traces_after_the_register_stores(self):
  # A register-promoted lift stores its live registers between the depth increment and the call.
  source=("void mag_wws_policy_spu_func_00003190(spu_context* ctx) {\n ctx->host_depth++; ctx->gpr[0] = SPU_PR_U(rg0); "
          "spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260); ctx->host_depth--; rg78 = SPU_PR_V(ctx->gpr[78]);\n}")
  pin=hashlib.sha256(source.encode()).hexdigest();new=instrument_source(source,pin)
  self.assertLess(new.index('ctx->gpr[0] = SPU_PR_U(rg0);'),new.index('mag_graphics_payload_begin'))
  self.assertLess(new.index('mag_graphics_payload_begin'),new.index('spu_indirect_branch'))
  self.assertGreater(new.index('mag_graphics_payload_end'),new.index('spu_drain_call'))
  self.assertEqual(new,instrument_source(new,pin))
 def test_call_without_the_depth_increment_is_refused(self):
  source="void mag_wws_policy_spu_func_00003190(spu_context* ctx) {\n spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260);\n}"
  with self.assertRaises(ValueError):instrument_source(source,hashlib.sha256(source.encode()).hexdigest())
