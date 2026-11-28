"""Verify minisign signatures against the trusted release keys.

Standard library only: the installer has no runtime dependencies. Only the prehashed `ED`
algorithm is accepted (BLAKE2b-512 of the file, then Ed25519); the legacy `Ed` algorithm, which
signs the whole file, is refused. The trusted comment, which minisign covers with a second
("global") signature, binds a signature to one release or preview set.

The Ed25519 check follows RFC 8032 section 5.1.7: both points must decode canonically, S must be
below the group order, and [S]B must equal R + [k]A (the cofactorless equation). This module only
verifies; it never creates keys or signatures.

`profiles/update-signing.json` (at the repository root) lists the trusted public keys. Each entry is
`{"id": "<16 uppercase hex>", "role": "release" | "recovery", "public_key": "<base64>"}`, where
`public_key` is the second line of the `.pub` file minisign writes and `id` is the key id minisign
prints on its first line.
"""
from __future__ import annotations

import base64
import binascii
import hashlib
import json
from pathlib import Path
import re
from typing import NamedTuple

MAX_SIGNATURE_BYTES = 1024
MAX_SIGNED_DATA_BYTES = 4096
MAX_PROFILE_BYTES = 64 * 1024
MAX_KEYS = 4
MAX_KEYS_PER_ROLE = 2
ROLES = ('release', 'recovery')
PUBLIC_KEY_ALGORITHM = b'Ed'
PREHASHED = b'ED'
LEGACY = b'Ed'
UNTRUSTED_PREFIX = 'untrusted comment: '
TRUSTED_PREFIX = 'trusted comment: '
RELEASE_TAG = re.compile(r'v[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}')
PREVIEW_VERSION = re.compile(r'preview-[0-9]{1,10}')
REVISION = re.compile(r'[0-9a-f]{40}')
REPOSITORY = re.compile(r'[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}')
COMMENT = re.compile(r'mag-recomp (release|preview) (\S+) source (\S+)')


class MinisignError(ValueError):
    """A signature, key or key list that cannot be trusted."""


# ---------------------------------------------------------------------------------------------
# Ed25519 verification (edwards25519, extended homogeneous coordinates).

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_D2 = 2 * _D % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(first, second):
    x1, y1, z1, t1 = first
    x2, y2, z2, t2 = second
    a = (y1 - x1) * (y2 - x2) % _P
    b = (y1 + x1) * (y2 + x2) % _P
    c = t1 * _D2 * t2 % _P
    d = 2 * z1 * z2 % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return e * f % _P, g * h % _P, f * g % _P, e * h % _P


def _multiply(scalar, point):
    result = (0, 1, 1, 0)
    while scalar:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _same_point(first, second):
    x1, y1, z1, _ = first
    x2, y2, z2, _ = second
    return (x1 * z2 - x2 * z1) % _P == 0 and (y1 * z2 - y2 * z1) % _P == 0


