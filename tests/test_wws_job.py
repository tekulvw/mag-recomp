import struct, unittest
from tools.wws_job import decode, flags, buffer_set

class Tests(unittest.TestCase):
    def test_flags_fields(self):
        word = (5 << 18) | (1 << 15) | (1 << 12) | (2 << 9) | (3 << 5) | 7
        f = flags(word)
        self.assertEqual((f['length_qwords'], f['input_read'], f['command'], f['buffer_set'], f['buffer']), (5, 1, 2, 3, 7))
    def test_buffer_set(self):
        self.assertEqual(buffer_set(0x106F0100), dict(first_page=0x10, pages_per_buffer=0x6F, buffers=1, first_buffer=0))
    def test_record_layout(self):
        words = [0] * 116; words[64], words[65] = 0x10, 0x81000000; words[112] = 0xAB
        d = decode(struct.pack('>116I', *words))
        self.assertEqual((len(d['commands']), d['dma'][0], d['marker'][0]), (32, dict(size=0x10, ea=0x81000000), 0xAB))
        with self.assertRaises(ValueError):
            decode(b'\0' * 463)
if __name__ == '__main__':
    unittest.main()
