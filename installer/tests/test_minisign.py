"""Synthetic tests of the minisign verifier: RFC 8032 vectors, throwaway keys made at run time,
and, when the minisign command is installed, keys and signatures made by real minisign."""
import base64
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import minisign_signer as signer
from mag_installer import minisign
from mag_installer.minisign import MinisignError

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
REVISION = '0123456789abcdef0123456789abcdef01234567'
RELEASE = f'mag-recomp release v1.2.3 source {REVISION}'

# RFC 8032 section 7.1, tests 1, 2, 3 and SHA(abc): (seed, public key, message, signature).
RFC8032 = [
    ('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60',
     'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a', '',
     'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46b'
     'd25bf5f0595bbe24655141438e7a100b'),
    ('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb',
     '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c', '72',
     '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c'
     '387b2eaeb4302aeeb00d291612bb0c00'),
    ('c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7',
     'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025', 'af82',
     '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc659'
     '4a7c15e9716ed28dc027beceea1ec40a'),
    ('833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42',
     'ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf',
     'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd'
     '454d4423643ce80e2a9ac94fa54ca49f',
     'dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e030'
     '3dca179c138ac17ad9bef1177331a704'),
]
ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
ORDER = 2 ** 252 + 27742317777372353535851937790883648493


def lines(text):
    return text.split('\n')


def replace_line(text, index, value):
    parts = lines(text)
    parts[index] = value
    return '\n'.join(parts)


class Ed25519Tests(unittest.TestCase):
    def test_rfc8032_vectors_verify_and_the_test_signer_reproduces_them(self):
        for seed, public_key, message, signature in RFC8032:
            seed, public_key, message, signature = map(bytes.fromhex, (seed, public_key, message, signature))
            with self.subTest(public_key=public_key.hex()[:8]):
                minisign.ed25519_verify(public_key, message, signature)
                self.assertEqual(signer.ed25519_public_key(seed), public_key)
                self.assertEqual(signer.ed25519_sign(seed, message), signature)
                with self.assertRaises(MinisignError):
                    minisign.ed25519_verify(public_key, message + b'x', signature)
                changed = bytearray(signature)
                changed[5] ^= 1
                with self.assertRaises(MinisignError):
                    minisign.ed25519_verify(public_key, message, bytes(changed))

    def test_non_canonical_scalar_and_points_are_refused(self):
        _, public_key, message, signature = map(bytes.fromhex, RFC8032[1])
        s = int.from_bytes(signature[32:], 'little')
        with self.assertRaisesRegex(MinisignError, 'scalar'):
            minisign.ed25519_verify(public_key, message, signature[:32] + (s + ORDER).to_bytes(32, 'little'))
        prime = 2 ** 255 - 19
        for encoded in ((prime + 1).to_bytes(32, 'little'),           # y >= p
                        (1 | (1 << 255)).to_bytes(32, 'little'),      # x = 0 with the sign bit set
                        (2).to_bytes(32, 'little')):                  # y = 2 is not on the curve
            with self.subTest(encoded=encoded.hex()), self.assertRaises(MinisignError):
                minisign.ed25519_verify(encoded, message, signature)
            with self.subTest(r=encoded.hex()), self.assertRaises(MinisignError):
                minisign.ed25519_verify(public_key, message, encoded + signature[32:])


