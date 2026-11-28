import hashlib
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

from tools import prepare_v212_install as prep


def identity(path, data):
    return {'path': path, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def archive(requires, provides, version=0x10003, compressed=False):
    # Synthetic PSARC with one named member, optionally zlib-compressed.
    names = b'/patch.xml\n'
    xml = f'<patch><requires>{requires}</requires><provides>{provides}</provides></patch>'.encode()
    toc = 32 + 60 + 4
    def entry(first, data, offset):
        return bytes(16) + struct.pack('>I', first) + len(data).to_bytes(5, 'big') + offset.to_bytes(5, 'big')
    stored_xml = zlib.compress(xml) if compressed else xml
    return (struct.pack('>4sI4sIIIII', b'PSAR', version, b'zlib', toc, 30, 2, 65536, 0)
            + entry(0, names, toc) + entry(1, xml, toc + len(names))
            + struct.pack('>HH', len(names), len(stored_xml)) + names + stored_xml)


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.disc = self.root / 'disc-input'
        self.update = self.root / 'update-input'
        self.patches = self.root / 'patch-input'
        self.output = self.root / 'result'
        self.profile = {
            'schema': 1, 'title_id': 'BCUS98110', 'app_version': '02.12', 'content_level': 20,
            'disc_files': [identity('PS3_DISC.SFB', b'disc'),
                           identity('PS3_GAME/USRDIR/settings.sdat', b'settings'),
                           identity('PS3_GAME/USRDIR/map.sdat', b'map')],
            'settings_source': 'PS3_GAME/USRDIR/settings.sdat',
            'update_files': [identity('PARAM.SFO', b'update'), identity('USRDIR/EBOOT.BIN', b'new exe')],
            'update_pkg': identity('update.pkg', b'package'),
            'patches': [],
        }
        for base, rows in [(self.disc, self.profile['disc_files']), (self.update, self.profile['update_files'])]:
            for row in rows:
                values = {'PS3_DISC.SFB': b'disc', 'PS3_GAME/USRDIR/settings.sdat': b'settings',
                          'PS3_GAME/USRDIR/map.sdat': b'map', 'PARAM.SFO': b'update',
                          'USRDIR/EBOOT.BIN': b'new exe'}
                p = base / row['path']; p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(values[row['path']])
        self.patches.mkdir()
        for i, (requires, provides) in enumerate([(0, 10), (10, 20)]):
            name = f'patch{i}.psarc'; data = archive(requires, provides, 0x10000 if i == 0 else 0x10003)
            row = identity(name, data); row['name'] = row.pop('path')
            row.update(requires=requires, provides=provides, member_count=1)
            self.profile['patches'].append(row); (self.patches / name).write_bytes(data)

    def assemble(self, **kwargs):
        return prep.assemble(self.disc, self.patches, self.output, self.profile,
                             update_tree=self.update, **kwargs)

    def test_assemble_verify_and_source_independence(self):
        # Unlisted prepared executables must never leak into the output.
        (self.disc / 'PS3_GAME/USRDIR/EBOOT.elf').write_bytes(b'stale')
        result = self.assemble()
        self.assertFalse(result['runtime_verified'])
        self.assertEqual(prep.verify(self.output, self.profile)['content_level'], 20)
        installed = self.output / 'dev_hdd0/game/BCUS98110'
        self.assertEqual((installed / 'USRDIR/game.psarc.sdat').read_bytes(), b'settings')
        self.assertFalse((self.output / 'disc/PS3_GAME/USRDIR/EBOOT.elf').exists())
        target = self.output / 'disc/PS3_GAME/USRDIR/map.sdat'
        self.assertNotEqual(target.stat().st_ino, (self.disc / 'PS3_GAME/USRDIR/map.sdat').stat().st_ino)
        target.write_bytes(b'changed')
        self.assertEqual((self.disc / 'PS3_GAME/USRDIR/map.sdat').read_bytes(), b'map')
        with self.assertRaises(ValueError): prep.verify(self.output, self.profile)

    def test_missing_disc_or_wrong_edition_patch_rejected(self):
        for path in [self.disc / 'PS3_GAME/USRDIR/map.sdat', self.patches / 'patch0.psarc']:
            with self.subTest(path=path):
                data = path.read_bytes(); path.write_bytes(b'other edition')
                with self.assertRaises(ValueError): self.assemble()
                self.assertFalse(self.output.exists()); path.write_bytes(data)
        (self.disc / 'PS3_GAME/USRDIR/map.sdat').unlink()
        with self.assertRaises(ValueError): self.assemble()

    def test_embedded_dependency_chain_rejected_even_with_matching_hash(self):
        data = archive(11, 20); path = self.patches / 'patch1.psarc'; path.write_bytes(data)
        self.profile['patches'][1].update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
        with self.assertRaisesRegex(ValueError, 'dependenc'): self.assemble()

    def test_existing_output_preserved(self):
        self.output.mkdir(); marker = self.output / 'mine'; marker.write_text('keep')
        with self.assertRaises(ValueError): self.assemble()
        self.assertEqual(marker.read_text(), 'keep')

    def test_overlap_rejected(self):
        self.output = self.disc / 'nested-output'
        with self.assertRaisesRegex(ValueError, 'overlap'): self.assemble()

    def test_symlink_file_and_directory_rejected(self):
        path = self.disc / 'PS3_GAME/USRDIR/map.sdat'; data = path.read_bytes(); path.unlink()
        real = self.root / 'map'; real.write_bytes(data); path.symlink_to(real)
        with self.assertRaisesRegex(ValueError, 'symlink'): self.assemble()
        path.unlink(); path.write_bytes(data)
        folder = self.disc / 'PS3_GAME/USRDIR'; folder.rename(self.root / 'outside')
        folder.symlink_to(self.root / 'outside', target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'): self.assemble()

    def test_failed_copy_cleans_owned_staging(self):
        with patch.object(prep, 'copy_file', side_effect=OSError('disk full')):
            with self.assertRaises(OSError): self.assemble()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob('.result.staging-*')), [])

    def test_verify_rejects_unexpected_files_and_receipt_tampering(self):
        self.assemble(); extra = self.output / 'disc/extra'; extra.write_text('extra')
        with self.assertRaisesRegex(ValueError, 'unexpected'): prep.verify(self.output, self.profile)
        extra.unlink()
        receipt = self.output / 'installation-receipt.json'
        data = json.loads(receipt.read_text()); data['runtime_verified'] = True
        receipt.write_text(json.dumps(data))
        with self.assertRaises(ValueError): prep.verify(self.output, self.profile)

    def test_profile_traversal_rejected(self):
        self.profile['disc_files'][0]['path'] = '../escape'
        with self.assertRaises(ValueError): self.assemble()

    def test_missing_update_file_rejected(self):
        (self.update / 'USRDIR/EBOOT.BIN').unlink()
        with self.assertRaises(ValueError): self.assemble()

    def test_compressed_patch_xml_and_truncated_archive(self):
        p = self.root / 'compressed.psarc'; p.write_bytes(archive(10, 20, compressed=True))
        self.assertEqual(prep.patch_dependency(p), (10, 20, 1))
        p.write_bytes(p.read_bytes()[:-10])
        with self.assertRaises(ValueError): prep.patch_dependency(p)

    def synthetic_extractor(self, fail=False):
        pkg = self.root / 'update.pkg'; pkg.write_bytes(b'package')
        extractor = self.root / 'extract.py'
        extractor.write_text("import pathlib,sys\np=pathlib.Path(sys.argv[2])\n"
                             "(p/'USRDIR').mkdir(parents=True)\n"
                             "(p/'PARAM.SFO').write_bytes(b'update')\n"
                             "(p/'USRDIR/EBOOT.BIN').write_bytes(b'new exe')\n"
                             + ('raise SystemExit(9)\n' if fail else ''))
        self.profile['pkg_extractor_sha256'] = prep.digest(extractor)
        return pkg, extractor

    def test_raw_pkg_assembly_with_synthetic_external_extractor(self):
        pkg, extractor = self.synthetic_extractor()
        result = prep.assemble(self.disc, self.patches, self.output, self.profile,
                               update_pkg=pkg, pkg_extractor=extractor)
        self.assertEqual(result, prep.verify(self.output, self.profile))

    def test_failed_extractor_cleans_output_and_preserves_package(self):
        pkg, extractor = self.synthetic_extractor(fail=True)
        with self.assertRaisesRegex(ValueError, 'extraction failed'):
            prep.assemble(self.disc, self.patches, self.output, self.profile,
                          update_pkg=pkg, pkg_extractor=extractor)
        self.assertFalse(self.output.exists())
        self.assertEqual(pkg.read_bytes(), b'package')
        self.assertEqual(list(self.root.glob('.result.staging-*')), [])

    def test_changed_extractor_is_not_executed(self):
        pkg, extractor = self.synthetic_extractor(); extractor.write_text('raise Exception()')
        with self.assertRaisesRegex(ValueError, 'extractor differs'):
            prep.assemble(self.disc, self.patches, self.output, self.profile,
                          update_pkg=pkg, pkg_extractor=extractor)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
