import sys
import subprocess
import tempfile
import unittest
import json
from unittest.mock import patch
from pathlib import Path
from tools.lift import run_generator, guard_generated_source, generate_lift
from tools.prepare_inputs import sha256_file
from tools.inventory import SDK_REVISION


class LiftTests(unittest.TestCase):
    def test_substituted_inventory_map_stops_before_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'builds').mkdir()
            elf=root/'EBOOT.elf'; elf.write_bytes(b'test image')
            expected=sha256_file(elf)
            (root/'builds/retail-v1.0.json').write_text(json.dumps(dict(images=dict(EBOOT=dict(elf_sha256=expected)))))
            functions=root/'reviewed-functions.json'; imports=root/'EBOOT.imports.json'
            functions.write_text('functions'); imports.write_text('imports')
            (root/'summary.json').write_text(json.dumps(dict(elf_sha256=expected,
                sdk_revision=SDK_REVISION,code_end='0x100',
                manifests=dict(functions_sha256=sha256_file(functions),imports_sha256=sha256_file(imports)))))
            for selected in [functions,imports]:
                original=selected.read_text(); selected.write_text('foreign image map')
                with self.subTest(manifest=selected.name), patch('tools.lift.ROOT',root), \
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

    def test_existing_output_cannot_be_reused_for_changed_inputs(self):
        self.out.mkdir(); (self.out/'old.cpp').write_text('old input')
        with self.assertRaises(FileExistsError):
            run_generator([sys.executable, '-c', 'pass'], self.out)
        self.assertEqual((self.out/'old.cpp').read_text(), 'old input')
