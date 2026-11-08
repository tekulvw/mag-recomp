"""Checked SPU program sources resolve inside the checked directory (synthetic)."""
from pathlib import Path
import json
import tempfile
import unittest

from tools import spu_programs_lift as lift


def fixture(root: Path, recorded_root: str, names=('spu_aa', 'spu_bb')):
    for name in names:
        (root / name).mkdir()
        (root / name / 'spu_recomp.c').write_text('/* synthetic */\n')
    (root / lift.REGISTRY).write_text('/* synthetic registry */\n')
    listed = [f'{recorded_root}/{n}/spu_recomp.c' for n in names] + [f'{recorded_root}/{lift.REGISTRY}']
    (root / lift.SOURCES).write_text('set(SPU_GENERATED_SOURCES\n' + ''.join(f'  [==[{p}]==]\n' for p in listed) + ')\n')
    programs = [dict(name=n, status='registered') for n in names] + [dict(status='handled')]
    return dict(programs=programs)


class CheckedSourcesTests(unittest.TestCase):
    def test_copied_directory_compiles_its_own_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'copy with space'
            root.mkdir()
            inv = fixture(root, '/elsewhere/original-host/spu-programs')
            sources = lift.checked_sources(root, inv)
            self.assertEqual(sources, [root / 'spu_aa/spu_recomp.c', root / 'spu_bb/spu_recomp.c', root / lift.REGISTRY])
            out = Path(tmp) / 'sources.cmake'
            lift.write_sources(out, sources)
            text = out.read_text()
            self.assertNotIn('/elsewhere/', text)
            self.assertIn(f'[==[{(root / "spu_aa/spu_recomp.c").as_posix()}]==]', text)

    def test_recorded_order_is_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inv = fixture(root, '/x')
            inv['programs'].reverse()
            self.assertEqual(lift.checked_sources(root, inv)[0], root / 'spu_aa/spu_recomp.c')

    def test_recorded_list_must_match_inventory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inv = fixture(root, '/x')
            inv['programs'][0]['name'] = 'spu_cc'
            (root / 'spu_cc').mkdir()
            (root / 'spu_cc/spu_recomp.c').write_text('')
            with self.assertRaisesRegex(ValueError, 'does not list'):
                lift.checked_sources(root, inv)

    def test_missing_source_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inv = fixture(root, '/x')
            (root / 'spu_bb/spu_recomp.c').unlink()
            with self.assertRaisesRegex(ValueError, 'missing'):
                lift.checked_sources(root, inv)

    def test_unexpected_names_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inv = fixture(root, '/x')
            inv['programs'][0]['name'] = '../escape'
            with self.assertRaisesRegex(ValueError, 'unexpected'):
                lift.checked_sources(root, inv)


if __name__ == '__main__':
    unittest.main()