class MinisignFormatTests(unittest.TestCase):
    def setUp(self):
        self.secret, self.line, self.key_id = signer.keypair(b'release test key')
        self.data = b'0' * 64 + b'  mag-installer-v1.2.3-linux-x86_64.tar.gz\n'
        self.text = signer.sign(self.secret, self.data, RELEASE)

    def verify(self, text=None, data=None, keys=None):
        return minisign.verify(self.data if data is None else data, self.text if text is None else text,
                               [self.line] if keys is None else keys)

    def test_prehashed_signature_verifies_and_returns_its_trusted_comment(self):
        self.assertEqual(self.verify(), RELEASE)
        self.assertEqual(self.verify(self.text.encode()), RELEASE)
        self.assertEqual(self.verify(self.text.replace('\n', '\r\n')), RELEASE)
        self.assertEqual(self.verify(keys=[signer.public_key_text(self.secret)]), RELEASE)
        self.assertEqual(self.verify(keys=[(self.secret.key_id, self.secret.public_key)]), RELEASE)
        parsed = minisign.parse_signature(self.text)
        self.assertEqual(parsed.algorithm, b'ED')
        self.assertEqual(minisign.key_id_hex(parsed.key_id), self.key_id)
        self.assertEqual(parsed.trusted_comment, RELEASE)
        self.assertEqual(len(parsed.signature), 64)
        self.assertEqual(len(parsed.global_signature), 64)

    def test_public_key_parsing(self):
        key_id, public_key = minisign.parse_public_key(self.line)
        self.assertEqual((key_id, public_key), (self.secret.key_id, self.secret.public_key))
        self.assertEqual(minisign.parse_public_key(signer.public_key_text(self.secret)), (key_id, public_key))
        self.assertRegex(self.key_id, r'^[0-9A-F]{16}$')
        self.assertEqual(self.key_id, key_id[::-1].hex().upper())
        other = signer.keypair(b'other')[0]
        wrong_comment = f'untrusted comment: minisign public key {other.key_id_hex}\n{self.line}\n'
        for text in (wrong_comment, 'x' + self.line, self.line[:-4], 'comment\n' + self.line,
                     base64.b64encode(b'EE' + bytes(40)).decode()):
            with self.subTest(text=text[:20]), self.assertRaises(MinisignError):
                minisign.parse_public_key(text)

    def test_tampered_data_signature_or_comment_is_refused(self):
        with self.assertRaisesRegex(MinisignError, 'signed file'):
            self.verify(data=self.data + b'\n')
        signature_line = lines(self.text)[1]
        raw = bytearray(base64.b64decode(signature_line))
        raw[40] ^= 0x10
        with self.assertRaises(MinisignError):
            self.verify(replace_line(self.text, 1, base64.b64encode(bytes(raw)).decode()))
        changed = replace_line(self.text, 2, 'trusted comment: ' + RELEASE.replace('v1.2.3', 'v1.2.4'))
        with self.assertRaisesRegex(MinisignError, 'global signature'):
            self.verify(changed)
        global_raw = bytearray(base64.b64decode(lines(self.text)[3]))
        global_raw[0] ^= 1
        with self.assertRaisesRegex(MinisignError, 'global signature'):
            self.verify(replace_line(self.text, 3, base64.b64encode(bytes(global_raw)).decode()))
        # Another key's global signature over the same comment does not transfer.
        other = signer.keypair(b'other key', key_id=self.secret.key_id)[0]
        foreign = signer.sign(other, self.data, RELEASE)
        with self.assertRaises(MinisignError):
            self.verify(replace_line(self.text, 3, lines(foreign)[3]))

    def test_unknown_key_and_legacy_algorithm_are_refused(self):
        other_secret, other_line, other_id = signer.keypair(b'unknown key')
        with self.assertRaisesRegex(MinisignError, other_id):
            self.verify(signer.sign(other_secret, self.data, RELEASE))
        with self.assertRaisesRegex(MinisignError, 'Legacy'):
            self.verify(signer.sign(self.secret, self.data, RELEASE, prehashed=False))
        raw = bytearray(base64.b64decode(lines(self.text)[1]))
        raw[:2] = b'EX'
        with self.assertRaisesRegex(MinisignError, 'Unsupported'):
            self.verify(replace_line(self.text, 1, base64.b64encode(bytes(raw)).decode()))
        with self.assertRaisesRegex(MinisignError, 'not a trusted key'):
            self.verify(keys=[])
        with self.assertRaisesRegex(MinisignError, 'share one key id'):
            same_id = signer.keypair(b'same id', key_id=self.secret.key_id)[1]
            self.verify(keys=[self.line, same_id])

    def test_malformed_and_oversized_signatures_are_refused(self):
        signature_line = lines(self.text)[1]
        cases = {
            'bad base64': replace_line(self.text, 1, signature_line[:-2] + '!='),
            'short': replace_line(self.text, 1, signature_line[:-8]),
            'non-canonical base64': replace_line(self.text, 1, signature_line[:-2] + ALPHABET[ALPHABET.index(signature_line[-2]) ^ 1] + '='),
            'missing line': '\n'.join(lines(self.text)[:3]),
            'extra line': self.text + 'extra\n',
            'no untrusted prefix': replace_line(self.text, 0, 'comment: x'),
            'no trusted prefix': replace_line(self.text, 2, RELEASE),
            'oversized': replace_line(self.text, 0, 'untrusted comment: ' + 'x' * 1024),
            'nul': replace_line(self.text, 0, 'untrusted comment: \0'),
            'not utf-8': self.text.encode() + b'\xff',
        }
        for name, text in cases.items():
            with self.subTest(case=name), self.assertRaises(MinisignError):
                self.verify(text)
        with self.assertRaisesRegex(MinisignError, 'at most'):
            self.verify(data=b'x' * 5000)

    def test_trusted_comments_bind_kind_version_and_commit(self):
        self.assertEqual(minisign.release_comment('v1.2.3', REVISION), RELEASE)
        preview = minisign.preview_comment('preview-123', REVISION)
        self.assertEqual(preview, f'mag-recomp preview preview-123 source {REVISION}')
        self.assertEqual(minisign.parse_trusted_comment(RELEASE), ('release', 'v1.2.3', REVISION))
        self.assertEqual(minisign.check_trusted_comment(RELEASE, 'release', 'v1.2.3', REVISION)[1], 'v1.2.3')
        for comment, kind, version, revision in [
                (preview, 'release', 'preview-123', REVISION),
                (preview, 'release', 'v1.2.3', REVISION),
                (RELEASE, 'preview', 'v1.2.3', REVISION),
                (RELEASE, 'release', 'v1.2.4', REVISION),
                (RELEASE, 'release', 'v1.2.3', 'f' * 40),
                (RELEASE + ' ', 'release', 'v1.2.3', REVISION)]:
            with self.subTest(comment=comment, kind=kind, version=version), self.assertRaises(MinisignError):
                minisign.check_trusted_comment(comment, kind, version, revision)
        for bad in (('release', 'preview-1', REVISION), ('preview', 'v1.2.3', REVISION),
                    ('release', 'v1.2', REVISION), ('release', 'v1.2.3', REVISION.upper()),
                    ('nightly', 'v1.2.3', REVISION)):
            with self.subTest(bad=bad), self.assertRaises(MinisignError):
                minisign.trusted_comment(*bad)
        for comment in ('timestamp:1\tfile:SHA256SUMS.txt', f'mag-recomp release v1.2.3 source {REVISION[:-1]}'):
            with self.subTest(comment=comment), self.assertRaises(MinisignError):
                minisign.parse_trusted_comment(comment)


class TrustedKeyListTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'update-signing.json'
        self.release = signer.keypair(b'release')
        self.recovery = signer.keypair(b'recovery')

    def entry(self, key, role):
        return {'id': key[2], 'role': role, 'public_key': key[1]}

    def write(self, keys, **extra):
        profile = {'schema_version': 1, 'repository': 'tekulvw/mag-recomp', 'keys': keys} | extra
        self.path.write_text(json.dumps(profile))

    def test_committed_key_list_is_valid_and_empty_until_the_user_adds_keys(self):
        path = REPOSITORY_ROOT / 'profiles' / 'update-signing.json'
        profile = minisign.load_signing_profile(path)
        self.assertEqual(profile['repository'], 'tekulvw/mag-recomp')
        self.assertIsInstance(minisign.load_trusted_keys(path), tuple)

    def test_release_and_recovery_keys_load_and_verify(self):
        self.write([self.entry(self.release, 'release'), self.entry(self.recovery, 'recovery')])
        keys = minisign.load_trusted_keys(self.path)
        self.assertEqual([(key.id, key.role) for key in keys],
                         [(self.release[2], 'release'), (self.recovery[2], 'recovery')])
        for key in (self.release, self.recovery):
            text = signer.sign(key[0], b'sums', RELEASE)
            self.assertEqual(minisign.verify(b'sums', text, keys), RELEASE)

    def test_invalid_key_lists_are_refused(self):
        good = self.entry(self.release, 'release')
        cases = {
            'id mismatch': [good | {'id': self.recovery[2]}],
            'lowercase id': [good | {'id': good['id'].lower()}],
            'unknown role': [good | {'role': 'backup'}],
            'unknown field': [good | {'comment': 'x'}],
            'missing field': [{'id': good['id'], 'role': 'release'}],
            'full pub text': [good | {'public_key': signer.public_key_text(self.release[0])}],
            'bad base64': [good | {'public_key': good['public_key'][:-3] + '!!='}],
            'duplicate': [good, good | {'role': 'recovery'}],
            'too many of a role': [self.entry(signer.keypair(bytes([n]) * 3), 'release') for n in range(3)],
            'too many': [self.entry(signer.keypair(bytes([n]) * 3), 'release' if n % 2 else 'recovery')
                         for n in range(5)],
            'not a list': {'release': good},
        }
        for name, keys in cases.items():
            with self.subTest(case=name):
                self.write(keys)
                with self.assertRaises(MinisignError):
                    minisign.load_trusted_keys(self.path)
        for extra in ({'schema_version': 2}, {'schema_version': True}, {'repository': 'not a repository'},
                      {'note': 'x'}):
            with self.subTest(extra=extra):
                self.write([good], **extra)
                with self.assertRaises(MinisignError):
                    minisign.load_trusted_keys(self.path)
        self.path.write_text('{')
        with self.assertRaises(MinisignError):
            minisign.load_trusted_keys(self.path)
        with self.assertRaises(MinisignError):
            minisign.load_trusted_keys(Path(self.temp.name) / 'missing.json')
        self.write([good])
        link = Path(self.temp.name) / 'link.json'
        try:
            link.symlink_to(self.path)
        except OSError:
            return
        with self.assertRaises(MinisignError):
            minisign.load_trusted_keys(link)


