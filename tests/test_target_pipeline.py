"""Synthetic coverage for the temporary 2.12 inventory/lift separation."""
import contextlib
import json
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import inventory, lift, targets
from tools.prepare_inputs import sha256_file


class TargetPipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.target = 'retail-v2.12'
        self.elf = self.root/'EBOOT.elf'
        self.elf.write_bytes(b'synthetic updated launcher')
        engine = self.root/'mage_g.elf'
        engine.write_bytes(b'synthetic updated engine')
        self.definition = SimpleNamespace(
            name=self.target, title_id='BCUS98110', app_version='02.12',
            profile=f'profiles/{self.target}.json', analysis=f'analysis/{self.target}',
            generated=f'generated/{self.target}', native_supported=False,
            executables={'EBOOT.BIN': ('a'*64, sha256_file(self.elf)),
                         'bin/mage_g.self': ('b'*64, sha256_file(engine))})
        self.legacy = SimpleNamespace(
            name='retail-v1.0', profile='profiles/retail-v1.0.json',
            analysis='analysis/retail-v1.0', generated='generated/retail-v1.0')
        images = {}
        for relative, (source_hash, elf_hash) in self.definition.executables.items():
            name = Path(relative).stem
            images[name] = dict(elf=str(self.root/f'{name}.elf'),
                                source_sha256=source_hash, elf_sha256=elf_hash)
        self.profile = dict(target=self.target, title_id=self.definition.title_id,
                            app_version=self.definition.app_version, images=images)
        (self.root/'profiles').mkdir()
        self.write_profile()
        self.analysis = self.root/self.definition.analysis/'EBOOT'
        self.analysis.mkdir(parents=True)
        self.functions = self.analysis/'reviewed-functions.json'
        self.imports = self.analysis/'EBOOT.imports.json'
        self.functions.write_text('[]')
        self.imports.write_text('[]')
        self.summary = dict(target=self.target, image='EBOOT', elf_sha256=sha256_file(self.elf),
                            sdk_revision=inventory.SDK_REVISION, code_end='0x100',
                            manifests=dict(functions_sha256=sha256_file(self.functions),
                                           imports_sha256=sha256_file(self.imports)))
        self.write_summary()
        self.output = self.root/self.definition.generated/'EBOOT'

    def write_profile(self):
        (self.root/self.definition.profile).write_text(json.dumps(self.profile))

    def write_summary(self):
        (self.analysis/'summary.json').write_text(json.dumps(self.summary))

    @contextlib.contextmanager
    def environment(self):
        def get_target(name='retail-v1.0'):
            return {self.target: self.definition, 'retail-v1.0': self.legacy}[name]
        with contextlib.ExitStack() as stack:
            for module in (inventory, lift, targets):
                stack.enter_context(patch.object(module, 'get_target', side_effect=get_target, create=True))
            stack.enter_context(patch('tools.prepare_inputs.WORKSPACE', self.root))
            stack.enter_context(patch('tools.prepare_inputs.ROOT', self.root))
            stack.enter_context(patch('tools.lift.ROOT', self.root))
            stack.enter_context(patch('tools.inventory.ROOT', self.root))
            stack.enter_context(patch('tools.lift.load_framework', return_value=(
                {'ps3recomp': {'lifter-ppu-v212': {'commit': 'reviewed-ppu'}},
                 'allowed_lifter_revisions': ['reviewed-ppu']}, self.root/'framework.json')))
            yield

    def generate(self, **kwargs):
        return lift.generate_lift(self.elf, self.functions, self.imports,
                                  self.root/'sdk', self.output, target=self.target, **kwargs)

    def fake_generate(self, command, output_dir, **kwargs):
        output_dir.mkdir(parents=True)
        (output_dir/'ppu_recomp_000.cpp').write_text('void f() { /* TODO: unknown */; }\n')
        return dict(command=command, exit_code=0)

    def test_updated_lift_uses_profile_target_and_reviewed_ppu_pin_without_v1_hooks(self):
        with self.environment(), patch('tools.lift.check_sdk') as check, \
             patch('tools.lift.run_generator', side_effect=self.fake_generate), \
             patch('tools.lift.subprocess.run', return_value=subprocess.CompletedProcess([], 0)), \
             patch('tools.lift.instrument_source', side_effect=AssertionError('v1 movie hook')), \
             patch('tools.lift.instrument_graphics', side_effect=AssertionError('v1 graphics hook')):
            result = self.generate()
        check.assert_called_once_with(self.root/'sdk', expected_revision='reviewed-ppu', role='lifter-ppu-v212')
        self.assertEqual(result['lifter_role'], 'lifter-ppu-v212')
        self.assertEqual(result['target'], self.target)
        self.assertEqual(result['sdk_revision'], 'reviewed-ppu')
        self.assertEqual(result['analysis_sdk_revision'], inventory.SDK_REVISION)
        self.assertIn('Census baseline only', result['audit_meaning'])
        self.assertIn('not an assertion of correctness', result['audit_meaning'])
        self.assertNotIn('movie_picture_hooks', result)
        self.assertIn('unsupported lifted code', (self.output/'ppu_recomp_000.cpp').read_text())

    def test_updated_lift_rejects_v1_hooks_and_nonlocal_before_loading_them(self):
        for options in ({'hooks': ['does_not_exist']}, {'nonlocal_profile': self.root/'absent.json'}):
            with self.subTest(options=options), self.environment(), patch('tools.lift.run_generator') as generator:
                with self.assertRaisesRegex(ValueError, '2.12|target|v1'):
                    self.generate(**options)
                generator.assert_not_called()

    def test_updated_engine_uses_only_independently_validated_nonlocal_profile(self):
        self.elf=self.root/'mage_g.elf'
        self.analysis=self.root/self.definition.analysis/'mage_g';self.analysis.mkdir()
        self.functions=self.analysis/'reviewed-functions.json';self.functions.write_text('[]')
        self.imports=self.analysis/'mage_g.imports.json';self.imports.write_text('[]')
        self.summary.update(image='mage_g',elf_sha256=sha256_file(self.elf))
        self.write_summary();self.output=self.root/self.definition.generated/'mage_g'
        selected=self.root/'review.json';selected.write_text('synthetic reviewed profile')
        config=self.root/'config.json';config.write_text('synthetic nonlocal configuration')
        review=dict(target=self.target,image_name='mage_g',sdk_revision='reviewed-ppu')
        calls=[]
        def validate(*args):
            calls.append(args);return config,review
        module=SimpleNamespace(validate_profile=validate)
        with self.environment(), patch('tools.lift.check_sdk'), \
             patch.dict(sys.modules, {'tools.v212_nonlocal_profile':module}), \
             patch('tools.lift.run_generator',side_effect=self.fake_generate), \
             patch('tools.lift.subprocess.run',return_value=subprocess.CompletedProcess([],0)):
            result=self.generate(nonlocal_profile=selected)
        self.assertEqual(calls,[(selected,self.root/'sdk',self.elf,'mage_g')])
        self.assertEqual(result['nonlocal_control_flow'],review)
        self.assertEqual(result['nonlocal_profile_sha256'],sha256_file(selected))
        at=result['command'].index('--nonlocal-control-flow')
        self.assertEqual(result['command'][at+1],str(config))

    def test_wrong_profile_target_version_or_pair_binding_is_rejected(self):
        for field, value in [('target', 'retail-v1.0'), ('target', None),
                             ('app_version', '01.00'), ('title_id', 'FOREIGN')]:
            with self.subTest(field=field, value=value):
                original = self.profile[field]
                self.profile[field] = value
                self.write_profile()
                with self.environment(), patch('tools.lift.check_sdk'), patch('tools.lift.run_generator') as generator:
                    with self.assertRaises(ValueError):
                        self.generate()
                    generator.assert_not_called()
                self.profile[field] = original
        for field in ('source_sha256', 'elf_sha256'):
            original = self.profile['images']['mage_g'][field]
            self.profile['images']['mage_g'][field] = '0'*64
            self.write_profile()
            with self.subTest(pair_field=field), self.environment(), patch('tools.lift.check_sdk'):
                with self.assertRaises(ValueError):
                    self.generate()
            self.profile['images']['mage_g'][field] = original

    def test_wrong_inventory_target_image_or_hash_is_rejected(self):
        for field, value in [('target', 'retail-v1.0'), ('target', None),
                             ('image', 'mage_g'), ('elf_sha256', '0'*64), ('sdk_revision', 'foreign')]:
            original = self.summary[field]
            self.summary[field] = value
            self.write_summary()
            with self.subTest(field=field), self.environment(), patch('tools.lift.check_sdk'), \
                 patch('tools.lift.run_generator') as generator:
                with self.assertRaises(ValueError):
                    self.generate()
                generator.assert_not_called()
            self.summary[field] = original

    def test_changed_manifest_is_rejected(self):
        self.functions.write_text('[{"foreign": true}]')
        with self.environment(), patch('tools.lift.check_sdk'):
            with self.assertRaises(ValueError):
                self.generate()

    def test_updated_lift_rejects_other_target_output_roots_and_symlinks(self):
        for root_name in ('analysis', 'generated'):
            forbidden = self.root/root_name/'retail-v1.0'
            forbidden.mkdir(parents=True)
            alias = self.root/f'alias-{root_name}'
            alias.symlink_to(forbidden, target_is_directory=True)
            for output in (forbidden/'EBOOT', alias/'EBOOT'):
                self.output = output
                with self.subTest(output=output), self.environment(), patch('tools.lift.check_sdk'), \
                     patch('tools.lift.run_generator') as generator:
                    with self.assertRaises(ValueError):
                        self.generate()
                    generator.assert_not_called()
                self.assertFalse(output.exists())

    def test_failed_audit_retains_provenance_and_evidence(self):
        def audit(command, stdout, **kwargs):
            stdout.write('synthetic audit failure\n')
            return subprocess.CompletedProcess(command, 7)
        with self.environment(), patch('tools.lift.check_sdk'), \
             patch('tools.lift.run_generator', side_effect=self.fake_generate), \
             patch('tools.lift.subprocess.run', side_effect=audit):
            with self.assertRaisesRegex(RuntimeError, 'audit failed'):
                self.generate()
        receipt = json.loads((self.output/'provenance.json').read_text())
        self.assertEqual((receipt['status'], receipt['audit_exit'], receipt['target']),
                         ('failed', 7, self.target))
        self.assertIn('synthetic audit failure', (self.output/'audit.log').read_text())

    def test_updated_inventory_rejects_foreign_profile_before_tools_or_writes(self):
        self.profile['images']['mage_g']['elf_sha256'] = '0'*64
        self.write_profile()
        output = self.root/'new-inventory'
        with self.environment(), patch('tools.inventory.check_sdk'), patch('tools.inventory.subprocess.run') as run:
            with self.assertRaises(ValueError):
                inventory.inventory_image(self.elf, self.root/'sdk', output, target=self.target)
            run.assert_not_called()
        self.assertFalse(output.exists())

    def test_updated_inventory_rejects_foreign_output_even_through_symlink(self):
        foreign = self.root/'analysis/retail-v1.0'
        foreign.mkdir(parents=True)
        alias = self.root/'analysis-alias'
        alias.symlink_to(foreign, target_is_directory=True)
        with self.environment(), patch('tools.inventory.check_sdk'), patch('tools.inventory.subprocess.run') as run:
            with self.assertRaises(ValueError):
                inventory.inventory_image(self.elf, self.root/'sdk', alias/'EBOOT', target=self.target)
            run.assert_not_called()
        self.assertFalse((foreign/'EBOOT').exists())

    def test_updated_inventory_requires_fresh_output(self):
        output = self.root/'used-inventory'
        output.mkdir()
        marker = output/'evidence.log'
        marker.write_text('old evidence')
        with self.environment(), patch('tools.inventory.check_sdk'), patch('tools.inventory.subprocess.run') as run:
            with self.assertRaisesRegex(ValueError, 'fresh'):
                inventory.inventory_image(self.elf, self.root/'sdk', output, target=self.target)
            run.assert_not_called()
        self.assertEqual(marker.read_text(), 'old evidence')

    def test_inventory_cli_refuses_summary_symlink_into_other_target(self):
        foreign = self.root/'profiles/retail-v1.0.json'
        foreign.write_text('preserved v1 profile')
        (self.root/'profiles/retail-v2.12-inventory.json').symlink_to(foreign)
        # Permit a fresh inventory, so a refusal must protect the summary path.
        for path in self.analysis.iterdir():
            path.unlink()
        with self.environment(), patch('sys.argv', [str(inventory.__file__), '--sdk', str(self.root/'sdk'), '--target', self.target]), \
             patch('subprocess.check_output', side_effect=[inventory.SDK_REVISION, '']), \
             patch('subprocess.run', side_effect=AssertionError('Tool ran before summary output check')) as run:
            with self.assertRaisesRegex(ValueError, 'overlap'):
                runpy.run_path(str(inventory.__file__), run_name='__main__')
            run.assert_not_called()
        self.assertEqual(foreign.read_text(), 'preserved v1 profile')

    def test_inventory_preserves_target_and_manifest_identity(self):
        output = self.root/'fresh-inventory'
        def tool(command, **kwargs):
            name = Path(command[1]).name
            if name == 'elf_parser.py':
                stdout = json.dumps({'section_headers': [{'addr': '0x100', 'size': '0x20', 'flags': '0x4'}]})
            else:
                stdout = ''
            if name == 'ppu_loader.py':
                (output/'EBOOT.functions.json').write_text('[{"start":"0x100","end":"0x120","toc":"0x200"}]')
                (output/'EBOOT.imports.json').write_text('[]')
            elif name == 'find_functions.py':
                (output/'functions.json').write_text('[{"start":"0x100","end":"0x120","toc":"0x200"}]')
            elif name == 'gen_imports.py':
                (output/'named-imports.json').write_text('[]')
            elif name == 'extract_spu_images.py':
                (output/'spu').mkdir()
            return subprocess.CompletedProcess(command, 0, stdout)
        with self.environment(), patch('tools.inventory.check_sdk'), \
             patch('tools.inventory.validate_ppu_elf', return_value={'synthetic': True}), \
             patch('tools.inventory.subprocess.run', side_effect=tool):
            result = inventory.inventory_image(self.elf, self.root/'sdk', output, target=self.target)
        self.assertEqual((result['target'], result['image']), (self.target, 'EBOOT'))
        self.assertEqual(result['elf_sha256'], sha256_file(self.elf))
        self.assertEqual(result['manifests']['functions_sha256'], sha256_file(output/'reviewed-functions.json'))
        self.assertEqual(json.loads((output/'summary.json').read_text()), json.loads(json.dumps(result)))

    def test_updated_source_guard_does_not_annotate_v1_entry_address(self):
        def generate(command, output_dir):
            output_dir.mkdir(parents=True)
            (output_dir/'ppu_recomp_000.cpp').write_text('void func_00010230(ppu_context* ctx) {}\n')
            return dict(command=command, exit_code=0)
        with self.environment(), patch('tools.lift.check_sdk'), \
             patch('tools.lift.run_generator', side_effect=generate), \
             patch('tools.lift.subprocess.run', return_value=subprocess.CompletedProcess([], 0)):
            result = self.generate()
        self.assertNotIn('retail_entry', (self.output/'ppu_recomp_000.cpp').read_text())
        self.assertNotIn('log verified retail entry', result['postprocessing'])


if __name__ == '__main__':
    unittest.main()
