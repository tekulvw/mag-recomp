"""Prove and prepare a profile-bound, one-function PPU relift.

Only external original ELF and generated inputs are read. SDK tool bytes come
from immutable Git objects, never the checkout's working files. A fresh output
preserves all formatting and all bytes outside the approved MTCTR assignment.
No native control, build, input overwrite, networking, or embedded game bytes.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
import sys
import tempfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def normalized(text):
    return ''.join(text.split())


def pinned_blob(sdk, revision, path, expected):
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('SDK revision must be a full immutable Git commit')
    commit = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', revision+'^{commit}'], text=True).strip()
    if commit != revision:
        raise ValueError('SDK object is not the pinned commit')
    data = subprocess.check_output(['git', '-C', str(sdk), 'show', revision+':'+path])
    if sha(data) != expected:
        raise ValueError('SDK Git object hash mismatch: '+path)
    return data


def load_tools(sdk, profile):
    """Both pinned revisions must use the identical pinned decoder."""
    tools = profile['tools']
    decoder = None
    lifters = []
    for version in ('before', 'after'):
        pin = tools[version]
        raw = pinned_blob(sdk, pin['revision'], 'tools/ppu_disasm.py', tools['decoder_sha256'])
        if decoder is not None and raw != decoder:
            raise ValueError('Decoder differs between revisions')
        decoder = raw
        lifters.append(pinned_blob(sdk, pin['revision'], 'tools/ppu_lifter.py', pin['lifter_sha256']))
    modules = []
    saved = sys.modules.get('ppu_disasm')
    old_path = sys.path[:]
    try:
        with tempfile.TemporaryDirectory(prefix='mag-partial-relift-') as temp:
            root = Path(temp)
            (root/'ppu_disasm.py').write_bytes(decoder)
            sys.path.insert(0, str(root))
            sys.modules.pop('ppu_disasm', None)
            for i, data in enumerate(lifters):
                path = root/('lifter_'+str(i)+'.py')
                path.write_bytes(data)
                name = '_mag_partial_lifter_'+str(i)
                spec = importlib.util.spec_from_file_location(name, path)
                module = importlib.util.module_from_spec(spec)
                sys.modules[name] = module
                try:
                    spec.loader.exec_module(module)
                finally:
                    sys.modules.pop(name, None)
                modules.append(module)
            decode = sys.modules['ppu_disasm'].disassemble_bytes
    finally:
        sys.path[:] = old_path
        sys.modules.pop('ppu_disasm', None)
        if saved is not None:
            sys.modules['ppu_disasm'] = saved
    return modules[0], modules[1], decode


def function_bytes(elf, start, end):
    if len(elf) < 64 or elf[:7] != b'\x7fELF\x02\x02\x01':
        raise ValueError('Expected big-endian ELF64')
    ph = struct.unpack_from('>Q', elf, 32)[0]
    size, count = struct.unpack_from('>HH', elf, 54)
    if size != 56 or count > 4096 or ph+size*count > len(elf) or not 0 <= start < end or (end-start)%4:
        raise ValueError('Invalid ELF/function bounds')
    candidates = []
    for i in range(count):
        kind, flags, offset, va, pa, filesz, memsz, alignment = struct.unpack_from('>IIQQQQQQ', elf, ph+i*size)
        if kind == 1 and va <= start and end <= va+filesz:
            if filesz > memsz or offset+filesz > len(elf):
                raise ValueError('Invalid ELF segment')
            candidates.append(elf[offset+start-va:offset+end-va])
    if len(candidates) != 1:
        raise ValueError('Function must belong to exactly one file-backed load segment')
    return candidates[0]


def propose(source, elf, profile, old, new, decode, verify_existing=False):
    if profile.get('format') != 1 or profile.get('change') != 'mtctr-full-width':
        raise ValueError('Unsupported partial-relift profile')
    if sha(elf) != profile['elf_sha256']:
        raise ValueError('Changed external ELF')
    f = profile['function']
    start, end = int(f['start'], 0), int(f['end'], 0)
    code = function_bytes(elf, start, end)
    if sha(code) != f['bytes_sha256']:
        raise ValueError('Changed original function bytes')
    ins = decode(code, start)
    if len(ins) != f['instruction_count']:
        raise ValueError('Changed original instruction count')
    mt = [i for i in ins if i.mnemonic == 'mtctr']
    mf = [i for i in ins if i.mnemonic == 'mfctr']
    if [[hex(i.addr), i.operands] for i in mt] != f['mtctr'] or [[hex(i.addr), i.operands] for i in mf] != f['mfctr']:
        raise ValueError('Changed original CTR instruction identities')
    before = '\n'.join(old.PPULifter().lift_function(ins, start, end).body_lines)
    after = '\n'.join(new.PPULifter().lift_function(ins, start, end).body_lines)
    oldline = 'ctx->ctr = (uint32_t)ctx->gpr['+str(f['mtctr_register'])+'];'
    newline = 'ctx->ctr = ctx->gpr['+str(f['mtctr_register'])+'];'
    if before.count(oldline) != 1 or after.count(newline) != 1 or normalized(after.replace(newline, oldline)) != normalized(before):
        raise ValueError('Full new lift changes more than the approved MTCTR assignment')
    text = source.decode('utf-8')
    signature = 'void '+f['name']+'(ppu_context* ctx) {'
    if text.count(signature) != 1:
        raise ValueError('Existing function must be unique')
    a = text.index(signature)
    following = text.find('\nvoid ', a+len(signature))
    b = following if following >= 0 else len(text)
    body = text[a:b]
    inner = body[body.index('{')+1:body.rindex('}')]
    input_hash = sha(source)
    already = verify_existing and input_hash == profile['output_sha256']
    expected_body = f['after_body_sha256'] if already else f['before_body_sha256']
    if input_hash != (profile['output_sha256'] if already else profile['input_sha256']) or sha(body.encode()) != expected_body:
        raise ValueError('Changed generated input/full function')
    if normalized(inner) != normalized(after if already else before):
        raise ValueError('Complete generated function differs from full original lift')
    replacement = body if already else body.replace(oldline, newline, 1)
    if normalized(replacement[replacement.index('{')+1:replacement.rindex('}')]) != normalized(after):
        raise ValueError('Proposed full function differs from new lift')
    output = (text[:a]+replacement+text[b:]).encode()
    if sha(output) != profile['output_sha256']:
        raise ValueError('Unexpected whole-output hash')
    if not already and output[:a] != source[:a]:
        raise ValueError('Changed bytes outside function')
    if not already and replacement.replace(newline, oldline, 1) != body:
        raise ValueError('Changed bytes beyond MTCTR assignment')
    return output, dict(profile_format=1, input_sha256=input_hash, output_sha256=sha(output),
                        elf_sha256=sha(elf), function_bytes_sha256=sha(code),
                        original_instruction_count=len(ins), already_applied=already,
                        tools=profile['tools'], full_before_normalized_sha256=sha(normalized(before).encode()),
                        full_after_normalized_sha256=sha(normalized(after).encode()),
                        scope='One complete original function validated; only its MTCTR emission replaced. Other generated functions unchanged.',
                        limitation='Source preparation/verification only. A fresh build and native replay remain necessary.')


def write_fresh(output, data, evidence):
    receipt = output.with_suffix(output.suffix+'.receipt.json')
    if output.exists() or output.is_symlink() or receipt.exists() or receipt.is_symlink():
        raise ValueError('Output and receipt must be fresh')
    created = []
    try:
        for path, payload in ((output, data), (receipt, (json.dumps(evidence, indent=2)+'\n').encode())):
            with path.open('xb') as stream:
                created.append(path)
                stream.write(payload)
    except BaseException:
        for path in created:
            path.unlink()
        raise
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', required=True, type=Path)
    p.add_argument('--profile', required=True, type=Path)
    p.add_argument('--elf', required=True, type=Path)
    p.add_argument('--source', required=True, type=Path)
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('--output', type=Path)
    group.add_argument('--verify-only', action='store_true')
    args = p.parse_args()
    profile_bytes = args.profile.read_bytes()
    profile = json.loads(profile_bytes)
    old, new, decode = load_tools(args.sdk, profile)
    data, evidence = propose(args.source.read_bytes(), args.elf.read_bytes(), profile, old, new, decode, args.verify_only)
    evidence.update(profile_sha256=sha(profile_bytes), utility_sha256=sha(Path(__file__).read_bytes()))
    if args.output:
        if args.output.resolve() in (args.source.resolve(), args.elf.resolve(), args.profile.resolve()):
            raise ValueError('Output must not replace an input')
        evidence['receipt'] = str(write_fresh(args.output, data, evidence))
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
