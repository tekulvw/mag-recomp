import struct, unittest
from tools.jobio import parse, summary, changed_ranges, GET, PUT, ZERO

def build(events, ls=64, gpr=16):
    body = b''
    for kind, cmd, lsa, ea, data in events:
        size = len(data) if kind != ZERO else 128
        body += struct.pack('<7I', kind, cmd, lsa, ea, size, 0x3260, 0) + (b'' if kind == ZERO else data)
    entry = bytes(range(ls)); exit_ = bytearray(entry); exit_[10] ^= 0xFF
    head = struct.pack('<8I', 0x4A4F4249, 1, 7, 0x12D28, ls, gpr, 0, len(body))
    return head + struct.pack('<2Q', 100, 3100000) + entry + bytes(gpr) + body + bytes(exit_) + bytes(gpr)

class Tests(unittest.TestCase):
    def test_round_trip(self):
        rec = parse(build([(GET, 0x40, 0x100, 0x1000, b'abcd'), (PUT, 0x20, 0x200, 0x2000, b'xy'), (ZERO, 0x2000, 0, 0x3000, b'')]))
        self.assertEqual([e['kind'] for e in rec['events']], [GET, PUT, ZERO])
        self.assertEqual(rec['events'][0]['data'], b'abcd')
        s = summary(rec)
        self.assertEqual((s['events'], s['by_kind']['GET'], s['bytes_by_kind']['PUT'], s['ls_changed_ranges']), (3, 1, 2, 1))
        self.assertAlmostEqual(s["duration_ms"], 3.0999)
    def test_transfer_only_format_never_claims_full_replay(self):
        raw = bytearray(build([(GET, 0x44, 0x100, 0x1000, b'abcd'), (PUT, 0x20, 0x200, 0x2000, b'xy')]))
        struct.pack_into('<I', raw, 4, 2)
        rec = parse(raw)
        self.assertEqual(rec['format_version'], 2)
        self.assertFalse(rec['full_job_replay'])
        self.assertTrue(rec['atomic_observations_omitted'])
        self.assertEqual([e['data'] for e in rec['events']], [b'abcd', b'xy'])
        self.assertFalse(summary(rec)['full_job_replay'])
        self.assertTrue(parse(build([]))['full_job_replay'])

    def test_truncated_version_one_does_not_claim_full_replay(self):
        raw = bytearray(build([]))
        struct.pack_into('<I', raw, 24, 1)
        rec = parse(raw)
        self.assertTrue(rec['truncated'])
        self.assertFalse(rec['full_job_replay'])

    def test_rejects_truncated(self):
        with self.assertRaises(ValueError):
            parse(build([(GET, 0x40, 0, 0, b'abcd')])[:-1])
    def test_changed_ranges_merge_nearby(self):
        a = bytes(100); b = bytearray(a); b[5] = 1; b[10] = 1; b[80] = 1
        self.assertEqual(changed_ranges(a, bytes(b)), [(5, 11), (80, 81)])
if __name__ == '__main__':
    unittest.main()
