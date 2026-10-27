import sys
import subprocess
import tempfile
import unittest
import json
from unittest.mock import patch
from pathlib import Path
from tools.lift import run_generator, guard_generated_source, generate_lift, load_hooks, apply_hooks, hook_records
from tools.prepare_inputs import sha256_file
from tools.inventory import SDK_REVISION


class LiftTests(unittest.TestCase):
    def test_substituted_inventory_map_stops_before_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'profiles').mkdir()
            elf=root/'EBOOT.elf'; elf.write_bytes(b'test image')
            expected=sha256_file(elf)
            (root/'profiles/retail-v1.0.json').write_text(json.dumps(dict(images=dict(EBOOT=dict(elf_sha256=expected)))))
            functions=root/'reviewed-functions.json'; imports=root/'EBOOT.imports.json'
            functions.write_text('functions'); imports.write_text('imports')
            (root/'summary.json').write_text(json.dumps(dict(elf_sha256=expected,
                sdk_revision=SDK_REVISION,code_end='0x100',
                manifests=dict(functions_sha256=sha256_file(functions),imports_sha256=sha256_file(imports)))))
            for selected in [functions,imports]:
                original=selected.read_text(); selected.write_text('foreign image map')
                with self.subTest(manifest=selected.name), patch('tools.lift.ROOT',root), patch('tools.prepare_inputs.ROOT',root), patch('tools.prepare_inputs.WORKSPACE',root), \
                     patch('tools.lift.check_sdk'), patch('tools.lift.run_generator',
                         side_effect=AssertionError('Generation started before inventory identity verification')) as generator:
                    with self.assertRaises(ValueError):
                        generate_lift(elf,functions,imports,root/'sdk',root/'out')
                    generator.assert_not_called()
                selected.write_text(original)

    def test_unknown_instruction_stops_instead_of_falling_through(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'guard.cpp'; binary=Path(tmp)/'guard'
            source.write_text(guard_generated_source('int main() { /* TODO: .word 0x00000000 */; return 0; }'))
            subprocess.run(['/usr/bin/clang++', str(source), '-o', str(binary)], check=True, capture_output=True)
            result=subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 86)
            self.assertIn('[mag] unsupported lifted code', result.stderr)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)/'out'

    def test_failed_generator_is_not_success(self):
        with self.assertRaises(RuntimeError):
            run_generator([sys.executable, '-c', 'raise SystemExit(7)'], self.out)

    def test_zero_exit_without_generated_files_is_not_success(self):
        with self.assertRaises(RuntimeError):
            run_generator([sys.executable, '-c', 'pass'], self.out)

    def hook_modules(self, **bodies):
        """Write synthetic hook modules on sys.path; return their names."""
        directory=Path(self.tmp.name)/'hooks';directory.mkdir(exist_ok=True)
        sys.path.insert(0,str(directory));self.addCleanup(sys.path.remove,str(directory))
        for name,body in bodies.items():
            (directory/f'{name}.py').write_text(f'def instrument_source(source):\n    {body}\n')
            sys.modules.pop(name,None)
        return list(bodies)

    def test_hooks_run_after_builtins_in_order(self):
        names=self.hook_modules(mag_hook_a="return source+'|A'",mag_hook_b="return source+'|B'")
        self.assertEqual(apply_hooks('builtin',load_hooks(names)),'builtin|A|B')

    def test_hooks_recorded_in_receipt(self):
        names=self.hook_modules(mag_hook_c="return source")
        [(name,module)]=load_hooks(names)
        self.assertEqual(hook_records([(name,module)]),
                         [{'module':'mag_hook_c','path':module.__file__,'sha256':sha256_file(Path(module.__file__))}])

    def test_hook_must_return_text(self):
        names=self.hook_modules(mag_hook_d="return None")
        with self.assertRaisesRegex(ValueError,'mag_hook_d'):
            apply_hooks('x',load_hooks(names))

    def test_failing_hook_aborts_lift(self):
        names=self.hook_modules(mag_hook_e="raise RuntimeError('boom')")
        root=Path(self.tmp.name)/'repo';(root/'profiles').mkdir(parents=True)
        elf=root/'EBOOT.elf';elf.write_bytes(b'test image');digest=sha256_file(elf)
        (root/'profiles/retail-v1.0.json').write_text(json.dumps(dict(images=dict(EBOOT=dict(elf_sha256=digest)))))
        functions=root/'reviewed-functions.json';imports=root/'EBOOT.imports.json'
        functions.write_text('functions');imports.write_text('imports')
        (root/'summary.json').write_text(json.dumps(dict(elf_sha256=digest,sdk_revision=SDK_REVISION,code_end='0x100',
            manifests=dict(functions_sha256=sha256_file(functions),imports_sha256=sha256_file(imports)))))
        out=root/'out'
        def generate(command,output_dir):
            output_dir.mkdir();(output_dir/'ppu_recomp_000.cpp').write_text('void f() {}\n');return dict(command=command,exit_code=0)
        with patch('tools.lift.ROOT',root),patch('tools.prepare_inputs.ROOT',root),patch('tools.prepare_inputs.WORKSPACE',root), \
             patch('tools.lift.check_sdk'),patch('tools.lift.run_generator',side_effect=generate), \
             patch('tools.lift.RETAIL_ELF',digest),patch('tools.lift.instrument_source',side_effect=lambda s:s), \
             patch('tools.lift.instrument_graphics',side_effect=lambda s:s):
            with self.assertRaisesRegex(RuntimeError,'boom'):
                generate_lift(elf,functions,imports,root/'sdk',out,hooks=names)
        self.assertFalse((out/'provenance.json').exists())

    def fake_lift(self, names, retail):
        """Run generate_lift end to end with the generator, built-in hooks and audit faked; return the receipt."""
        root=Path(self.tmp.name)/('retail' if retail else 'other');(root/'profiles').mkdir(parents=True)
        (root/'tools').mkdir();(root/'tools/movie_picture_hooks.py').write_text('# receipt fixture\n')
        elf=root/'EBOOT.elf';elf.write_bytes(b'test image '+root.name.encode());digest=sha256_file(elf)
        (root/'profiles/retail-v1.0.json').write_text(json.dumps(dict(images=dict(EBOOT=dict(elf_sha256=digest)))))
        functions=root/'reviewed-functions.json';imports=root/'EBOOT.imports.json'
        functions.write_text('functions');imports.write_text('imports')
        (root/'summary.json').write_text(json.dumps(dict(elf_sha256=digest,sdk_revision=SDK_REVISION,code_end='0x100',
            manifests=dict(functions_sha256=sha256_file(functions),imports_sha256=sha256_file(imports)))))
        out=root/'out'
        def generate(command,output_dir):
            output_dir.mkdir();(output_dir/'ppu_recomp_000.cpp').write_text('void f() {}\n');return dict(command=command,exit_code=0)
        with patch('tools.lift.ROOT',root),patch('tools.prepare_inputs.ROOT',root),patch('tools.prepare_inputs.WORKSPACE',root), \
             patch('tools.lift.check_sdk'),patch('tools.lift.run_generator',side_effect=generate), \
             patch('tools.lift.RETAIL_ELF',digest if retail else '0'*64),patch('tools.lift.instrument_source',side_effect=lambda s:s), \
             patch('tools.lift.instrument_graphics',side_effect=lambda s:s), \
             patch('tools.lift.subprocess.run',return_value=subprocess.CompletedProcess([],0)):
            generate_lift(elf,functions,imports,root/'sdk',out,hooks=names)
        return json.loads((out/'provenance.json').read_text()), (out/'ppu_recomp_000.cpp').read_text()

    def test_hooks_recorded_only_when_applied(self):
        names=self.hook_modules(mag_hook_f="return source+'// hooked\\n'")
        receipt,source=self.fake_lift(names,retail=True)
        self.assertEqual(([h['module'] for h in receipt['extension_hooks']],source.endswith('// hooked\n')),(['mag_hook_f'],True))
        receipt,source=self.fake_lift(names,retail=False)
        self.assertEqual(('extension_hooks' in receipt,source.endswith('// hooked\n')),(False,False))

    def test_existing_output_cannot_be_reused_for_changed_inputs(self):
        self.out.mkdir(); (self.out/'old.cpp').write_text('old input')
        with self.assertRaises(FileExistsError):
            run_generator([sys.executable, '-c', 'pass'], self.out)
        self.assertEqual((self.out/'old.cpp').read_text(), 'old input')
