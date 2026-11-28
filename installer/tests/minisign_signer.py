"""Test-only minisign signer: deterministic throwaway keys and `.minisig` text.

For synthetic tests only. Keys come from a seed chosen by the test, so nothing secret exists and
nothing is ever committed: tests generate their keys and signatures at run time. The Ed25519 code
is a small RFC 8032 (section 5.1.5 and 5.1.6) reference implementation, written independently of
the verifier in mag_installer/minisign.py so each checks the other. It is slow and not
constant-time, which does not matter for test keys.

Importing this module has no side effects. Other test suites (for example the launcher's C++
tests at the repository root) import it by path, so its API is stable:

    secret, public_key_line, key_id_hex = keypair(seed)
    text = sign(secret, data, trusted_comment)            # prehashed ED, the full .minisig text
    text = sign(secret, data, trusted_comment, prehashed=False)  # legacy Ed, for refusal tests
    public_key_text(secret)                               # the full .pub text
"""
from __future__ import annotations

import base64
import hashlib
from pathlib import Path
from typing import NamedTuple

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P
DEFAULT_UNTRUSTED_COMMENT = 'signature from minisign secret key'


def _inverse(value):
    return pow(value, P - 2, P)


def _point_add(first, second):
    # Projective twisted Edwards addition with a = -1 ("add-2008-bbjlp"); complete for edwards25519.
    x1, y1, z1 = first
    x2, y2, z2 = second
    a = z1 * z2 % P
    b = a * a % P
    c = x1 * x2 % P
    d = y1 * y2 % P
    e = D * c * d % P
    f, g = (b - e) % P, (b + e) % P
    x3 = a * f * ((x1 + y1) * (x2 + y2) - c - d) % P
    y3 = a * g * (d + c) % P
    return x3, y3, f * g % P


def _scalar_multiply(scalar, point):
    result = (0, 1, 1)
    addend = point
    while scalar > 0:
        if scalar & 1:
            result = _point_add(result, addend)
        addend = _point_add(addend, addend)
        scalar >>= 1
    return result


def _recover_x(y, sign):
    x2 = (y * y - 1) * _inverse(D * y * y + 1) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * pow(2, (P - 1) // 4, P) % P
    if x & 1 != sign:
        x = P - x
    return x


_BASE_Y = 4 * _inverse(5) % P
BASE = (_recover_x(_BASE_Y, 0), _BASE_Y, 1)


def _encode(point):
    x, y, z = point
    inverse = _inverse(z)
    x, y = x * inverse % P, y * inverse % P
    return (y | ((x & 1) << 255)).to_bytes(32, 'little')


def _expand(seed):
    if len(seed) != 32:
        raise ValueError('An Ed25519 seed is 32 bytes')
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], 'little')
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, digest[32:]


def ed25519_public_key(seed):
    """The 32-byte public key of a 32-byte Ed25519 seed."""
    scalar, _ = _expand(seed)
    return _encode(_scalar_multiply(scalar, BASE))


def ed25519_sign(seed, message, public_key=None):
    """A 64-byte RFC 8032 Ed25519 signature of `message`."""
    scalar, prefix = _expand(seed)
    if public_key is None:
        public_key = _encode(_scalar_multiply(scalar, BASE))
    r = int.from_bytes(hashlib.sha512(prefix + message).digest(), 'little') % L
    encoded_r = _encode(_scalar_multiply(r, BASE))
    k = int.from_bytes(hashlib.sha512(encoded_r + public_key + message).digest(), 'little') % L
    return encoded_r + ((r + k * scalar) % L).to_bytes(32, 'little')


class SecretKey(NamedTuple):
    seed: bytes        # 32-byte Ed25519 seed
    key_id: bytes      # 8 bytes, as stored in minisign files
    public_key: bytes  # 32 bytes

    @property
    def key_id_hex(self):
        return self.key_id[::-1].hex().upper()

    @property
    def public_key_line(self):
        return base64.b64encode(b'Ed' + self.key_id + self.public_key).decode('ascii')


def keypair(seed, key_id=None):
    """A deterministic test key: return (secret, public_key_line, key_id_hex).

    A 32-byte `seed` is the Ed25519 seed itself (so RFC 8032 vectors can be reproduced); any other
    length is hashed to one. `key_id` (8 bytes) defaults to a value derived from the seed.
    """
    seed = bytes(seed)
    if len(seed) != 32:
        seed = hashlib.sha256(b'minisign test seed\0' + seed).digest()
    if key_id is None:
        key_id = hashlib.blake2b(seed, digest_size=8, person=b'minisign-test-id').digest()
    key_id = bytes(key_id)
    if len(key_id) != 8:
        raise ValueError('A minisign key id is 8 bytes')
    secret = SecretKey(seed, key_id, ed25519_public_key(seed))
    return secret, secret.public_key_line, secret.key_id_hex


def public_key_text(secret):
    """The text of a `.pub` file, as minisign writes it."""
    return f'untrusted comment: minisign public key {secret.key_id_hex}\n{secret.public_key_line}\n'


def sign(secret, data, trusted_comment, prehashed=True,
         untrusted_comment=DEFAULT_UNTRUSTED_COMMENT):
    """The full `.minisig` text for `data`: algorithm `ED` (prehashed), or legacy `Ed` when not."""
    data = bytes(data)
    if prehashed:
        algorithm, message = b'ED', hashlib.blake2b(data, digest_size=64).digest()
    else:
        algorithm, message = b'Ed', data
    signature = ed25519_sign(secret.seed, message, secret.public_key)
    global_signature = ed25519_sign(secret.seed, signature + trusted_comment.encode('utf-8'),
                                    secret.public_key)
    return (f'untrusted comment: {untrusted_comment}\n'
            f'{base64.b64encode(algorithm + secret.key_id + signature).decode("ascii")}\n'
            f'trusted comment: {trusted_comment}\n'
            f'{base64.b64encode(global_signature).decode("ascii")}\n')


def write_public_key(path, secret):
    Path(path).write_text(public_key_text(secret), encoding='utf-8', newline='\n')
    return Path(path)


def write_signature(path, text):
    Path(path).write_text(text, encoding='utf-8', newline='\n')
    return Path(path)
