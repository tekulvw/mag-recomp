"""Synthetic offline SPU audit boundaries; no game inputs are required."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

from tools import target_spu_audit as audit

SDK = Path(__file__).resolve().parents[1] / 'local/ps3recomp-runtime'


def spu_elf():
    ident = b'\x7fELF\x01\x02\x01' + bytes(9)
    header = struct.pack('>16sHHIIIIIHHHHHH', ident, 2, 23, 1, 0x3000,
                         52, 0, 0, 52, 32, 1, 0, 0, 0)
    segment = struct.pack('>8I', 1, 0x100, 0x3000, 0x3000, 4, 4, 5, 16)
    return (header + segment).ljust(0x100, b'\0') + struct.pack('>I', 0x35000000)


def ppu_with_module():
    raw = bytearray(0x40)
    struct.pack_into('>II', raw, 0x10, 0x30, len(raw))
    raw[0x20:0x24] = b'\xc0\xde\xc0\xde'
    struct.pack_into('>I', raw, 0x28, 0x4000)
    struct.pack_into('>I', raw, 0x30, 0x35000000)
    header = bytearray(64)
    header[:7] = b'\x7fELF\x02\x02\x01'
    struct.pack_into('>Q', header, 0x20, 64)
    struct.pack_into('>HH', header, 0x36, 56, 1)
    ph = struct.pack('>IIQQQQQQ', 1, 5, 0x100, 0x10000, 0, len(raw), len(raw), 16)
    return bytes(header + ph).ljust(0x100, b'\0') + raw


class AuditTests(unittest.TestCase):
    def test_census_retains_unsupported_unresolved_and_indirect_hazards(self):
        report = dict(functions=[[0x100, 0x110]], unsupported={'.word': 2},
                      call_targets=[0x100, 0x200], branch_targets=[0x108])
        source = '/* TODO spu: unresolved branch */;\nspu_indirect_branch(ctx);\n'
        result = audit.generation_census(report, source)
        self.assertEqual(result['unsupported_count'], 2)
        self.assertEqual(result['unresolved_direct_targets'], [0x108, 0x200])
        self.assertEqual(result['todo_count'], 1)
        self.assertEqual(result['indirect_dispatch_count'], 1)
        self.assertFalse(result['runtime_verified'])

    def test_zero_unsupported_does_not_establish_runtime_acceptance(self):
        result = audit.generation_census(dict(functions=[[0, 4]], unsupported={},
                                              call_targets=[], branch_targets=[]), '')
        self.assertEqual(result['unsupported_count'], 0)
        self.assertFalse(result['runtime_verified'])

    def test_failed_subprocess_keeps_partial_outputs_log_and_exit_status(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td)
            command = [sys.executable, '-c',
                       "from pathlib import Path; import sys; Path(sys.argv[1]).write_text('partial'); print('diagnostic'); sys.exit(7)",
                       str(output / 'partial.c')]
            record = audit.run_tool(command, output / 'tool.log', timeout=5)
            self.assertEqual(record['returncode'], 7)
            self.assertIn('diagnostic', (output / 'tool.log').read_text())
            self.assertEqual((output / 'partial.c').read_text(), 'partial')
            self.assertFalse(record['completed'])

    def test_timeout_is_recorded(self):
        with tempfile.TemporaryDirectory() as td:
            record = audit.run_tool([sys.executable, '-c', 'import time; time.sleep(10)'],
                                    Path(td) / 'timeout.log', timeout=0.05)
            self.assertTrue(record['timed_out'])
            self.assertFalse(record['completed'])

    def test_output_is_fresh_and_isolated_before_writing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            with self.assertRaises(ValueError):
                audit.prepare_output(root / 'analysis/retail-v1.0/spu', 'retail-v2.12', root)
            self.assertFalse((root / 'analysis').exists())
            output = audit.prepare_output(root / 'analysis/retail-v2.12/spu', 'retail-v2.12', root)
            with self.assertRaises(ValueError):
                audit.prepare_output(output, 'retail-v2.12', root)

    @unittest.skipUnless((SDK / 'tools/spu_elf.py').is_file(), 'pinned SDK not installed')
    def test_raw_policy_uses_generic_function_discovery_before_generation(self):
        with tempfile.TemporaryDirectory() as td:
            raw = struct.pack('>III', 0x33000100, 0x35000000, 0x35000000)
            row = dict(kind='known_policy', sha256=audit.digest(raw), _raw=raw,
                       base=0xA00, entry=0xA00, code_end=0xA0C)
            audit.lift_program(row, SDK, Path(td), audit.sdk_modules(SDK), timeout=5)
            self.assertEqual(row['census']['functions'], 2)
            self.assertEqual(row['census']['unresolved_direct_targets'], [])

    @unittest.skipUnless((SDK / 'tools/spu_elf.py').is_file(), 'pinned SDK not installed')
    def test_generation_retains_synthetic_code_and_checks_entry_symbol(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / 'fixture.elf'
            source.write_bytes(spu_elf())
            row = audit.discover_programs({'fixture': source}, SDK)[0]
            audit.lift_program(row, SDK, root, audit.sdk_modules(SDK), timeout=5)
            self.assertEqual(row['status'], 'generated')
            self.assertTrue(row['entry_generated'])
            self.assertEqual(row['census']['functions'], 1)
            self.assertEqual(row['census']['unsupported_count'], 0)
            self.assertIn(row['generated_directory'] + '/spu_recomp.c', row['outputs'])
            self.assertFalse(row['abi_verified'])

    @unittest.skipUnless((SDK / 'tools/spu_elf.py').is_file(), 'pinned SDK not installed')
    def test_sectionless_embedded_elf_is_deduplicated_with_occurrences(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.elf'
            path.write_bytes(spu_elf() + bytes(16) + spu_elf())
            rows = audit.discover_programs({'fixture': path}, SDK)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['kind'], 'embedded_elf')
            self.assertEqual(rows[0]['executable_ranges'], [(0x3000, 0x3004)])
            self.assertEqual(len(rows[0]['occurrences']), 2)
            self.assertIsNone(rows[0]['role'])

    @unittest.skipUnless((SDK / 'tools/spu_elf.py').is_file(), 'pinned SDK not installed')
    def test_raw_wws_discovery_records_distinct_payload_identity(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.elf'
            path.write_bytes(ppu_with_module())
            rows = audit.discover_programs({'fixture': path}, SDK)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['kind'], 'raw_wws')
            self.assertEqual(rows[0]['entry'], 0x4030)
            self.assertEqual(rows[0]['occurrences'][0]['guest_ea'], 0x10000)
            self.assertFalse(rows[0]['abi_verified'])

    @unittest.skipUnless((SDK / 'tools/spu_elf.py').is_file(), 'pinned SDK not installed')
    def test_malformed_embedded_candidate_remains_visible(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'synthetic.elf'
            path.write_bytes(spu_elf()[:-1])
            rows = audit.discover_programs({'fixture': path}, SDK)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['status'], 'invalid')
            self.assertIn('available', rows[0]['reason'])


if __name__ == '__main__':
    unittest.main()
