"""Synthetic Linux package and real supervisor tests; no retail assets."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from mag_installer.native_package import package
from mag_installer.model import InstallerError
import test_native_package as fixtures


class LinuxPackageTests(unittest.TestCase):
    def fixture(self, root):
        return fixtures.NativePackageTests().fixture(root)

    def test_dispatch_layout_and_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); workspace=self.fixture(root)
            def tool(argv, token):
                if '-o' in argv:
                    Path(argv[argv.index('-o')+1]).write_bytes(b'native supervisor')
                return ''
            with patch('mag_installer.native_package._run_tool',side_effect=tool):
                app=package({'host':'linux-x86_64'},workspace,root/'MAG')
            self.assertTrue(os.access(app/'MAG',os.X_OK))
            self.assertTrue(os.access(app/'bin/MagEngine',os.X_OK))
            self.assertEqual(json.loads((app/'resources/runtime.json').read_text())['workspace'],'../workspace')
            self.assertEqual((app/'resources/source-receipts/launcher.json').read_bytes(),(workspace/'build/MagRecomp.receipt.json').read_bytes())
            self.assertFalse((app/'.supervisor-build').exists())
            with self.assertRaises(InstallerError):
                package({'host':'linux-x86_64'},workspace,app)

    def test_dynamic_ffmpeg_roots_use_sonames_and_copy_transitive_dependencies(self):
        from mag_installer.native_package_linux import _copy_dependencies
        from mag_installer.safety import Cancellation
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);lib=root/'lib';lib.mkdir()
            codec=root/'libavcodec.so.63.1';codec.write_bytes(b'codec')
            utility=root/'libavutil.so.61';utility.write_bytes(b'utility')
            calls=[]
            def tool(argv, token):
                calls.append(argv)
                if argv[0]=='readelf':
                    return ' 0x000000000000000e (SONAME) Library soname: [libavcodec.so.63]'
                return 'libavutil.so.61 => '+str(utility)+' (0x1000)\nlibc.so.6 => /lib/libc.so.6 (0x2000)'
            with patch('mag_installer.native_package._run_tool',side_effect=tool):
                names=_copy_dependencies([],lib,Cancellation(),[codec])
            self.assertEqual(names,['libavcodec.so.63','libavutil.so.61'])
            self.assertEqual((lib/'libavcodec.so.63').read_bytes(),b'codec')
            self.assertEqual((lib/'libavutil.so.61').read_bytes(),b'utility')
            self.assertFalse((lib/'libc.so.6').exists())
            self.assertTrue(any('LD_LIBRARY_PATH='+str(root.resolve()) in call for call in calls))

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader')
    def test_native_dynamic_library_closure_loads_after_source_removal(self):
        from mag_installer.native_package_linux import _copy_dependencies
        from mag_installer.safety import Cancellation
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'managed';source.mkdir();dest=root/'lib';dest.mkdir()
            helper=source/'libfixture_helper.so.1'
            (root/'helper.c').write_text('int helper(void){return 17;}')
            subprocess.run(['cc','-shared','-fPIC',str(root/'helper.c'),'-Wl,-soname,libfixture_helper.so.1','-o',str(helper)],check=True,timeout=30)
            dynamic=source/'libfixture_codec.so.63.1'
            (root/'codec.c').write_text('int helper(void);int fixture(void){return helper();}')
            subprocess.run(['cc','-shared','-fPIC',str(root/'codec.c'),str(helper),'-Wl,-soname,libfixture_codec.so.63','-o',str(dynamic)],check=True,timeout=30)
            _copy_dependencies([],dest,Cancellation(),[dynamic])
            (root/'probe.c').write_text('#include <dlfcn.h>\nint main(void){void* h=dlopen("libfixture_codec.so.63",RTLD_NOW);if(!h)return 91;int(*fn)(void)=dlsym(h,"fixture");return fn?fn():92;}')
            subprocess.run(['cc',str(root/'probe.c'),'-ldl','-o',str(root/'probe')],check=True,timeout=30)
            import shutil
            shutil.rmtree(source)
            result=subprocess.run([str(root/'probe')],env=dict(os.environ,LD_LIBRARY_PATH=str(dest)),capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,17,result.stderr)

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_handoff_relocation_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); install=root/'install';install.mkdir();workspace=self.fixture(install)
            disc=workspace/'local/retail-v1.0/PS3_GAME/USRDIR'
            elf=bytearray(136);elf[:7]=b'\x7fELF\x02\x02\x01'
            struct.pack_into('>Q',elf,32,64);struct.pack_into('>HH',elf,54,56,1)
            struct.pack_into('>I',elf,64,1);struct.pack_into('>QQ',elf,72,120,0x32A660);struct.pack_into('>Q',elf,96,16)
            elf[120:136]=bytes(range(16));(disc/'EBOOT.elf').write_bytes(elf)
            (disc/'bin/mage_g.self').write_bytes(b'synthetic self')
            source=root/'fixture.c';source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
 if(strcmp(getenv("PS3RECOMP_VK_WINDOW"),"1")||strcmp(getenv("PS3RECOMP_VK_GUEST_PROGRAMS"),"1"))return 48;
 if(getenv("PS3_DIAGNOSTIC_TEST")||getenv("MAG_DIAGNOSTIC_TEST")||!getenv("DISPLAY"))return 45;
 const char* request=getenv("MAG_HANDOFF_REQUEST");
 const char* settings=getenv("MAG_PLAYER_SETTINGS_PATH"),*overlay=getenv("MAG_PLAYER_OVERLAY_ROOT");
 if(!settings||!overlay)return 49;
 FILE* state=fopen(request?"launcher-settings.txt":"engine-settings.txt","w");if(!state)return 50;
 fprintf(state,"%s\n%s\n%d\n%d\n",settings,overlay,getenv("MAG_PLAYER_SETUP")!=NULL,getenv("MAG_PLAYER_SHOW_SETTINGS")!=NULL);fclose(state);
 if(request){FILE* f=fopen(request,"wx");if(!f)return 46;
 fprintf(f,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n",getenv("MAG_HANDOFF_TOKEN"));fclose(f);puts("launcher fixture");return 90;}
 if(getenv("MAG_ENGINE_ELF")||strcmp(getenv("PS3_BOOT_PATH"),"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self"))return 47;
 puts("engine fixture");return 23;
}
''')
            subprocess.run(['cc',str(source),'-o',str(root/'fixture')],check=True,timeout=30)
            manifest=json.loads((workspace/'profiles/retail-v1.0.json').read_text())
            for folder,image,elfpath in [('build','EBOOT',disc/'EBOOT.elf'),('build-engine','mage_g',disc/'bin/mage_g.elf')]:
                binary=workspace/folder/'MagRecomp';binary.write_bytes((root/'fixture').read_bytes())
                digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
                receipt=binary.with_suffix('.receipt.json');record=json.loads(receipt.read_text());record.update(binary_sha256=digest(binary),elf_sha256=digest(elfpath));receipt.write_text(json.dumps(record));manifest['images'][image]['elf_sha256']=digest(elfpath)
            manifest['images']['mage_g']['source_sha256']=digest(disc/'bin/mage_g.self')
            (workspace/'profiles/retail-v1.0.json').write_text(json.dumps(manifest))
            package({'host':'linux-x86_64'},workspace,install/'MAG')
            moved=root/'relocated installation';install.rename(moved)
            env=dict(os.environ,DISPLAY=':fixture',XDG_CONFIG_HOME=str(root/'user-config'),PS3_DIAGNOSTIC_TEST='1',MAG_DIAGNOSTIC_TEST='1')
            run=subprocess.run([str(moved/'MAG/MAG'),'--settings'],env=env,capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,23,run.stderr)
            logs=list((moved/'logs').glob('launch-*'));self.assertEqual(len(logs),1)
            self.assertEqual((logs[0]/'engine.log').stat().st_mode&0o777,0o600)
            self.assertIn('engine fixture',(logs[0]/'engine.log').read_text())
            before=(logs[0]/'launcher-settings.txt').read_text().splitlines()
            after=(logs[0]/'engine-settings.txt').read_text().splitlines()
            self.assertEqual(before[:2],after[:2])
            self.assertEqual(before[2:],['1','1'])
            self.assertEqual(after[2:],['0','0'])
            self.assertEqual(before[0],str(root/'user-config/MAG/settings.json'))
            self.assertFalse(Path(before[1]).exists())

            (moved/'MAG/bin/MagEngine').write_bytes(b'changed')
            run=subprocess.run([str(moved/'MAG/MAG')],capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,1);self.assertIn('identity changed',run.stderr)
