"""Synthetic portable player settings persistence; no game inputs or UI."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PortableSettingsTests(unittest.TestCase):
    def test_settings_persistence_and_validation(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory(prefix='mag-portable-settings-') as folder:
            binary = Path(folder) / 'settings-test'
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'src'), str(ROOT/'src/player_settings_portable.cpp'),
                str(ROOT/'tests/player_settings_portable.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), folder], check=True)

    def test_server_overlay_verifies_archive_preserves_assets_and_rejects_tampering(self):
        import hashlib
        import platform
        import struct
        import zlib
        from tools import psarc
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory(prefix='mag-portable-overlay-') as folder:
            root = Path(folder)
            binary = root / 'overlay-test'
            (root/'sdata_decrypt.h').write_text('#include <stddef.h>\n#include <stdint.h>\nstatic inline uint8_t* sdata_decrypt(const uint8_t*,size_t,size_t*) { return NULL; }\n')
            crypto = [] if platform.system() == 'Darwin' else ['-lcrypto']
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-DMAG_TEST_PORTABLE_OVERLAY', '-I'+str(ROOT/'src'), '-I'+str(root),
                str(ROOT/'src/player_settings_portable.cpp'), str(ROOT/'src/player_server_overlay_portable.cpp'),
                str(ROOT/'src/player_archive.cpp'), str(ROOT/'tests/player_settings_portable.cpp'),
                '-lz', *crypto, '-o', str(binary)], check=True)
            script = b'MAGClientNetConfig = { UseNPTicket = true }\n'
            member = '/run/mag/ps3/settings/maglocalsettings.lua'
            chunks = [(member+'\n/asset.bin\n').encode(), script, bytes(range(200))]
            stored = [min((chunk,zlib.compress(chunk)),key=len) for chunk in chunks]
            toc = 32+32*len(chunks)
            offset = toc
            data = bytearray(struct.pack('>4sI4sIIIII',b'PSAR',0x10003,b'zlib',toc,30,len(chunks),65536,2))
            for i,(raw,compressed) in enumerate(zip(chunks,stored)):
                data += bytes(16)+i.to_bytes(4,'big')+len(raw).to_bytes(5,'big')+offset.to_bytes(5,'big')
                offset += len(compressed)
            data += b''.join(len(chunk).to_bytes(2,'big') for chunk in stored)+b''.join(stored)
            archive_path = Path('PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat')
            source_root = root/'original'
            source = source_root/archive_path
            source.parent.mkdir(parents=True)
            source.write_bytes(data)
            sibling = source.parent/'other.asset'
            sibling.write_bytes(b'preserved sibling')
            owned = root/'owned'
            digest = hashlib.sha256(script).hexdigest()
            def run(digest=digest, *extra):
                return subprocess.run([str(binary),str(source_root),str(owned),digest,'overlay',*extra],
                    capture_output=True,text=True,timeout=10)
            result = run('0'*64)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse((owned/'vfs').exists())
            # Replacing a stale marker must never follow a link into source data.
            if platform.system() != 'Windows':
                (owned/'server-overlay.identity').symlink_to(sibling)
            result = run()
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(sibling.read_bytes(),b'preserved sibling')
            target = owned/'vfs'/archive_path
            _,_,extract,*_ = psarc.parse(target.read_bytes())
            self.assertTrue(extract(1).startswith(script))
            for line in [b'MAPSServerAddr = "127.0.0.1"',b'MAPSServerTCPPort = 54321',
                         b'MAPSServerUDPPort = 54322',b'UseNPTicket = false',
                         b'EnableServerOnClient = false',b'UseTestClient = false']:
                self.assertIn(line,extract(1))
            self.assertEqual(extract(2),bytes(range(200)))
            self.assertEqual((target.parent/'other.asset').read_bytes(),b'preserved sibling')
            self.assertEqual(source.read_bytes(),data)
            self.assertEqual(run().returncode,0)
            self.assertNotEqual(run('1'*64).returncode,0)
            blank = run('0'*64,'blank')
            self.assertEqual(blank.returncode,0,blank.stderr)
            self.assertEqual(blank.stdout.strip(),str(source_root))
            target.write_bytes(b'tampered')
            self.assertNotEqual(run().returncode,0)
            self.assertEqual(source.read_bytes(),data)