def _decode_point(encoded):
    """RFC 8032 section 5.1.3; refuses every non-canonical or off-curve encoding."""
    if len(encoded) != 32:
        raise MinisignError('An Ed25519 point is 32 bytes')
    value = int.from_bytes(encoded, 'little')
    sign, y = value >> 255, value & ((1 << 255) - 1)
    if y >= _P:
        raise MinisignError('Non-canonical Ed25519 point encoding')
    u = (y * y - 1) % _P
    v = (_D * y * y + 1) % _P
    x = u * pow(v, 3, _P) * pow(u * pow(v, 7, _P), (_P - 5) // 8, _P) % _P
    check = v * x * x % _P
    if check == (-u) % _P:
        x = x * _SQRT_M1 % _P
    elif check != u:
        raise MinisignError('Ed25519 point is not on the curve')
    if x == 0 and sign:
        raise MinisignError('Non-canonical Ed25519 point encoding')
    if x & 1 != sign:
        x = _P - x
    return x, y, 1, x * y % _P


_BASE_Y = 4 * pow(5, _P - 2, _P) % _P
_BASE = _decode_point(_BASE_Y.to_bytes(32, 'little'))


def ed25519_verify(public_key, message, signature):
    """Return normally when `signature` is a valid Ed25519 signature of `message`; raise otherwise."""
    if len(public_key) != 32 or len(signature) != 64:
        raise MinisignError('Ed25519 needs a 32-byte public key and a 64-byte signature')
    point_a = _decode_point(bytes(public_key))
    point_r = _decode_point(bytes(signature[:32]))
    s = int.from_bytes(signature[32:], 'little')
    if s >= _L:
        raise MinisignError('Non-canonical Ed25519 signature scalar')
    k = int.from_bytes(hashlib.sha512(bytes(signature[:32]) + bytes(public_key) + bytes(message)).digest(),
                       'little') % _L
    if not _same_point(_multiply(s, _BASE), _add(point_r, _multiply(k, point_a))):
        raise MinisignError('Ed25519 signature does not verify')


# ---------------------------------------------------------------------------------------------
# minisign text formats.

class Signature(NamedTuple):
    algorithm: bytes          # b'ED' (prehashed) or b'Ed' (legacy)
    key_id: bytes             # 8 bytes, as stored in the file
    signature: bytes          # 64-byte Ed25519 signature
    trusted_comment: str
    global_signature: bytes   # 64-byte Ed25519 signature of signature + trusted comment
    untrusted_comment: str


class TrustedKey(NamedTuple):
    id: str                   # 16 uppercase hex, as minisign prints it
    role: str                 # 'release' or 'recovery'
    key_id: bytes             # 8 bytes, as stored in the key and signature files
    public_key: bytes         # 32-byte Ed25519 public key


def key_id_hex(key_id):
    """minisign prints the 8 key-id bytes as a little-endian unsigned 64-bit number in hex."""
    if len(key_id) != 8:
        raise MinisignError('A minisign key id is 8 bytes')
    return bytes(key_id)[::-1].hex().upper()


def _base64(line, size, what):
    try:
        raw = base64.b64decode(line.encode('ascii'), validate=True)
    except (UnicodeError, binascii.Error, ValueError) as exc:
        raise MinisignError(f'Malformed base64 in the {what}') from exc
    if len(raw) != size or base64.b64encode(raw).decode('ascii') != line:
        raise MinisignError(f'The {what} has the wrong length or a non-canonical encoding')
    return raw


def _text(value, limit, what):
    if isinstance(value, (bytes, bytearray)):
        if len(value) > limit:
            raise MinisignError(f'The {what} is larger than {limit} bytes')
        try:
            value = bytes(value).decode('utf-8')
        except UnicodeError as exc:
            raise MinisignError(f'The {what} is not UTF-8 text') from exc
    if not isinstance(value, str):
        raise MinisignError(f'The {what} must be text')
    try:
        size = len(value.encode('utf-8'))
    except UnicodeError as exc:
        raise MinisignError(f'The {what} is not UTF-8 text') from exc
    if size > limit:
        raise MinisignError(f'The {what} is larger than {limit} bytes')
    if '\0' in value:
        raise MinisignError(f'The {what} contains a NUL character')
    return value


def _lines(text, count, what):
    lines = text.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    lines = [line[:-1] if line.endswith('\r') else line for line in lines]
    if len(lines) != count or any('\r' in line for line in lines):
        raise MinisignError(f'The {what} must have exactly {count} lines')
    return lines


def parse_public_key(text_or_line):
    """Parse a `.pub` file's text, or only its second line; return (key_id, public_key) bytes."""
    text = _text(text_or_line, 1024, 'public key').strip()
    if '\n' in text:
        comment, line = _lines(text, 2, 'public key')
        if not comment.startswith(UNTRUSTED_PREFIX):
            raise MinisignError('A minisign public key file starts with an untrusted comment')
    else:
        comment, line = None, text
    raw = _base64(line.strip(), 42, 'public key')
    if raw[:2] != PUBLIC_KEY_ALGORITHM:
        raise MinisignError('Unsupported public key algorithm')
    key_id, public_key = raw[2:10], raw[10:]
    named = re.fullmatch(r'untrusted comment: minisign public key ([0-9A-Fa-f]{16})', comment or '')
    if named and named.group(1).upper() != key_id_hex(key_id):
        raise MinisignError('The public key file names a different key id than the key it holds')
    return key_id, public_key


def parse_signature(text):
    """Parse a `.minisig` file (at most 1 KiB) without verifying it."""
    text = _text(text, MAX_SIGNATURE_BYTES, 'signature')
    untrusted, line, trusted, global_line = _lines(text, 4, 'signature')
    if not untrusted.startswith(UNTRUSTED_PREFIX):
        raise MinisignError('A minisign signature starts with an untrusted comment')
    if not trusted.startswith(TRUSTED_PREFIX):
        raise MinisignError('The third line of a minisign signature is its trusted comment')
    raw = _base64(line, 74, 'signature')
    return Signature(raw[:2], raw[2:10], raw[10:], trusted[len(TRUSTED_PREFIX):],
                     _base64(global_line, 64, 'global signature'), untrusted[len(UNTRUSTED_PREFIX):])


def _trusted(keys):
    result = {}
    for key in keys:
        if isinstance(key, TrustedKey):
            key_id, public_key = key.key_id, key.public_key
        elif isinstance(key, str):
            key_id, public_key = parse_public_key(key)
        else:
            key_id, public_key = key
        key_id, public_key = bytes(key_id), bytes(public_key)
        if len(key_id) != 8 or len(public_key) != 32:
            raise MinisignError('A trusted key has an 8-byte id and a 32-byte public key')
        if result.get(key_id, public_key) != public_key:
            raise MinisignError('Two trusted keys share one key id')
        result[key_id] = public_key
    return result


def verify(data, signature_text, keys, *, max_data_size=MAX_SIGNED_DATA_BYTES):
    """Verify `signature_text` over `data` with one of `keys`; return the trusted comment.

    `keys` holds TrustedKey entries (from load_trusted_keys), (key_id, public_key) byte pairs or
    public key text. Raises MinisignError on any failure: a legacy `Ed` signature, an unknown key
    id, a bad signature or a bad global signature (which covers the trusted comment).
    """
    if not isinstance(data, (bytes, bytearray)) or len(data) > max_data_size:
        raise MinisignError(f'Signed data must be bytes, at most {max_data_size} bytes')
    signature = parse_signature(signature_text)
    if signature.algorithm == LEGACY:
        raise MinisignError('Legacy (non-prehashed) minisign signatures are refused; sign with minisign 0.11 or later')
    if signature.algorithm != PREHASHED:
        raise MinisignError('Unsupported minisign signature algorithm')
    trusted = _trusted(keys)
    public_key = trusted.get(signature.key_id)
    if public_key is None:
        raise MinisignError(f'Signature key id {key_id_hex(signature.key_id)} is not a trusted key')
    try:
        ed25519_verify(public_key, hashlib.blake2b(bytes(data), digest_size=64).digest(), signature.signature)
    except MinisignError as exc:
        raise MinisignError('The signature does not match the signed file') from exc
    try:
        ed25519_verify(public_key, signature.signature + signature.trusted_comment.encode('utf-8'),
                       signature.global_signature)
    except MinisignError as exc:
        raise MinisignError('The global signature does not match the trusted comment') from exc
    return signature.trusted_comment


# ---------------------------------------------------------------------------------------------
# The trusted key list and the trusted comments of release and preview sets.

def load_signing_profile(path):
    """Read and validate `profiles/update-signing.json`; return {'repository', 'keys'}."""
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_PROFILE_BYTES:
        raise MinisignError('The signing key list is missing, linked or oversized')
    try:
        profile = json.loads(path.read_bytes().decode('utf-8'))
    except (OSError, UnicodeError, ValueError) as exc:
        raise MinisignError('The signing key list is not valid JSON') from exc
    if not isinstance(profile, dict) or set(profile) != {'schema_version', 'repository', 'keys'}:
        raise MinisignError('The signing key list has exactly schema_version, repository and keys')
    if type(profile['schema_version']) is not int or profile['schema_version'] != 1:
        raise MinisignError('Unsupported signing key list schema_version')
    if not isinstance(profile['repository'], str) or not REPOSITORY.fullmatch(profile['repository']):
        raise MinisignError('The signing key list names an owner/repository')
    entries = profile['keys']
    if not isinstance(entries, list) or len(entries) > MAX_KEYS:
        raise MinisignError(f'The signing key list holds at most {MAX_KEYS} keys')
    keys = []
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {'id', 'role', 'public_key'}:
            raise MinisignError('Each signing key has exactly id, role and public_key')
        if not isinstance(entry['id'], str) or not re.fullmatch(r'[0-9A-F]{16}', entry['id']):
            raise MinisignError('A signing key id is 16 uppercase hex digits')
        if entry['role'] not in ROLES:
            raise MinisignError('A signing key role is release or recovery')
        if not isinstance(entry['public_key'], str) or '\n' in entry['public_key']:
            raise MinisignError('public_key is the second line of the .pub file')
        key_id, public_key = parse_public_key(entry['public_key'])
        if key_id_hex(key_id) != entry['id']:
            raise MinisignError(f'Signing key {entry["id"]} holds the public key of {key_id_hex(key_id)}')
        keys.append(TrustedKey(entry['id'], entry['role'], key_id, public_key))
    if len({key.id for key in keys}) != len(keys) or len({key.public_key for key in keys}) != len(keys):
        raise MinisignError('Signing key ids and public keys are unique')
    for role in ROLES:
        if sum(key.role == role for key in keys) > MAX_KEYS_PER_ROLE:
            raise MinisignError(f'At most {MAX_KEYS_PER_ROLE} {role} keys')
    return {'repository': profile['repository'], 'keys': tuple(keys)}


def load_trusted_keys(path):
    """The validated keys of `profiles/update-signing.json`, as a tuple of TrustedKey."""
    return load_signing_profile(path)['keys']


def _check_version(kind, version, source_revision):
    if kind == 'release':
        if not isinstance(version, str) or not RELEASE_TAG.fullmatch(version):
            raise MinisignError('A release is signed under a vX.Y.Z tag')
    elif kind == 'preview':
        if not isinstance(version, str) or not PREVIEW_VERSION.fullmatch(version):
            raise MinisignError('A preview set is signed under a preview-N version')
    else:
        raise MinisignError('A signed set is a release or a preview')
    if not isinstance(source_revision, str) or not REVISION.fullmatch(source_revision):
        raise MinisignError('A signed set names its full lowercase source commit')


def trusted_comment(kind, version, source_revision):
    """`mag-recomp release <tag> source <commit>` or `mag-recomp preview <version> source <commit>`."""
    _check_version(kind, version, source_revision)
    return f'mag-recomp {kind} {version} source {source_revision}'


def release_comment(tag, source_revision):
    return trusted_comment('release', tag, source_revision)


def preview_comment(version, source_revision):
    return trusted_comment('preview', version, source_revision)


def parse_trusted_comment(comment):
    """Return (kind, version, source_revision) of a release or preview trusted comment."""
    match = COMMENT.fullmatch(comment) if isinstance(comment, str) else None
    if not match:
        raise MinisignError('The trusted comment does not name a mag-recomp release or preview set')
    kind, version, source_revision = match.groups()
    _check_version(kind, version, source_revision)
    return kind, version, source_revision


def check_trusted_comment(comment, kind, version, source_revision):
    """Refuse a trusted comment that names another kind, version or source commit."""
    if comment != trusted_comment(kind, version, source_revision):
        raise MinisignError(f'The signature is not for the {kind} {version} at {source_revision}')
    return parse_trusted_comment(comment)
