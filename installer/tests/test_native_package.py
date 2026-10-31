import os
import hashlib
import json
from pathlib import Path
import tempfile
import platform
import subprocess
import shutil
import struct
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

    @unittest.skipUnless(platform.system() == 'Darwin', 'native supervisor fixture')
    def test_supervisor_carries_settings_through_handoff_and_cleans_owned_overlay(self):
        """Two tiny native fixtures stand in for both images; no guest runs."""
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);workspace=self.fixture(root)
            source=root/'fixture.c'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv) {
 if(argc!=2)return 60;
 int launcher=strstr(argv[1],"EBOOT.elf")!=NULL;
 const char *path=getenv("MAG_PLAYER_SETTINGS_PATH"),*overlay=getenv("MAG_PLAYER_OVERLAY_ROOT");
 if(!path||!overlay)return 61;
 FILE *f=fopen(launcher?"launcher-env.json":"engine-env.json","w");
 if(!f)return 62;
 fprintf(f,"{\"settings_path\":\"%s\",\"overlay_root\":\"%s\",\"setup\":%s,\"show\":%s}",
  path,overlay,getenv("MAG_PLAYER_SETUP")?"true":"false",getenv("MAG_PLAYER_SHOW_SETTINGS")?"true":"false");fclose(f);
 if(!launcher)return 17;
 f=fopen(getenv("MAG_HANDOFF_REQUEST"),"w");if(!f)return 63;
 fprintf(f,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}",getenv("MAG_HANDOFF_TOKEN"));fclose(f);
 return 90;
}
''')
            binary=root/'fixture'
            subprocess.run(['xcrun','clang',str(source),'-o',str(binary)],check=True,capture_output=True)
            disc=workspace/'local/retail-v1.0/PS3_GAME/USRDIR'
            # Minimal artificial ELF structure for the supervisor's key-segment
            # identity check. These bytes are generated, not game material.
            elf=bytearray(136);elf[:7]=b'\x7fELF\x02\x02\x01'
            struct.pack_into('>Q',elf,32,64);struct.pack_into('>HH',elf,54,56,1)
            struct.pack_into('>I',elf,64,1);struct.pack_into('>Q',elf,72,120)
            struct.pack_into('>Q',elf,80,0x32A660);struct.pack_into('>Q',elf,96,16)
            elf[120:]=bytes(range(16));(disc/'EBOOT.elf').write_bytes(elf)
            (disc/'bin/mage_g.self').write_bytes(b'synthetic self fixture')
            manifest=json.loads((workspace/'profiles/retail-v1.0.json').read_text())
            digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
            manifest['images']['EBOOT']['elf_sha256']=digest(disc/'EBOOT.elf')
            manifest['images']['mage_g']['source_sha256']=digest(disc/'bin/mage_g.self')
            (workspace/'profiles/retail-v1.0.json').write_text(json.dumps(manifest))
            for folder,image in [('build','EBOOT'),('build-engine','mage_g')]:
                target=workspace/folder/'MagRecomp';shutil.copyfile(binary,target)
                receipt=target.with_suffix('.receipt.json');record=json.loads(receipt.read_text())
                record.update(binary_sha256=digest(target),elf_sha256=manifest['images'][image]['elf_sha256'])
                receipt.write_text(json.dumps(record))
            app=package({},workspace,root/'MAG.app')
            result=subprocess.run([str(app/'Contents/MacOS/MAG'),'--settings'],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,17,result.stderr)
            launch=next((root/'logs').glob('launch-*'))
            before=json.loads((launch/'launcher-env.json').read_text())
            after=json.loads((launch/'engine-env.json').read_text())
            self.assertTrue(before['setup']);self.assertTrue(before['show'])
            self.assertFalse(after['setup']);self.assertFalse(after['show'])
            self.assertEqual(before['settings_path'],after['settings_path'])
            self.assertTrue(before['settings_path'].endswith('/Library/Application Support/MAG/settings.json'))
            self.assertEqual(before['overlay_root'],after['overlay_root'])
            self.assertFalse(Path(before['overlay_root']).exists())
