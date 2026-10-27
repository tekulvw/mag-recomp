import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

from tools.prepare_inputs import validate_ppu_elf, verify_hash


def fixture():
    data = bytearray(512)
    data[:16] = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH', data, 16,
                     2, 21, 1, 0x10100, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    struct.pack_into('>IIQQQQQQ', data, 64,
                     1, 5, 0, 0x10000, 0x10000, 512, 512, 0x10000)
    struct.pack_into('>II', data, 0x100, 0x10080, 0x11000)
    return data


class InputTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'test.elf'

    def test_valid_ppu_entry_descriptor(self):
        self.path.write_bytes(fixture())
        result = validate_ppu_elf(self.path)
        self.assertEqual(result['entry_code'], '0x10080')
        self.assertEqual(result['entry_toc'], '0x11000')

    def test_rejects_wrong_formats_and_truncated_segments(self):
        cases = []
        for offset, value in [(0, b'SCE\0'), (4, b'\x01'), (5, b'\x01'),
                              (18, b'\x00\x3e'), (80, (0xffffffff).to_bytes(8, 'big'))]:
            data = fixture(); data[offset:offset+len(value)] = value; cases.append(data)
        cases.extend([fixture()[:50], fixture()[:400]])
        for data in cases:
            with self.subTest(data=data[:24]):
                self.path.write_bytes(data)
                with self.assertRaises(ValueError):
                    validate_ppu_elf(self.path)

    def test_entry_must_resolve_to_executable_segment(self):
        data = fixture(); struct.pack_into('>I', data, 0x100, 0x20000)
        self.path.write_bytes(data)
        with self.assertRaises(ValueError):
            validate_ppu_elf(self.path)

    def test_changed_input_rejected(self):
        self.path.write_bytes(b'abc')
        verify_hash(self.path, 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
        self.path.write_bytes(b'abd')
        with self.assertRaises(ValueError):
            verify_hash(self.path, 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
