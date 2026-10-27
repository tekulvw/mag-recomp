import hashlib
import struct
import unittest
import zlib
from tools import psarc


def archive():
    chunks=[b'/settings.lua\n/asset.bin\n', b'UseNPTicket = true\n'*30, bytes(range(200))]
    stored=[min((c,zlib.compress(c)),key=len) for c in chunks]
    toc=32+30*len(chunks)+2*len(chunks)
    data=bytearray(struct.pack('>4sI4sIIIII',b'PSAR',0x10003,b'zlib',toc,30,len(chunks),65536,2))
    offset=toc
    for i,(raw,encoded) in enumerate(zip(chunks,stored)):
        data+=bytes(16)+i.to_bytes(4,'big')+len(raw).to_bytes(5,'big')+offset.to_bytes(5,'big')
        offset+=len(encoded)
    data+=b''.join(len(c).to_bytes(2,'big') for c in stored)+b''.join(stored)
    return bytes(data)


class PsarcTests(unittest.TestCase):
    def test_replace_preserves_unrelated_members_and_records_identity(self):
        source=archive(); payload=b'UseNPTicket = false\n'
        result,report=psarc.replace(source,'/settings.lua',payload)
        entries,names,extract,*_=psarc.parse(result)
        self.assertEqual(extract(1),payload)
        self.assertEqual(extract(2),bytes(range(200)))
        self.assertEqual(extract(0),b'/settings.lua\n/asset.bin\n')
        self.assertEqual(report['source_sha256'],hashlib.sha256(source).hexdigest())
        self.assertEqual(report['unrelated_members_unchanged'],2)
        self.assertEqual(source,archive())

    def test_reject_missing_oversize_and_shared_blocks(self):
        with self.assertRaises(ValueError):psarc.replace(archive(),'/missing',b'x')
        with self.assertRaises(ValueError):psarc.replace(archive(),'/settings.lua',b'x'*65537)
        data=bytearray(archive());data[32+2*30+16:32+2*30+20]=(1).to_bytes(4,'big')
        with self.assertRaises(ValueError):psarc.replace(bytes(data),'/settings.lua',b'x')

    def test_reject_truncation_and_bad_header(self):
        for source in [archive()[:31],archive()[:100],b'NPD '+archive()[4:]]:
            with self.subTest(size=len(source)):
                with self.assertRaises(ValueError):psarc.parse(source)
