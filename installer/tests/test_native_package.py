import os
import hashlib
import json
from pathlib import Path
import tempfile
import platform
import subprocess
import shutil
import unittest
from unittest.mock import patch
from mag_installer.model import InstallerError
from mag_installer.native_package import package


@unittest.skipIf(os.name == 'nt', 'macOS .app packaging (otool, install_name_tool, codesign)')
class NativePackageTests(unittest.TestCase):
    def fixture(self, root):
        workspace=root/'workspace';(workspace/'profiles').mkdir(parents=True)
        disc=workspace/'local/retail-v1.0/PS3_GAME/USRDIR';(disc/'bin').mkdir(parents=True)
        images={}
        for folder,image,elf in [('build','EBOOT',disc/'EBOOT.elf'),('build-engine','mage_g',disc/'bin/mage_g.elf')]:
            elf.write_bytes(b'fixture ELF '+image.encode()); binary=workspace/folder/'MagRecomp';binary.parent.mkdir();binary.write_bytes(image.encode())
            digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
            receipt=dict(image_name=image,binary_sha256=digest(binary),elf_sha256=digest(elf),sdk_revision='a'*40)
            binary.with_suffix('.receipt.json').write_text(json.dumps(receipt))
            images[image]=dict(elf=str(elf),elf_sha256=digest(elf),source_sha256='b'*64)
        (workspace/'profiles/retail-v1.0.json').write_text(json.dumps(dict(images=images)))
        (workspace/'profiles/framework.json').write_text(json.dumps(dict(strict_runtime_revisions=['a'*40],strict_hle_runtime_revisions=['a'*40])))
        return workspace

    def tool(self, argv, token):
        if argv[0].endswith('otool') and argv[1]=='-L':return argv[-1]+':\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n'
        if '-o' in argv:Path(argv[argv.index('-o')+1]).write_bytes(b'supervisor fixture')
        return ''

    def test_package_creates_real_app_structure_keeps_assets_local_and_preserves_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);workspace=self.fixture(root);events=[]
            original=(workspace/'build/MagRecomp.receipt.json').read_bytes()
            with patch('mag_installer.native_package._run_tool',side_effect=self.tool):
                app=package({'python':'/usr/bin/python3'},workspace,root/'MAG.app',events=events.append)
            self.assertTrue((app/'Contents/MacOS/MAG').is_file())
            self.assertTrue((app/'Contents/Info.plist').is_file())
            manifest=json.loads((app/'Contents/Resources/runtime.json').read_text())
            self.assertFalse((app/'Contents/Resources/PS3_GAME').exists())
            self.assertEqual((app/'Contents/Resources/source-receipts/launcher.json').read_bytes(),original)
            self.assertEqual((workspace/'build/MagRecomp.receipt.json').read_bytes(),original)
            self.assertEqual(manifest['workspace'],'../workspace')
            self.assertEqual(events[-1]['event'],'app-packaged')

    def test_nested_owned_workspace_is_relative_and_external_workspace_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);install=root/'installation';install.mkdir()
            build=install/'build-work';build.mkdir();workspace=self.fixture(build)
            with patch('mag_installer.native_package._run_tool',side_effect=self.tool):
                app=package({},workspace,install/'MAG.app')
            manifest=json.loads((app/'Contents/Resources/runtime.json').read_text())
            self.assertEqual(manifest['workspace'],'../build-work/workspace')
            external=self.fixture(root/'external')
            with self.assertRaisesRegex(InstallerError,'installation'):
                package({},external,install/'External.app')
            self.assertFalse((install/'External.app').exists())

    @unittest.skipUnless(platform.system() == 'Darwin' and Path('/usr/bin/xcrun').is_file(), 'macOS signing/toolchain fixture')
    def test_real_macos_packaging_signs_synthetic_dylib_closure_without_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);workspace=self.fixture(root)
            source=root/'library.c';source.write_text('int fixture(void) { return 0; }\n')
            library=root/'libfixture.dylib'
            subprocess.run(['/usr/bin/xcrun','clang','-dynamiclib',str(source),'-install_name',str(library),'-o',str(library)],check=True,capture_output=True)
            source=root/'main.c';source.write_text('int fixture(void); int main(void) { return fixture(); }\n')
            binary=root/'fixture'
            subprocess.run(['/usr/bin/xcrun','clang',str(source),str(library),'-o',str(binary)],check=True,capture_output=True)
            for folder in ('build','build-engine'):
                target=workspace/folder/'MagRecomp';shutil.copyfile(binary,target)
                receipt=target.with_suffix('.receipt.json');record=json.loads(receipt.read_text())
                record['binary_sha256']=hashlib.sha256(target.read_bytes()).hexdigest();receipt.write_text(json.dumps(record))
            app=package({},workspace,root/'MAG.app')
            subprocess.run(['/usr/bin/codesign','--verify','--deep','--strict',str(app)],check=True,capture_output=True)
            dependencies=subprocess.check_output(['/usr/bin/otool','-L',str(app/'Contents/MacOS/MagRecomp')],text=True)
            self.assertIn('@executable_path/../Frameworks/libfixture.dylib',dependencies)
            self.assertNotIn(str(library),dependencies)
            self.assertTrue((app/'Contents/Frameworks/libfixture.dylib').is_file())

    def test_existing_destination_and_mismatched_receipts_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);workspace=self.fixture(root);app=root/'MAG.app';app.mkdir()
            with self.assertRaises(InstallerError):package({'python':'/usr/bin/python3'},workspace,app)
            app.rmdir();(workspace/'build/MagRecomp').write_bytes(b'changed')
            with patch('mag_installer.native_package._run_tool',side_effect=self.tool):
                with self.assertRaisesRegex(InstallerError,'receipt'):package({'python':'/usr/bin/python3'},workspace,app)
            self.assertFalse(app.exists())

    def test_dylib_closure_rewrites_then_signs_before_bundled_receipt_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);workspace=self.fixture(root);lib=root/'libfixture.dylib';lib.write_bytes(b'lib');calls=[]
            def run(argv,token):
                calls.append(argv)
                if argv[0].endswith('otool') and argv[1]=='-L':
                    dep='/usr/lib/libSystem.B.dylib' if argv[-1].endswith('.dylib') else str(lib)
                    return argv[-1]+':\n\t'+dep+' (compatibility version 1.0.0, current version 1.0.0)\n'
                if '-o' in argv:Path(argv[argv.index('-o')+1]).write_bytes(b'supervisor fixture')
                if argv[0].endswith('codesign') and Path(argv[-1]).is_file():
                    p=Path(argv[-1]);p.write_bytes(p.read_bytes()+b'-signed')
                return ''
            with patch('mag_installer.native_package._run_tool',side_effect=run):
                app=package({'python':'/usr/bin/python3'},workspace,root/'MAG.app')
            binary=app/'Contents/MacOS/MagRecomp'
            receipt=json.loads((app/'Contents/Resources/package-receipts/launcher.json').read_text())
            self.assertEqual(receipt['binary_sha256'],hashlib.sha256(binary.read_bytes()).hexdigest())
            self.assertTrue(any('-change' in c for c in calls))
            self.assertTrue((app/'Contents/Frameworks/libfixture.dylib').is_file())