MINISIGN = shutil.which('minisign')


def minisign_version():
    result = subprocess.run([MINISIGN, '-v'], capture_output=True, text=True, timeout=30, check=False)
    match = re.search(r'(\d+)\.(\d+)', result.stdout + result.stderr)
    return (int(match.group(1)), int(match.group(2))) if match else (0, 0)


@unittest.skipUnless(MINISIGN, 'the minisign command is not installed')
class RealMinisignTests(unittest.TestCase):
    """Throwaway keys made by real minisign (-W: no password) in a temporary folder."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prehash = [] if minisign_version() >= (0, 11) else ['-H']
        subprocess.run([MINISIGN, '-G', '-W', '-p', str(self.root / 'k.pub'), '-s', str(self.root / 'k.key')],
                       check=True, timeout=60, stdin=subprocess.DEVNULL, capture_output=True)
        self.public = (self.root / 'k.pub').read_text()
        self.data = self.root / 'SHA256SUMS.txt'
        self.data.write_bytes(b'f' * 64 + b'  mag-installer-v1.2.3-macos-arm64.zip\n')

    def test_real_minisign_signature_verifies(self):
        subprocess.run([MINISIGN, '-S', *self.prehash, '-s', str(self.root / 'k.key'), '-m', str(self.data),
                        '-t', RELEASE], check=True, timeout=60, stdin=subprocess.DEVNULL, capture_output=True)
        text = (self.root / 'SHA256SUMS.txt.minisig').read_text()
        key_id, _ = minisign.parse_public_key(self.public)
        self.assertEqual(lines(self.public)[0], f'untrusted comment: minisign public key {minisign.key_id_hex(key_id)}')
        self.assertEqual(minisign.parse_signature(text).algorithm, b'ED')
        self.assertEqual(minisign.verify(self.data.read_bytes(), text, [self.public]), RELEASE)
        self.assertEqual(minisign.verify(self.data.read_bytes(), text, [lines(self.public)[1]]), RELEASE)
        with self.assertRaises(MinisignError):
            minisign.verify(self.data.read_bytes() + b'x', text, [self.public])

    def test_real_minisign_accepts_the_test_signer(self):
        secret, _, _ = signer.keypair(b'interop')
        signer.write_public_key(self.root / 'test.pub', secret)
        signer.write_signature(self.root / 'SHA256SUMS.txt.minisig', signer.sign(secret, self.data.read_bytes(), RELEASE))
        result = subprocess.run([MINISIGN, '-V', '-p', str(self.root / 'test.pub'), '-m', str(self.data)],
                                capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(RELEASE, result.stdout + result.stderr)

    @unittest.skipUnless(MINISIGN and minisign_version() >= (0, 11), 'legacy signing needs minisign 0.11 -l')
    def test_real_legacy_signature_is_refused(self):
        subprocess.run([MINISIGN, '-S', '-l', '-s', str(self.root / 'k.key'), '-m', str(self.data), '-t', RELEASE],
                       check=True, timeout=60, stdin=subprocess.DEVNULL, capture_output=True)
        with self.assertRaisesRegex(MinisignError, 'Legacy'):
            minisign.verify(self.data.read_bytes(), (self.root / 'SHA256SUMS.txt.minisig').read_text(), [self.public])


if __name__ == '__main__':
    unittest.main()
