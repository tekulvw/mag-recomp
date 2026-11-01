"""Synthetic generated functions only; no title files or external SDK required."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import installer_ppu_splice as tool


def sha(data):
    return hashlib.sha256(data).hexdigest()


class InstallerPPUSpliceTests(unittest.TestCase):
    def fixture(self, root):
        base = root / 'base'; base.mkdir()
        inventory = root / 'inventory'; inventory.mkdir()
        before = 'void func_00001000(ppu_context* ctx) {\n    ctx->gpr[1] = 1;\n}\n'
        after = before.replace(' = 1;', ' = 2;')
        other = 'void func_00002000(ppu_context* ctx) {\n    ctx->gpr[2] = 99;\n}\n'
        generated = (before + other).encode()
        (base / 'ppu_recomp_000.cpp').write_bytes(generated)
        elf = root / 'engine.elf'; elf.write_bytes(b'synthetic input identity')
        funcs = inventory / 'reviewed-functions.json'; funcs.write_text('[]')
        imports = inventory / 'mage_g.imports.json'; imports.write_text('{}')
        manifest = {'functions_sha256': tool.digest(funcs), 'imports_sha256': tool.digest(imports)}
        (inventory / 'summary.json').write_text(json.dumps({'elf_sha256':tool.digest(elf), 'manifests':manifest, 'code_end':'0x3000'}))
        provenance = {'elf_sha256':tool.digest(elf), **manifest, 'sdk_revision':'a'*40,
                      'outputs':{'ppu_recomp_000.cpp':sha(generated)}}
        (base / 'provenance.json').write_text(json.dumps(provenance))
        config = root / 'control.json'; config.write_text('{}')
        nonlocal_profile = root / 'nonlocal.json'
        nonlocal_profile.write_text(json.dumps({'elf_sha256':tool.digest(elf), 'sdk_revision':'a'*40,
                                               'configuration':config.name, 'configuration_sha256':tool.digest(config)}))
        def lifter(body):
            return ('import pathlib,sys\nfrom elf_parser import PINNED_FIXTURE\n'
                    'assert PINNED_FIXTURE == "synthetic parser"\n'
                    'p=pathlib.Path(sys.argv[sys.argv.index("--output")+1]); p.mkdir()\n'
                    '(p/"ppu_recomp_000.cpp").write_text('+repr(body+other)+')\n').encode()
        old, new = lifter(before), lifter(after)
        parser = b'PINNED_FIXTURE = "synthetic parser"\n'
        profile = {'format':1, 'elf_sha256':tool.digest(elf), 'function':'func_00001000', 'body_sha256':sha(after.encode()),
                   'decoder_sha256':sha(b'decoder fixture'), 'elf_parser_sha256':sha(parser),
                   'before':{'revision':'a'*40, 'lifter_sha256':sha(old)},
                   'after':{'revision':'b'*40, 'lifter_sha256':sha(new)}, 'reason':'synthetic dispatcher correction'}
        def pinned(sdk, revision, path, expected):
            data = (parser if path.endswith('elf_parser.py') else b'decoder fixture'
                    if path.endswith('ppu_disasm.py') else old if revision == 'a'*40 else new)
            if sha(data) != expected: raise ValueError('SDK Git object hash mismatch')
            return data
        args = dict(base=base, output=root/'output', work=root/'work', elf=elf, inventory=inventory,
                    nonlocal_profile=nonlocal_profile, before_sdk=root/'old-sdk', after_sdk=root/'new-sdk', profile=profile)
        return args, pinned, before, after, other

    def test_reproduces_only_named_function_and_records_pins(self):
        with tempfile.TemporaryDirectory() as temp:
            args, pinned, before, after, other = self.fixture(Path(temp))
            with patch.object(tool, 'pinned_blob', side_effect=pinned) as read:
                output = tool.prepare(**args)
            self.assertEqual((output/'ppu_recomp_000.cpp').read_text(), after+other)
            self.assertEqual((args['base']/'ppu_recomp_000.cpp').read_text(), before+other)
            record = json.loads((output/'provenance.json').read_text())
            self.assertEqual(record['function_splices'][-1]['changed'], ['func_00001000'])
            self.assertEqual(record['installer_dispatcher_splice']['profile'], args['profile'])
            self.assertEqual(len(read.call_args_list), 4)
            self.assertEqual(sorted(p.name for p in args['work'].iterdir()), ['after.log','before.log','splice.log'])

    def test_rejects_wrong_game_and_inventory_before_tool_materialization(self):
        for what in ('elf', 'inventory'):
            with self.subTest(what=what), tempfile.TemporaryDirectory() as temp:
                args, pinned, *_ = self.fixture(Path(temp))
                target = args['elf'] if what == 'elf' else args['inventory']/'reviewed-functions.json'
                target.write_bytes(b'changed')
                with patch.object(tool, 'pinned_blob') as read, self.assertRaisesRegex(ValueError, 'identity mismatch'):
                    tool.prepare(**args)
                read.assert_not_called()
                self.assertFalse(args['work'].exists())

    def test_rejects_unreviewed_lifter_and_unexpected_generated_body(self):
        for what in ('lifter', 'parser', 'body'):
            with self.subTest(what=what), tempfile.TemporaryDirectory() as temp:
                args, pinned, *_ = self.fixture(Path(temp))
                profile = copy.deepcopy(args['profile']); args['profile'] = profile
                if what == 'lifter': profile['after']['lifter_sha256'] = '0'*64
                elif what == 'parser': profile['elf_parser_sha256'] = '0'*64
                else: profile['body_sha256'] = '0'*64
                with patch.object(tool, 'pinned_blob', side_effect=pinned), self.assertRaisesRegex(ValueError, 'hash mismatch|reviewed body'):
                    tool.prepare(**args)
