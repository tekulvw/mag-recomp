import unittest
from tools.movie_picture_hooks import instrument_source

class MoviePictureHooksTests(unittest.TestCase):
    def test_capture_is_before_fifo_pointer_exposure_and_scoped_to_binder(self):
        body="void func_009841FC(ppu_context* ctx) {\n        vm_write32(ctx->gpr[11] + 0x4, ctx->gpr[10]);\n        vm_write32(ctx->gpr[30] + 0x8, ctx->gpr[6]);\n        return;\n}\nvoid func_00990000(ppu_context* ctx) { return; }"
        changed=instrument_source(body)
        self.assertLess(changed.index('mag_movie_picture_command('),changed.index('vm_write32(ctx->gpr[30]'))
        self.assertEqual(changed.count('mag_movie_picture_command('),1)
        self.assertEqual(instrument_source(changed),changed)

    def test_unknown_generated_shape_fails_before_modification(self):
        with self.assertRaises(ValueError):
            instrument_source('void func_009841FC(ppu_context* ctx) { return; }')

    def test_original_descriptor_scope_preserves_all_returns_and_fallback(self):
        body='void func_00E14D48(ppu_context* ctx) {\n        if(ctx->gpr[3])return;\n        return;\n}\n'
        changed=instrument_source(body)
        self.assertIn('MagMoviePictureScope mag_picture_scope',changed)
        self.assertEqual(changed.count('return;'),2)
        self.assertEqual(instrument_source(changed),changed)

    def test_other_title_functions_are_untouched(self):
        body='void func_00010000(ppu_context* ctx) { return; }'
        self.assertEqual(instrument_source(body),body)
