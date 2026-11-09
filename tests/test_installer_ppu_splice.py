"""Synthetic generated functions only; no title files or external SDK required."""
import copy
import hashlib
import json
import subprocess
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
            # Like the real lifter, fail unless the shared decoder and ELF parser sit beside it.
            return ('import pathlib,sys\nh=pathlib.Path(__file__).parent\n'
                    'assert (h/"ppu_disasm.py").read_bytes()==b"decoder fixture" and (h/"elf_parser.py").read_bytes()==b"parser fixture"\n'
                    'p=pathlib.Path(sys.argv[sys.argv.index("--output")+1]); p.mkdir()\n'
                    '(p/"ppu_recomp_000.cpp").write_text('+repr(body+other)+')\n').encode()
        old, new = lifter(before), lifter(after)
        profile = {'format':1, 'elf_sha256':tool.digest(elf), 'function':'func_00001000', 'body_sha256':sha(after.encode()),
                   'decoder_sha256':sha(b'decoder fixture'), 'parser_sha256':sha(b'parser fixture'), 'before':{'revision':'a'*40, 'lifter_sha256':sha(old)},
                   'after':{'revision':'b'*40, 'tag':'mag-recomp/lifter-synthetic', 'lifter_sha256':sha(new)}, 'reason':'synthetic dispatcher correction'}
        def pinned(sdk, revision, path, expected):
            data = (b'decoder fixture' if path.endswith('ppu_disasm.py') else b'parser fixture' if path.endswith('elf_parser.py')
                    else old if revision == 'a'*40 else new)
            if sha(data) != expected: raise ValueError('SDK Git object hash mismatch')
            return data
        args = dict(base=base, output=root/'output', work=root/'work', elf=elf, inventory=inventory,
                    nonlocal_profile=nonlocal_profile, before_sdk=root/'old-sdk', after_sdk=root/'new-sdk', profile=profile)
        return args, pinned, before, after, other

    def test_reproduces_only_named_function_and_records_pins(self):
        with tempfile.TemporaryDirectory() as temp:
            args, pinned, before, after, other = self.fixture(Path(temp))
            with patch.object(tool, 'pinned_blob', side_effect=pinned) as read, \
                    patch.object(tool, 'tag_commit', return_value='b'*40) as tag:
                output = tool.prepare(**args)
            tag.assert_called_once_with(args['after_sdk'], 'mag-recomp/lifter-synthetic')
            self.assertEqual((output/'ppu_recomp_000.cpp').read_text(), after+other)
            self.assertEqual((args['base']/'ppu_recomp_000.cpp').read_text(), before+other)
            record = json.loads((output/'provenance.json').read_text())
            self.assertEqual(record['function_splices'][-1]['changed'], ['func_00001000'])
            self.assertEqual([entry['profile'] for entry in record['installer_splices']], [args['profile']])
            self.assertEqual(sorted(call.args[2] for call in read.call_args_list),
                             ['tools/elf_parser.py', 'tools/ppu_disasm.py', 'tools/ppu_lifter.py', 'tools/ppu_lifter.py'])
            self.assertEqual(sorted(p.name for p in args['work'].iterdir()), ['after.log','before.log','splice.log'])

    def test_chains_a_second_profile_onto_the_first_output_and_keeps_both_records(self):
        """The installer applies the dispatcher fix, then the CTR fix to its output."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            args, pinned, before, after, other = self.fixture(root)
            with patch.object(tool, 'pinned_blob', side_effect=pinned), patch.object(tool, 'tag_commit', return_value='b'*40):
                first = tool.prepare(**args)
            # A second fix changes only the other function; its before lift is the pinned lifter's output.
            fixed_other = other.replace(' = 99;', ' = 100;')
            def lifter(text):
                return ('import pathlib,sys\np=pathlib.Path(sys.argv[sys.argv.index("--output")+1]); p.mkdir()\n'
                        '(p/"ppu_recomp_000.cpp").write_text('+repr(text)+')\n').encode()
            old, new = lifter(before + other), lifter(before + fixed_other)
            second = dict(args['profile'], function='func_00002000', body_sha256=sha(fixed_other.encode()),
                          after={'revision': 'e'*40, 'tag': 'mag-recomp/lifter-synthetic-2', 'lifter_sha256': sha(new)},
                          before=dict(args['profile']['before'], lifter_sha256=sha(old)), reason='synthetic CTR correction')
            def pinned_second(sdk, revision, path, expected):
                if path.endswith('ppu_lifter.py'):
                    data = old if revision == 'a'*40 else new
                    if sha(data) != expected: raise ValueError('SDK Git object hash mismatch')
                    return data
                return pinned(sdk, revision, path, expected)
            chained = dict(args, base=first, output=root/'output-2', work=root/'work-2', profile=second)
            with patch.object(tool, 'pinned_blob', side_effect=pinned_second), \
                    patch.object(tool, 'tag_commit', return_value='e'*40):
                output = tool.prepare(**chained)
            self.assertEqual((output/'ppu_recomp_000.cpp').read_text(), after + fixed_other)
            self.assertEqual((first/'ppu_recomp_000.cpp').read_text(), after + other)
            record = json.loads((output/'provenance.json').read_text())
            self.assertEqual([s['changed'] for s in record['function_splices']], [['func_00001000'], ['func_00002000']])
            self.assertEqual([e['profile'] for e in record['installer_splices']], [args['profile'], second])

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
        for what in ('lifter', 'body'):
            with self.subTest(what=what), tempfile.TemporaryDirectory() as temp:
                args, pinned, *_ = self.fixture(Path(temp))
                profile = copy.deepcopy(args['profile']); args['profile'] = profile
                if what == 'lifter': profile['after']['lifter_sha256'] = '0'*64
                else: profile['body_sha256'] = '0'*64
                with patch.object(tool, 'pinned_blob', side_effect=pinned), patch.object(tool, 'tag_commit', return_value='b'*40), \
                        self.assertRaisesRegex(ValueError, 'hash mismatch|reviewed body'):
                    tool.prepare(**args)

    def test_rejects_an_after_tag_that_names_another_commit(self):
        with tempfile.TemporaryDirectory() as temp:
            args, pinned, *_ = self.fixture(Path(temp))
            for found in (None, 'c'*40):
                with self.subTest(found=found), patch.object(tool, 'pinned_blob', side_effect=pinned) as read, \
                        patch.object(tool, 'tag_commit', return_value=found), self.assertRaisesRegex(ValueError, 'after lifter tag'):
                    tool.prepare(**args)
                read.assert_not_called()


class CheckLiftersTests(unittest.TestCase):
    """The check-only mode reads real Git objects: a shallow kit-style checkout
    must hold the after commit and its tag, not just the pinned before commit."""
    def git(self, repo, *args):
        return subprocess.check_output(['git', '-C', str(repo), '-c', 'user.name=Fixture',
                                        '-c', 'user.email=fixture@example.invalid', *args], text=True).strip()

    def lifter_repo(self, root):
        repo = root / 'lifter'; (repo / 'tools').mkdir(parents=True)
        subprocess.check_call(['git', 'init', '-q', str(repo)])
        for name, data in (('ppu_disasm.py', b'decoder'), ('elf_parser.py', b'parser'), ('ppu_lifter.py', b'lifter 1')):
            (repo / 'tools' / name).write_bytes(data)
        self.git(repo, 'add', '-A'); self.git(repo, 'commit', '-qm', 'before')
        before = self.git(repo, 'rev-parse', 'HEAD')
        (repo / 'tools/ppu_lifter.py').write_bytes(b'lifter 2')
        self.git(repo, 'commit', '-qam', 'after')
        after = self.git(repo, 'rev-parse', 'HEAD')
        self.git(repo, 'tag', '-a', '-m', 'fix', 'mag-recomp/lifter-synthetic', after)
        self.git(repo, 'checkout', '-q', '--detach', before)
        profile = {'decoder_sha256': sha(b'decoder'), 'parser_sha256': sha(b'parser'),
                   'before': {'revision': before, 'lifter_sha256': sha(b'lifter 1')},
                   'after': {'revision': after, 'tag': 'mag-recomp/lifter-synthetic', 'lifter_sha256': sha(b'lifter 2')}}
        return repo, profile

    def test_a_checkout_with_the_tagged_after_commit_passes(self):
        with tempfile.TemporaryDirectory() as temp:
            repo, profile = self.lifter_repo(Path(temp))
            decoder, parser, lifters = tool.check_lifters(profile, repo, repo)
            self.assertEqual((decoder, parser, lifters['before'], lifters['after']),
                             (b'decoder', b'parser', b'lifter 1', b'lifter 2'))

    def test_a_shallow_pinned_only_checkout_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); repo, profile = self.lifter_repo(root)
            # As the source kit packed it before the after commit was pinned.
            shallow = root / 'shallow'; subprocess.check_call(['git', 'init', '-q', str(shallow)])
            self.git(shallow, '-c', 'protocol.file.allow=always', 'fetch', '-q', '--no-tags', '--depth=1',
                     str(repo), profile['before']['revision'])
            with self.assertRaisesRegex(ValueError, 'after lifter tag'):
                tool.check_lifters(profile, shallow, shallow)
            # With the after tag fetched too (what the kit now packs), it passes.
            ref = 'refs/tags/' + profile['after']['tag']
            self.git(shallow, '-c', 'protocol.file.allow=always', 'fetch', '-q', '--no-tags', '--depth=1',
                     str(repo), '+' + ref + ':' + ref)
            self.assertEqual(tool.check_lifters(profile, shallow, shallow)[2]['after'], b'lifter 2')
