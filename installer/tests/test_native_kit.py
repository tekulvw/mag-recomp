import os
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from mag_installer import native_kit
from mag_installer.native_recipe import ROLES
from mag_installer.model import InstallerError
from mag_installer.safety import Cancellation, Cancelled

@unittest.skipIf(os.name == 'nt', 'kit preparation is a POSIX maintainer step (CI prepares kits on Linux)')
class NativeKitTests(unittest.TestCase):
    def fixture(self,root):
        source=root/'source';source.mkdir()
        for name in ('src','tools','cmake','profiles'):(source/name).mkdir()
        (source/'CMakeLists.txt').write_text('project(sample)\n')
        (source/'src/main.cpp').write_text('int main() {}\n')
        (source/'profiles/account-secret.json').write_text('PRIVATE')
        (source/'local').mkdir();(source/'local/game.bin').write_text('PRIVATE GAME')
        (source/'runs').mkdir();(source/'runs/raw.png').write_text('PRIVATE CAPTURE')
        sdk=root/'sdk';sdk.mkdir()
        self.git('init','-q',str(sdk))
        (sdk/'runtime.txt').write_text('public source\n')
        self.git('-C',str(sdk),'add','runtime.txt')
        self.git('-C',str(sdk),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','fixture')
        revision=self.git('-C',str(sdk),'rev-parse','HEAD').strip()
        return source,{role:{'path':str(sdk),'revision':revision} for role in ROLES}

    def git(self,*args):
        return subprocess.check_output(['/usr/bin/git',*args],text=True,stderr=subprocess.PIPE)

    def test_prepares_only_curated_source_and_shallow_pinned_sdks(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve();source,sdks=self.fixture(root)
            original_index=(root/'sdk/.git/index').read_bytes()
            output=root/'kit.tar.gz'
            with patch('mag_installer.native_kit.validate_source_tree',return_value={r:{'commit':s['revision'],'tag':'fixture'} for r,s in sdks.items()}):
                descriptor=native_kit.prepare(source,sdks,output)
            self.assertEqual(descriptor['sha256'],hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(descriptor['size'],output.stat().st_size)
            with tarfile.open(output) as archive:
                names=archive.getnames()
                self.assertFalse(any('account-secret' in n or 'game.bin' in n or 'raw.png' in n for n in names))
                self.assertIn('source/src/main.cpp',names)
                manifest=json.load(archive.extractfile('KIT.json'))
                self.assertEqual(manifest['jobs'],2)
                self.assertEqual(manifest['source_bytes'],sum((source/item['path']).stat().st_size for item in manifest['source_inventory']))
                for role in sdks:
                    self.assertIn(f'sdks/{role}/.git/shallow',names)
                    self.assertNotIn(f'sdks/{role}/.git/FETCH_HEAD',names)
                extracted=root/'extracted';extracted.mkdir()
                archive.extractall(extracted,filter='data')
                for role,record in sdks.items():
                    actual=self.git('-C',str(extracted/'sdks'/role),'rev-parse','HEAD').strip()
                    self.assertEqual(actual,record['revision'])
            self.assertEqual((root/'sdk/.git/index').read_bytes(),original_index)

    def test_packs_the_tagged_splice_commit_beside_the_pinned_lifter(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve();source,sdks=self.fixture(root)
            sdk=root/'sdk';identity=['-c','user.name=Fixture','-c','user.email=fixture@example.invalid']
            (sdk/'runtime.txt').write_text('fixed\n')
            self.git('-C',str(sdk),*identity,'commit','-qam','fix')
            fixed=self.git('-C',str(sdk),'rev-parse','HEAD').strip()
            self.git('-C',str(sdk),*identity,'tag','-a','-m','fix','mag-recomp/lifter-fixture',fixed)
            pins={r:{'commit':s['revision'],'tag':'fixture'} for r,s in sdks.items()}
            pins['lifter-ppu']['splice_after']={'tag':'mag-recomp/lifter-fixture','commit':fixed}
            with patch('mag_installer.native_kit.validate_source_tree',return_value=pins):
                native_kit.prepare(source,sdks,root/'kit.tar.gz')
            extracted=root/'extracted';extracted.mkdir()
            with tarfile.open(root/'kit.tar.gz') as archive:
                archive.extractall(extracted,filter='data')
            lifter=extracted/'sdks/lifter-ppu'
            self.assertEqual(self.git('-C',str(lifter),'rev-parse','HEAD').strip(),sdks['lifter-ppu']['revision'])
            self.assertEqual(self.git('-C',str(lifter),'rev-parse','refs/tags/mag-recomp/lifter-fixture^{commit}').strip(),fixed)
            self.assertEqual(self.git('-C',str(lifter),'show',fixed+':runtime.txt'),'fixed\n')
            # Roles without a splice commit stay single-commit.
            with self.assertRaises(subprocess.CalledProcessError):
                self.git('-C',str(extracted/'sdks/runtime'),'rev-parse','--verify','refs/tags/mag-recomp/lifter-fixture')
            # A list packs every tagged commit.
            (sdk/'runtime.txt').write_text('second fix\n')
            self.git('-C',str(sdk),*identity,'commit','-qam','second fix')
            second=self.git('-C',str(sdk),'rev-parse','HEAD').strip()
            self.git('-C',str(sdk),*identity,'tag','-a','-m','second fix','mag-recomp/lifter-fixture-2',second)
            pins['lifter-ppu']['splice_after']=[{'tag':'mag-recomp/lifter-fixture','commit':fixed},
                                                {'tag':'mag-recomp/lifter-fixture-2','commit':second}]
            with patch('mag_installer.native_kit.validate_source_tree',return_value=pins):
                native_kit.prepare(source,sdks,root/'kit-list.tar.gz')
            listed=root/'extracted-list';listed.mkdir()
            with tarfile.open(root/'kit-list.tar.gz') as archive:
                archive.extractall(listed,filter='data')
            for tag,commit,text in (('mag-recomp/lifter-fixture',fixed,'fixed\n'),('mag-recomp/lifter-fixture-2',second,'second fix\n')):
                self.assertEqual(self.git('-C',str(listed/'sdks/lifter-ppu'),'rev-parse','refs/tags/'+tag+'^{commit}').strip(),commit)
                self.assertEqual(self.git('-C',str(listed/'sdks/lifter-ppu'),'show',commit+':runtime.txt'),text)
            # A tag that names another commit is refused.
            pins['lifter-ppu']['splice_after']={'tag':'mag-recomp/lifter-fixture','commit':fixed}
            pins['lifter-ppu']['splice_after']['commit']=sdks['lifter-ppu']['revision']
            with patch('mag_installer.native_kit.validate_source_tree',return_value=pins):
                with self.assertRaisesRegex(InstallerError,'does not name its pinned splice commit'):
                    native_kit.prepare(source,sdks,root/'refused.tar.gz')

    def test_git_checkout_source_refuses_untracked_files_in_allowlisted_directories(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve();source,sdks=self.fixture(root)
            identity=['-c','user.name=Fixture','-c','user.email=fixture@example.invalid']
            self.git('init','-q',str(source))
            self.git('-C',str(source),'add','CMakeLists.txt','src/main.cpp')
            self.git('-C',str(source),*identity,'commit','-qm','fixture')
            (source/'src/scratch-capture.txt').write_text('UNTRACKED')
            pins={r:{'commit':s['revision'],'tag':'fixture'} for r,s in sdks.items()}
            with patch('mag_installer.native_kit.validate_source_tree',return_value=pins):
                with self.assertRaisesRegex(InstallerError,'untracked.*src/scratch-capture.txt'):
                    native_kit.prepare(source,sdks,root/'refused.tar.gz')
                self.assertFalse((root/'refused.tar.gz').exists())
                self.git('-C',str(source),'add','src/scratch-capture.txt')
                self.git('-C',str(source),*identity,'commit','-qm','tracked')
                native_kit.prepare(source,sdks,root/'kit.tar.gz')
                self.assertTrue((root/'kit.tar.gz').is_file())

    def test_invalid_source_preflight_and_existing_output_fail_before_publish(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve();source,sdks=self.fixture(root);output=root/'kit.tar.gz'
            with self.assertRaises(InstallerError):native_kit.prepare(source,sdks,output)
            self.assertFalse(output.exists())
            output.write_bytes(b'KEEP')
            with self.assertRaises(InstallerError):native_kit.prepare(source,sdks,output)
            self.assertEqual(output.read_bytes(),b'KEEP')

    def test_cancelled_prepare_never_creates_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve();source,sdks=self.fixture(root);output=root/'kit.tar.gz'
            token=Cancellation();token.cancel()
            with self.assertRaises(Cancelled):native_kit.prepare(source,sdks,output,token=token)
            self.assertFalse(output.exists())
