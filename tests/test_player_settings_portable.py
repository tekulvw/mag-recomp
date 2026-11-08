"""Windows and Linux player settings: decoder, digest, settings file, overlay and launch hook.

All inputs are synthetic. The driver builds on the host, without zlib, as the portable players do.
"""
import hashlib
import json
import os
import pathlib
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from tools import psarc

ROOT = pathlib.Path(__file__).resolve().parents[1]
MEMBER = '/run/mag/ps3/settings/maglocalsettings.lua'
ARCHIVE = pathlib.Path('PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat')
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


def synthetic_archive(script, compress=True):
    chunks = [(MEMBER + '\n/asset.bin\n').encode(), script, bytes(range(200))]
    stored = [min((c, zlib.compress(c, 9)), key=len) if compress else c for c in chunks]
    toc = 32 + 30 * len(chunks) + 2 * len(chunks); offset = toc
    data = bytearray(struct.pack('>4sI4sIIIII', b'PSAR', 0x10003, b'zlib', toc, 30, len(chunks), 65536, 2))
    for i, (raw, encoded) in enumerate(zip(chunks, stored)):
        data += bytes(16) + i.to_bytes(4, 'big') + len(raw).to_bytes(5, 'big') + offset.to_bytes(5, 'big'); offset += len(encoded)
    data += b''.join(len(c).to_bytes(2, 'big') for c in stored) + b''.join(stored)
    return bytes(data)


