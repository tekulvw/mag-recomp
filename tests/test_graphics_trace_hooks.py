import unittest
from tools.graphics_trace_hooks import instrument_source, SHAPES
class Hooks(unittest.TestCase):
 def test_synthetic_direct_early_return_and_idempotence(self):
  s="void func_0023D7D0(ppu_context* ctx) {\n if(ctx->gpr[3]) return;\n func_00A047D8(ctx);\n return;\n}"
  import hashlib
  pins={"0023D7D0":hashlib.sha256(s.encode()).hexdigest()}
  t=instrument_source(s,pins);self.assertIn("MagGraphicsTraceScope",t);self.assertIn("func_00A047D8(ctx)",t);self.assertEqual(t,instrument_source(t,pins))
 def test_mismatch_closed(self):
  with self.assertRaises(ValueError):instrument_source("void func_0023D7D0(ppu_context* ctx) {\n return;\n}")
