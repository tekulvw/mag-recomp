import dataclasses
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
from unittest.mock import patch

from tools import prepare_inputs
from tools import targets
from tests.test_inputs import fixture


class TargetTests(unittest.TestCase):
    def test_default_preserves_v1_paths_and_identities(self):
        target = targets.get_target()
        self.assertEqual(target.name, 'retail-v1.0')
        self.assertEqual(target.app_version, '01.00')
        self.assertEqual(target.profile, 'profiles/retail-v1.0.json')
        self.assertEqual(target.local_usrdir, 'local/retail-v1.0/PS3_GAME/USRDIR')
        self.assertEqual(dict(target.executables), prepare_inputs.RETAIL)
        self.assertTrue(target.native_supported)

    def test_v212_paths_and_identities_are_separate_and_immutable(self):
        old = targets.get_target(); new = targets.get_target('retail-v2.12')
        self.assertEqual(new.app_version, '02.12')
        self.assertFalse(new.native_supported)
        for field in ('profile', 'analysis', 'generated', 'local_usrdir'):
            self.assertNotEqual(getattr(old, field), getattr(new, field))
        self.assertEqual(new.executables['bin/mage_g.self'][1],
                         '705a6ec6a25458b2401b869e75e2904597304e2c06d9534a171a1dcd54aca2b3')
        with self.assertRaises(dataclasses.FrozenInstanceError): new.name = 'other'
        with self.assertRaises(TypeError): new.executables['extra'] = ('x', 'y')
        with self.assertRaises(ValueError): targets.get_target('../retail-v1.0')

    def profile(self):
        target = targets.get_target('retail-v2.12')
        return {'target': target.name, 'title_id': target.title_id, 'app_version': target.app_version,
                'images': {Path(name).stem if name != 'EBOOT.BIN' else 'EBOOT':
                           {'source_sha256': hashes[0], 'elf_sha256': hashes[1]}
                           for name, hashes in target.executables.items()}}

    def test_v212_profile_requires_exact_complete_pair_and_target(self):
        original = self.profile(); targets.validate_target_profile(original, 'retail-v2.12')
        mutations = [lambda p: p.pop('target'), lambda p: p.update(app_version='01.00'),
                     lambda p: p['images'].pop('mage_g'),
                     lambda p: p['images']['EBOOT'].update(elf_sha256=targets.get_target().executables['EBOOT.BIN'][1])]
        for mutate in mutations:
            p = json.loads(json.dumps(original)); mutate(p)
            with self.subTest(profile=p), self.assertRaises(ValueError):
                targets.validate_target_profile(p, 'retail-v2.12')
        with self.assertRaises(ValueError): targets.validate_target_profile(original)

    def test_output_cannot_overlap_other_target_or_follow_symlink_there(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('analysis', 'generated', 'local', 'build', 'runs'):
                with self.subTest(kind=name), self.assertRaises(ValueError):
                    targets.validate_target_output(root / name / 'retail-v1.0/nested', 'retail-v2.12', root)
            with self.assertRaises(ValueError):
                targets.validate_target_output(root / 'profiles/retail-v1.0.json', 'retail-v2.12', root)
            targets.validate_target_output(root / 'analysis/retail-v2.12', 'retail-v2.12', root)
            targets.validate_target_output(root / 'custom-audit', 'retail-v2.12', root)
            (root / 'generated/retail-v1.0').mkdir(parents=True)
            (root / 'alias').symlink_to(root / 'generated/retail-v1.0', target_is_directory=True)
            with self.assertRaises(ValueError):
                targets.validate_target_output(root / 'alias/new', 'retail-v2.12', root)

    def test_preparation_and_profile_write_use_selected_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); src = root / 'source'; local = root / 'prepared'
            definitions = {}
            for name in ('EBOOT.BIN', 'bin/mage_g.self'):
                source = (name + ' synthetic SELF').encode(); elf = bytes(fixture())
                definitions[name] = (hashlib.sha256(source).hexdigest(), hashlib.sha256(elf).hexdigest())
                for base in (src, local):
                    p = base / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(source)
                (local / name).with_suffix('.elf').write_bytes(elf)
            selected = dataclasses.replace(targets.get_target('retail-v2.12'), executables=definitions)
            original_get = targets.get_target
            with patch('tools.prepare_inputs.get_target', return_value=selected), \
                 patch('tools.targets.get_target', side_effect=lambda name=targets.DEFAULT_TARGET:
                       selected if name == selected.name else original_get(name)):
                result = prepare_inputs.prepare_inputs(src, local, target=selected.name)
                self.assertEqual((result['target'], result['app_version']), (selected.name, '02.12'))
                with patch('tools.prepare_inputs.WORKSPACE', root):
                    path = prepare_inputs.write_image_profile(result, target=selected.name)
                self.assertEqual(path, root / 'profiles/retail-v2.12.json')
                self.assertFalse((root / 'profiles/retail-v1.0.json').exists())
                (local / 'bin/mage_g.self').write_bytes(b'wrong version')
                with self.assertRaises(ValueError):
                    prepare_inputs.prepare_inputs(src, local, target=selected.name)

    def test_writer_rejects_foreign_profile_before_touching_default(self):
        with tempfile.TemporaryDirectory() as tmp, patch('tools.prepare_inputs.WORKSPACE', Path(tmp)):
            with self.assertRaises(ValueError):
                prepare_inputs.write_image_profile({'title_id': 'BCUS98110', 'app_version': '02.12'})
            self.assertFalse((Path(tmp) / 'profiles').exists())

    def test_new_target_outputs_cannot_enter_public_or_git_metadata_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / '.gitignore').write_text('analysis/\ngenerated/\nprofiles/retail-v2.12.json\n')
            for path in (root / 'public-output', root / '.git/private-lift'):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    targets.validate_target_output(path, 'retail-v2.12', root)
                self.assertFalse(path.exists())
            targets.validate_target_output(root / 'generated/retail-v2.12', 'retail-v2.12', root)
            targets.validate_target_output(root / 'profiles/retail-v2.12.json', 'retail-v2.12', root)


if __name__ == '__main__':
    unittest.main()