@unittest.skipUnless(COMPILER, 'C++ compiler')
class PortablePlayerSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); root = pathlib.Path(cls.temp.name)
        # No game SDK or encrypted inputs: the fixture refuses encrypted data.
        (root / 'sdata_decrypt.h').write_text('#include <stddef.h>\n#include <stdint.h>\n'
            'static inline uint8_t* sdata_decrypt(const uint8_t*,size_t,size_t*) { return NULL; }\n')
        cls.binary = root / 'driver'
        sources = ['tests/player_settings_portable.cpp', 'src/player_settings_portable.cpp',
                   'src/player_server_overlay_portable.cpp', 'src/player_archive.cpp',
                   'src/player_inflate.cpp', 'src/player_sha256.cpp']
        result = subprocess.run([COMPILER, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-DMAG_PLAYER_ARCHIVE_NO_ZLIB=1',
                                 '-I' + str(ROOT / 'src'), '-I' + str(root), *[str(ROOT / s) for s in sources],
                                 '-o', str(cls.binary)], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_driver(self, *args, env=None):
        return subprocess.run([str(self.binary), *map(str, args)], capture_output=True, timeout=30, env=env)

    def test_decoder_matches_zlib_on_every_block_type(self):
        rng = random.Random(7)
        samples = [b'', b'a', b'MAGClientNetConfig = { EnableNetwork = true }\n' * 40,
                   bytes(rng.randrange(256) for _ in range(70000)), bytes(rng.randrange(4) for _ in range(50000))]
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'z'
            for sample in samples:
                for level in (0, 1, 6, 9):  # stored, fixed and dynamic blocks
                    for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY):
                        encoder = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
                        encoded = encoder.compress(sample) + encoder.flush()
                        path.write_bytes(encoded)
                        result = self.run_driver('inflate', path, len(sample))
                        self.assertEqual(result.returncode, 0, (len(sample), level, strategy))
                        self.assertEqual(result.stdout, sample)
            encoded = zlib.compress(samples[2])
            for bad in (encoded[:-1], encoded[:-4] + bytes(4), b'\x78\x9c' + encoded[2:10], b'\x00' + encoded[1:]):
                path.write_bytes(bad)
                self.assertNotEqual(self.run_driver('inflate', path, len(samples[2])).returncode, 0)
            path.write_bytes(encoded)
            self.assertNotEqual(self.run_driver('inflate', path, len(samples[2]) + 1).returncode, 0)
            self.assertNotEqual(self.run_driver('inflate', path, len(samples[2]) - 1).returncode, 0)

    def test_digest_matches_sha256(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'data'
            for size in (0, 1, 3, 55, 56, 63, 64, 65, 119, 120, 1000, 100000):
                data = random.Random(size).randbytes(size); path.write_bytes(data)
                self.assertEqual(self.run_driver('sha256', path).stdout.decode().strip(), hashlib.sha256(data).hexdigest())

    def test_settings_file_rules(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'settings.json'
            def parse(value):
                path.write_text(value if isinstance(value, str) else json.dumps(value))
                result = self.run_driver('parse', path)
                return (json.loads(result.stdout) if result.returncode == 0 else None), result.stderr.decode()
            settings, _ = parse({})
            self.assertEqual(settings, {'schema_version': 1, 'server_address': 'play.magtest.online',
                                        'tcp_port': 0, 'udp_port': 0, 'player_name': ''})
            # A macOS settings file is accepted; its other fields are ignored.
            settings, _ = parse({'schema_version': 1, 'server_address': ' Example.ORG ', 'tcp_port': 1, 'udp_port': 2,
                                 'display_mode': 'windowed', 'render_scale': 100, 'player_name': 'Rook_7'})
            self.assertEqual((settings['server_address'], settings['tcp_port'], settings['udp_port'], settings['player_name']),
                             ('example.org', 1, 2, 'Rook_7'))
            self.assertEqual(parse({'server_address': ''})[0]['server_address'], '')
            self.assertEqual(parse({'server_address': '192.0.2.5'})[0]['server_address'], '192.0.2.5')
            for bad in ({'schema_version': 2}, {'server_address': 'http://x.org'}, {'server_address': 'x.org:80'},
                        {'server_address': '-x.org'}, {'server_address': '300.1.1.1'}, {'server_address': 5},
                        {'tcp_port': 70000}, {'tcp_port': '1'}, {'udp_port': -1}, {'player_name': 'ab'},
                        {'player_name': '7up'}, {'player_name': 'a' * 17}, {'player_name': 'bad name'},
                        '{"tcp_port": 1.5}', '{"a": 1, "a": 2}', '[]', '{"a": 1} x', '{"a" 1}'):
                self.assertIsNone(parse(bad)[0], bad)

    def vfs(self, root, script):
        vfs = root / 'vfs'; source = vfs / ARCHIVE; source.parent.mkdir(parents=True)
        source.write_bytes(synthetic_archive(script))
        (source.parent / 'other.asset').write_bytes(b'unchanged sibling')
        (vfs / 'PS3_GAME/PARAM.SFO').write_bytes(b'sfo')
        return vfs

    def test_overlay_view_reuse_and_refusals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp); script = b'MAGClientNetConfig = { UseNPTicket = true }\n'
            vfs = self.vfs(root, script); owned = root / 'owned'; sha = hashlib.sha256(script).hexdigest()
            original = (vfs / ARCHIVE).read_bytes()
            bad = self.run_driver('overlay', vfs, owned, '127.0.0.1', '0' * 64)
            self.assertEqual(bad.returncode, 1); self.assertTrue(bad.stdout.startswith(b'2\n'))  # archive error
            self.assertIn(b'supported retail version', bad.stdout); self.assertFalse((owned / 'vfs').exists())
            blank = self.run_driver('overlay', vfs, owned, '', sha)
            self.assertEqual(blank.stdout.split(b'\n')[:2], [b'0', str(vfs).encode()])
            good = self.run_driver('overlay', vfs, owned, '127.0.0.1', sha)
            self.assertEqual(good.returncode, 0, good.stdout)
            view = pathlib.Path(good.stdout.split(b'\n')[1].decode())
            self.assertEqual(view, owned / 'vfs')
            _, _, extract, *_ = psarc.parse((view / ARCHIVE).read_bytes())
            text = extract(1)
            self.assertTrue(text.startswith(script))
            for line in (b'MAGClientNetConfig.UseNPTicket = false', b'MAPSServerAddr = "127.0.0.1"',
                         b'MAGClientNetConfig.MAPSServerTCPPort = 54321', b'MAGClientNetConfig.EnableServerOnClient = false'):
                self.assertIn(line, text)
            self.assertEqual(extract(2), bytes(range(200)))
            self.assertEqual((vfs / ARCHIVE).read_bytes(), original)
            sibling = view / ARCHIVE.parent / 'other.asset'
            self.assertEqual(sibling.read_bytes(), b'unchanged sibling')
            self.assertEqual(sibling.stat().st_ino, (vfs / ARCHIVE.parent / 'other.asset').stat().st_ino)  # a link, not a copy
            self.assertFalse(sibling.is_symlink())
            inode = (view / ARCHIVE).stat().st_ino
            self.assertEqual(self.run_driver('overlay', vfs, owned, '127.0.0.1', sha).returncode, 0)
            self.assertEqual((view / ARCHIVE).stat().st_ino, inode)  # reused
            (view / ARCHIVE).write_bytes(b'changed')  # a damaged view is rebuilt, never trusted
            self.assertEqual(self.run_driver('overlay', vfs, owned, '127.0.0.1', sha).returncode, 0)
            self.assertNotEqual((view / ARCHIVE).read_bytes(), b'changed')
            self.assertEqual(self.run_driver('overlay', vfs, owned, '127.0.0.2', sha).returncode, 0)
            self.assertIn(b'"127.0.0.2"', psarc.parse((view / ARCHIVE).read_bytes())[2](1))
            self.assertEqual((vfs / ARCHIVE.parent / 'other.asset').read_bytes(), b'unchanged sibling')
            self.assertEqual((vfs / ARCHIVE).read_bytes(), original)
            self.assertEqual(sorted(p.name for p in owned.iterdir()), ['server-overlay.txt', 'vfs'])

    def prepare(self, root, **variables):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('MAG_', 'PS3'))}
        env.update(variables)
        result = self.run_driver('prepare', env=env)
        values = dict(line.split('=', 1) for line in result.stdout.decode().splitlines())
        return result.returncode, values, result.stderr.decode()

    def test_launch_hook(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp); vfs = self.vfs(root, b'-- not the retail member\n')
            settings = root / 'config/MAG/settings.json'; owned = root / 'owned'
            common = {'MAG_PLAYER_SETTINGS_PATH': str(settings), 'MAG_PLAYER_OVERLAY_ROOT': str(owned), 'PS3_VFS_ROOT': str(vfs)}
            # Inert without the supervisor's settings path.
            code, values, _ = self.prepare(root, PS3_VFS_ROOT=str(vfs))
            self.assertEqual((code, values['PS3_VFS_ROOT'], values['PS3_NP_ONLINE_ID']), (0, str(vfs), ''))
            self.assertFalse(settings.exists())
            # First run: options are validated and saved; the name reaches the runtime and the keyboard.
            code, _, err = self.prepare(root, **common, MAG_PLAYER_SETUP='1', MAG_PLAYER_SET_NAME='no')
            self.assertEqual(code, 9); self.assertIn('--name', err); self.assertFalse(settings.exists())
            code, values, err = self.prepare(root, **common, MAG_PLAYER_SETUP='1',
                                             MAG_PLAYER_SET_SERVER='127.0.0.1', MAG_PLAYER_SET_NAME='Rook_7')
            self.assertEqual(code, 0, err)
            self.assertEqual(json.loads(settings.read_text())['server_address'], '127.0.0.1')
            self.assertEqual(values['PS3_NP_ONLINE_ID'], 'Rook_7'); self.assertEqual(values['PS3RECOMP_OSK_INITIAL_TEXT'], 'Rook_7')
            # A non-retail archive never hangs or fails the launch: it is reported, and play is offline.
            self.assertIn('Playing offline', err)
            self.assertEqual((values['PS3_VFS_ROOT'], values['PS3_NET_ONLINE']), (str(vfs), ''))
            # The engine phase applies the launcher's snapshot, not a file edited meanwhile.
            settings.write_text(json.dumps({'player_name': 'Other'}))
            code, values, _ = self.prepare(root, **common)
            self.assertEqual((code, values['PS3_NP_ONLINE_ID']), (0, 'Rook_7'))
            # A damaged file is reported, kept, and the launch plays offline with no server.
            settings.write_text('{broken')
            code, values, err = self.prepare(root, **common, MAG_PLAYER_SETUP='1')
            self.assertEqual(code, 0); self.assertIn('Playing offline', err); self.assertEqual(settings.read_text(), '{broken')
            self.assertEqual(values['PS3_NET_ONLINE'], '')
            # No snapshot: the engine refuses visibly.
            (owned / 'launch-settings.json').unlink()
            self.assertEqual(self.prepare(root, **common)[0], 9)


if __name__ == '__main__':
    unittest.main()
