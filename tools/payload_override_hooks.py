"""Opt-in payload-override seam in the pinned WWS policy lift.

Wraps the policy's one payload call (call 0x325C, continuation 0x3260) in a guard that first asks
`mag_payload_override` (src/mag_extension.h). With no override registered the guard is one
branch and the lifted job runs exactly as before; a registered override that handles the payload
skips the call. Nothing else in the policy changes.

The step is never part of a default build: a policy lift without it is byte-identical to before
and its receipt is unchanged. When applied, the lift directory's provenance.json records it under
`payload_override_hooks`. Run it after tools/graphics_payload_hooks.py when both are used."""
import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import sha256_file, verify_hash
from tools.graphics_payload_hooks import POLICY_SHA, FUNCTION_SHA, BEFORE as TRACE_BEFORE, AFTER as TRACE_AFTER

CALL = 'spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260);'
GUARD_OPEN = 'if (!mag_payload_override(ctx, ctx->pc & 0x3FFFFu)) { '
GUARD_CLOSE = ' }'
DECLARATION = 'extern int mag_payload_override(void* spu_ctx, unsigned int target);\n'
FUNCTION = r'void mag_wws_policy_spu_func_00003190\(spu_context\* ctx\) \{.*?^}'


def original_body(body):
    """The pinned original function text: without this guard and without the trace hooks."""
    return (body.replace(GUARD_OPEN + CALL + GUARD_CLOSE, CALL, 1)
                .replace(TRACE_BEFORE, '', 1).replace(TRACE_AFTER, '', 1))


def instrument_source(source, pin=None):
    m = re.search(FUNCTION, source, re.M | re.S)
    if not m:
        raise ValueError('Missing original policy payload function')
    body = m[0]
    digest = hashlib.sha256(original_body(body).encode()).hexdigest()
    if digest != (FUNCTION_SHA if pin is None else pin):
        raise ValueError('Original policy function mismatch')
    if GUARD_OPEN not in body:
        if body.count(CALL) != 1:
            raise ValueError('Original call/continuation mismatch')
        body = body.replace(CALL, GUARD_OPEN + CALL + GUARD_CLOSE, 1)
        source = source[:m.start()] + body + source[m.end():]
    return source if DECLARATION in source else DECLARATION + source


def apply_hooks(directory):
    directory = Path(directory)
    record = json.loads((directory / 'provenance.json').read_text())
    if record.get('policy_sha256') != POLICY_SHA or record.get('status') != 'complete':
        raise ValueError('Policy identity mismatch')
    for name, digest in record['outputs'].items():
        verify_hash(directory / name, digest)
    path = directory / 'mag_wws_policy/spu_recomp.c'
    old = path.read_text()
    new = instrument_source(old)
    if new == old and record.get('payload_override_hooks'):
        return record
    record['payload_override_hooks'] = {
        'policy_sha256': POLICY_SHA, 'original_function_sha256': FUNCTION_SHA,
        'tool_sha256': sha256_file(Path(__file__)), 'original_source_sha256': sha256_file(path),
        'call_pc': '0x325C', 'continuation': '0x3260',
        'scope': 'Ask registered payload overrides before the original payload call; '
                 'unhandled payloads run the original call and drain unchanged'}
    temporary = path.with_suffix('.override-tmp')
    temporary.write_text(new)
    os.replace(temporary, path)
    record['outputs']['mag_wws_policy/spu_recomp.c'] = sha256_file(path)
    temporary = directory / 'provenance.override-tmp.json'
    temporary.write_text(json.dumps(record, indent=2) + '\n')
    os.replace(temporary, directory / 'provenance.json')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--directory', type=Path, required=True,
                        help='engine SPU lift directory holding provenance.json and mag_wws_policy/')
    args = parser.parse_args()
    apply_hooks(args.directory.resolve())
    print(f'Payload override seam present in {args.directory / "mag_wws_policy/spu_recomp.c"}')
