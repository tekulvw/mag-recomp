"""Server override edits only a synthetic archive's selected member."""
import pathlib
import hashlib
import platform
import struct
import zlib
import shutil
import subprocess
import tempfile
import unittest
from tests.test_psarc import archive
from tools import psarc

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler and zlib')
class PlayerArchiveTests(unittest.TestCase):
    def test_native_replacement_preserves_original_and_unrelated_members(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);binary=root/'rewrite'
            result=subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'src'),str(ROOT/'tests/player_archive.cpp'),str(ROOT/'src/player_archive.cpp'),
                '-lz','-o',str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            source=root/'source';target=root/'target';original=archive();source.write_bytes(original)
            result=subprocess.run([str(binary),str(source),str(target)],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(source.read_bytes(),original)
            _,names,extract,*_=psarc.parse(target.read_bytes())
            self.assertEqual(names,['/settings.lua','/asset.bin'])
            self.assertEqual(extract(1),b'MAGClientNetConfig.MAPSServerAddr = "192.0.2.5"\n')
            self.assertEqual(extract(2),bytes(range(200)))
            target.unlink()
            shared=bytearray(original);shared[108:112]=(1).to_bytes(4,'big')
            malformed=[original[:31],original[:100],b'NPD '+original[4:],bytes(shared)]
            for data in malformed:
                source.write_bytes(data)
                result=subprocess.run([str(binary),str(source),str(target)],capture_output=True,text=True,timeout=5)
                self.assertNotEqual(result.returncode,0)
                self.assertFalse(target.exists())

    @unittest.skipUnless(platform.system() == 'Darwin', 'native overlay with CommonCrypto')
    def test_private_vfs_override_identity_reuse_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);binary=root/'overlay'
            # No game SDK or encrypted inputs: the fixture refuses encrypted data.
            (root/'sdata_decrypt.h').write_text('#include <stddef.h>\n#include <stdint.h>\nstatic inline uint8_t* sdata_decrypt(const uint8_t*,size_t,size_t*) { return NULL; }\n')
            obj=root/'store.o'
            subprocess.run(['clang','-fobjc-arc','-c',str(ROOT/'src/player_settings_store.m'),'-o',str(obj)],check=True,capture_output=True)
            result=subprocess.run(['clang++','-std=c++17','-fobjc-arc','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-I'+str(ROOT/'src'),'-I'+str(root),
                str(ROOT/'tests/player_server_overlay.mm'),str(ROOT/'src/player_server_overlay.mm'),
                str(ROOT/'src/player_archive.cpp'),str(obj),'-framework','Foundation','-lz','-o',str(binary)],
                capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            member='/run/mag/ps3/settings/maglocalsettings.lua'
            script=b'MAGClientNetConfig = { UseNPTicket = true }\n'
            chunks=[(member+'\n/asset.bin\n').encode(),script,bytes(range(200))]
            stored=[min((c,zlib.compress(c)),key=len) for c in chunks]
            toc=32+30*len(chunks)+2*len(chunks);offset=toc
            data=bytearray(struct.pack('>4sI4sIIIII',b'PSAR',0x10003,b'zlib',toc,30,len(chunks),65536,2))
            for i,(raw,encoded) in enumerate(zip(chunks,stored)):
                data+=bytes(16)+i.to_bytes(4,'big')+len(raw).to_bytes(5,'big')+offset.to_bytes(5,'big');offset+=len(encoded)
            data+=b''.join(len(c).to_bytes(2,'big') for c in stored)+b''.join(stored)
            vfs=root/'source';archive_path=pathlib.Path('PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat')
            source=vfs/archive_path;source.parent.mkdir(parents=True);source.write_bytes(data)
            sibling=source.parent/'other.asset';sibling.write_bytes(b'unchanged sibling')
            owned=root/'owned';owned.mkdir();sha=hashlib.sha256(script).hexdigest()
            run=lambda digest:subprocess.run([str(binary),str(vfs),str(owned),digest],capture_output=True,text=True,timeout=10)
            bad=run('0'*64);self.assertNotEqual(bad.returncode,0);self.assertFalse((owned/'vfs').exists())
            good=run(sha);self.assertEqual(good.returncode,0,good.stderr)
            output=owned/'vfs'/archive_path
            _,_,extract,*_=psarc.parse(output.read_bytes())
            self.assertTrue(extract(1).startswith(script))
            self.assertIn(b'UseNPTicket = true',extract(1))
            self.assertIn(b'MAPSServerAddr = "127.0.0.1"',extract(1))
            self.assertIn(b'MAPSServerTCPPort = 54321',extract(1))
            self.assertEqual(extract(2),bytes(range(200)))
            self.assertEqual(source.read_bytes(),data)
            self.assertEqual((output.parent/'other.asset').read_bytes(),b'unchanged sibling')
            self.assertEqual(run(sha).returncode,0)
            output.write_bytes(b'changed');self.assertNotEqual(run(sha).returncode,0)
            self.assertEqual(source.read_bytes(),data)
