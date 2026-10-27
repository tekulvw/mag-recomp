"""Focused generator contract; actual instruction/SDK replay is a separate gate."""
import importlib.util
import pathlib
import unittest

PATH = pathlib.Path(__file__).with_name('geometry_spu_continuations.py')


class Contract(unittest.TestCase):
    def test_missing_interior_entries_require_additive_bounds(self):
        spec = importlib.util.spec_from_file_location('continuations', PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = '''void mag_edgegeom_perf_spu_func_00007A40(spu_context* ctx) {
    if (!spu_verified_code_enter_span(ctx, 105, 0x7A40u, 0x7BBCu)) {}
}
void mag_edgegeom_perf_spu_func_00007BBC(spu_context* ctx) {
    if (!spu_verified_code_enter_span(ctx, 105, 0x7BBCu, 0x7ED0u)) {}
}
'''
        bounds = module.continuation_bounds(source)
        self.assertEqual(bounds, [(0x7A44, 0x7BBC), (0x7BE0, 0x7ED0)])
        self.assertNotIn((0x7A40, 0x7A44), bounds)
        with self.assertRaises(ValueError):
            module.continuation_bounds(source.replace('0x7ED0u', '0x7DF0u'))

    def test_registration_is_same_image_and_default_off(self):
        spec = importlib.util.spec_from_file_location('continuations', PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = module.registration_source()
        self.assertIn('MAG_COMPILED_GEOMETRY', source)
        self.assertIn('spu_begin_image(105)', source)
        self.assertEqual(source.count('spu_register_function('), 2)
        self.assertNotIn('spu_verified_overlay_register', source)
        self.assertNotIn('entry_override_register', source)
        self.assertLess(source.index('strcmp(enabled,"1")'), source.index('spu_begin_image(105)'))


if __name__ == '__main__':
    unittest.main()
