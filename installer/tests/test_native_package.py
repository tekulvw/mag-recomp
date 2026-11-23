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

    def mag_folder(self, root):
        folder=root/'MAG';folder.mkdir();return folder

    def test_package_creates_real_app_structure_links_game_files_and_preserves_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();workspace=self.fixture(root/'build');folder=self.mag_folder(root);events=[]
            original=(workspace/'build/MagRecomp.receipt.json').read_bytes()
            with patch('mag_installer.native_package._run_tool',side_effect=self.tool):
                app=package({'python':'/usr/bin/python3'},workspace,folder/'MAG.app',events=events.append)
            self.assertTrue((app/'Contents/MacOS/MAG').is_file())
            self.assertTrue((app/'Contents/Info.plist').is_file())
            manifest=json.loads((app/'Contents/Resources/runtime.json').read_text())
            self.assertFalse((app/'Contents/Resources/PS3_GAME').exists())
            # The game files are in the MAG folder, hard links to the workspace's copies.
            self.assertEqual(manifest['game'],'../game');self.assertNotIn('workspace',manifest)
            for relative in ('PS3_GAME/USRDIR/EBOOT.elf','PS3_GAME/USRDIR/bin/mage_g.elf'):
                self.assertTrue((folder/'game'/relative).samefile(workspace/'local/retail-v1.0'/relative))
            # The build's receipt, which names build-machine paths, stays with the build; the
            # package receipt binds it by digest.
            self.assertFalse((app/'Contents/Resources/source-receipts').exists())
            receipt=json.loads((app/'Contents/Resources/package-receipts/launcher.json').read_text())
            self.assertEqual(receipt['source_receipt_sha256'],hashlib.sha256(original).hexdigest())
            self.assertEqual((workspace/'build/MagRecomp.receipt.json').read_bytes(),original)
            self.assertEqual(manifest['environment'],{})  # no movie decoder in this build
            self.assertEqual(events[-1]['event'],'app-packaged')
            self.assertEqual(sorted(p.name for p in folder.iterdir()),['MAG.app','game'])
            for path in app.rglob('*'):
                if path.is_file():self.assertNotIn(str(root).encode(),path.read_bytes(),path)
    def test_playback_defaults_match_the_boot_tool(self):
        import ast
        from mag_installer.native_package import MOVIE_PLAYBACK_ENV, playback_environment
        boot=ast.parse((Path(__file__).resolve().parents[2]/'tools/boot.py').read_text())
        value=next(ast.literal_eval(n.value) for n in boot.body if isinstance(n,ast.Assign) and any(getattr(t,'id',None)=='MOVIE_PLAYBACK_ENV' for t in n.targets))
        self.assertEqual(MOVIE_PLAYBACK_ENV,value)
        self.assertEqual(playback_environment({'launcher':{},'engine':{'movie_playback':True}}),value)
        self.assertEqual(playback_environment({'launcher':{'movie_playback':False},'engine':{}}),{})

    def test_workspace_inside_the_mag_folder_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();folder=self.mag_folder(root)
            workspace=self.fixture(folder/'build-work')
            with patch('mag_installer.native_package._run_tool',side_effect=self.tool):
                with self.assertRaisesRegex(InstallerError,'outside the MAG folder'):
                    package({},workspace,folder/'MAG.app')
            self.assertFalse((folder/'MAG.app').exists());self.assertFalse((folder/'game').exists())

    @unittest.skipUnless(platform.system() == 'Darwin' and Path('/usr/bin/xcrun').is_file(), 'macOS signing/toolchain fixture')
    def test_real_macos_packaging_signs_synthetic_dylib_closure_without_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();workspace=self.fixture(root)
            source=root/'library.c';source.write_text('int fixture(void) { return 0; }\n')
            library=root/'libfixture.dylib'
            subprocess.run(['/usr/bin/xcrun','clang','-dynamiclib',str(source),'-install_name',str(library),'-o',str(library)],check=True,capture_output=True)
            source=root/'main.c';source.write_text('int fixture(void); int main(void) { return fixture(); }\n')
            binary=root/'fixture'
            # Linked as the build links: with a search path into the build machine's tools.
            subprocess.run(['/usr/bin/xcrun','clang',str(source),str(library),'-Wl,-rpath,'+str(root),'-o',str(binary)],check=True,capture_output=True)
            for folder in ('build','build-engine'):
                target=workspace/folder/'MagRecomp';shutil.copyfile(binary,target)
                receipt=target.with_suffix('.receipt.json');record=json.loads(receipt.read_text())
                record['binary_sha256']=hashlib.sha256(target.read_bytes()).hexdigest();receipt.write_text(json.dumps(record))
            app=package({},workspace,self.mag_folder(root)/'MAG.app')
            subprocess.run(['/usr/bin/codesign','--verify','--deep','--strict',str(app)],check=True,capture_output=True)
            # Only the library's file name is recorded, not where the build found it.
            self.assertNotIn(str(root),(app/'Contents/Resources/runtime.json').read_text())
            dependencies=subprocess.check_output(['/usr/bin/otool','-L',str(app/'Contents/MacOS/MagRecomp')],text=True)
            self.assertIn('@executable_path/../Frameworks/libfixture.dylib',dependencies)
            self.assertNotIn(str(library),dependencies)
            commands=subprocess.check_output(['/usr/bin/otool','-l',str(app/'Contents/MacOS/MagRecomp')],text=True).split('\n',1)[1]
            self.assertNotIn(str(root),commands);self.assertNotIn('LC_RPATH',commands)
            self.assertTrue((app/'Contents/Frameworks/libfixture.dylib').is_file())

    def test_existing_destination_and_mismatched_receipts_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();workspace=self.fixture(root/'build');folder=self.mag_folder(root);app=folder/'MAG.app';app.mkdir()
            with self.assertRaises(InstallerError):package({'python':'/usr/bin/python3'},workspace,app)
            app.rmdir();(folder/'game').mkdir()
            with self.assertRaisesRegex(InstallerError,'already holds game files'):package({'python':'/usr/bin/python3'},workspace,app)
            (folder/'game').rmdir();(workspace/'build/MagRecomp').write_bytes(b'changed')
            with patch('mag_installer.native_package._run_tool',side_effect=self.tool):
                with self.assertRaisesRegex(InstallerError,'receipt'):package({'python':'/usr/bin/python3'},workspace,app)
            self.assertFalse(app.exists());self.assertFalse((folder/'game').exists())

    def test_failed_packaging_removes_its_game_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();workspace=self.fixture(root/'build');folder=self.mag_folder(root)
            def tool(argv,token):
                if argv[0].endswith('codesign'):raise InstallerError('synthetic signing failure')
                return self.tool(argv,token)
            with patch('mag_installer.native_package._run_tool',side_effect=tool):
                with self.assertRaisesRegex(InstallerError,'synthetic signing'):package({},workspace,folder/'MAG.app')
            self.assertEqual(list(folder.iterdir()),[])

    def test_dylib_closure_rewrites_then_signs_before_bundled_receipt_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();workspace=self.fixture(root);lib=root/'libfixture.dylib';lib.write_bytes(b'lib');calls=[]
            def run(argv,token):
                calls.append(argv)
                if argv[0].endswith('otool') and argv[1]=='-L':
                    dep='/usr/lib/libSystem.B.dylib' if argv[-1].endswith('.dylib') else str(lib)
                    return argv[-1]+':\n\t'+dep+' (compatibility version 1.0.0, current version 1.0.0)\n'
                if argv[0].endswith('otool') and argv[1]=='-l':
                    return ('Load command 9\n          cmd LC_RPATH\n      cmdsize 48\n         path /build/managed-tools/prefix/lib (offset 12)\n'
                            'Load command 10\n          cmd LC_RPATH\n      cmdsize 40\n         path @loader_path/../lib (offset 12)\n')
                if '-o' in argv:Path(argv[argv.index('-o')+1]).write_bytes(b'supervisor fixture')
                if argv[0].endswith('codesign') and Path(argv[-1]).is_file():
                    p=Path(argv[-1]);p.write_bytes(p.read_bytes()+b'-signed')
                return ''
            with patch('mag_installer.native_package._run_tool',side_effect=run):
                app=package({'python':'/usr/bin/python3'},workspace,self.mag_folder(root)/'MAG.app')
            binary=app/'Contents/MacOS/MagRecomp'
            receipt=json.loads((app/'Contents/Resources/package-receipts/launcher.json').read_text())
            self.assertEqual(receipt['binary_sha256'],hashlib.sha256(binary.read_bytes()).hexdigest())
            self.assertTrue(any('-change' in c for c in calls))
            deleted=[c for c in calls if '-delete_rpath' in c]
            self.assertTrue(deleted);self.assertTrue(all(c[c.index('-delete_rpath')+1]=='/build/managed-tools/prefix/lib' for c in deleted))
            self.assertTrue((app/'Contents/Frameworks/libfixture.dylib').is_file())

    def supervisor_workspace(self, root):
        """A finished workspace whose launcher and engine are one tiny native fixture."""
        workspace=self.fixture(root/'build')
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
 const char *movie=getenv("PS3RECOMP_SAIL_TIMED_THX"),*gamedata=getenv("PS3_GAMEDATA_ROOT");
 if(!gamedata)return 64;
 /* Each engine stage adds one line to a save file: a later launch must see the earlier ones. */
 int saves=0;
 if(!launcher){char save[4096];snprintf(save,sizeof save,"%s/save.txt",gamedata);
  FILE *s=fopen(save,"a");if(!s)return 65;fputs("x\n",s);fclose(s);
  s=fopen(save,"r");for(int c;(c=fgetc(s))!=EOF;)saves+=c=='\n';fclose(s);}
 fprintf(f,"{\"settings_path\":\"%s\",\"overlay_root\":\"%s\",\"setup\":%s,\"show\":%s,\"skip\":%s,\"movie\":\"%s\",\"gamedata\":\"%s\",\"saves\":%d}",
  path,overlay,getenv("MAG_PLAYER_SETUP")?"true":"false",getenv("MAG_PLAYER_SHOW_SETTINGS")?"true":"false",getenv("MAG_SKIP_SETUP")?"true":"false",movie?movie:"",gamedata,saves);fclose(f);
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
            record.update(binary_sha256=digest(target),elf_sha256=manifest['images'][image]['elf_sha256'],movie_playback=True)
            receipt.write_text(json.dumps(record))
        return workspace

    @unittest.skipUnless(platform.system() == 'Darwin', 'native supervisor fixture')
    def test_supervisor_carries_settings_through_handoff_and_cleans_owned_overlay(self):
        """Two tiny native fixtures stand in for both images; no guest runs."""
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();workspace=self.supervisor_workspace(root);mag=self.mag_folder(root)
            app=package({},workspace,mag/'MAG.app')
            result=subprocess.run([str(app/'Contents/MacOS/MAG'),'--settings'],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,17,result.stderr)
            launch=next((mag/'logs').glob('launch-*'))
            before=json.loads((launch/'launcher-env.json').read_text())
            after=json.loads((launch/'engine-env.json').read_text())
            self.assertTrue(before['setup']);self.assertTrue(before['show']);self.assertFalse(before['skip'])
            self.assertFalse(after['setup']);self.assertFalse(after['show']);self.assertFalse(after['skip'])
            self.assertEqual((before['movie'],after['movie']),('1','1'))  # decoder build: playback switches ship
            # Saves live in the installation's home/gamedata for both stages, not in the launch folder.
            self.assertEqual(Path(before['gamedata']).resolve(),(mag/'home/gamedata').resolve())
            self.assertEqual(before['gamedata'],after['gamedata']);self.assertEqual(after['saves'],1)
            # A second launch gets a new log folder but the same saves.
            again=subprocess.run([str(app/'Contents/MacOS/MAG')],capture_output=True,text=True,timeout=15)
            self.assertEqual(again.returncode,17,again.stderr)
            second=next(p for p in (mag/'logs').glob('launch-*') if p!=launch)
            later=json.loads((second/'engine-env.json').read_text())
            self.assertEqual(later['gamedata'],after['gamedata']);self.assertEqual(later['saves'],2)
            # Setup opens at every launch (the launcher decides); automation opts out explicitly.
            plain=json.loads((second/'launcher-env.json').read_text())
            self.assertTrue(plain['setup']);self.assertFalse(plain['show']);self.assertFalse(plain['skip'])
            for argv,extra in ((['--no-prompt'],{}),([],{'MAG_SKIP_SETUP':'1'})):
                seen=set((mag/'logs').glob('launch-*'))
                quiet=subprocess.run([str(app/'Contents/MacOS/MAG'),*argv],capture_output=True,text=True,timeout=15,env={**os.environ,**extra})
                self.assertEqual(quiet.returncode,17,quiet.stderr)
                launch_dir=next(p for p in (mag/'logs').glob('launch-*') if p not in seen)
                skipped=json.loads((launch_dir/'launcher-env.json').read_text())
                self.assertTrue(skipped['skip']);self.assertFalse(skipped['show'])
                self.assertFalse(json.loads((launch_dir/'engine-env.json').read_text())['skip'])
            self.assertEqual(before['settings_path'],after['settings_path'])
            self.assertTrue(before['settings_path'].endswith('/Library/Application Support/MAG/settings.json'))
            self.assertEqual(before['overlay_root'],after['overlay_root'])
            self.assertFalse(Path(before['overlay_root']).exists())
            # Self-contained: move the MAG folder elsewhere and delete the build folder (the
            # workspace and everything else the build used); the game still starts, with its saves.
            moved=root/'elsewhere'/'MAG Copy';moved.parent.mkdir();mag.rename(moved);shutil.rmtree(root/'build')
            relocated=subprocess.run([str(moved/'MAG.app/Contents/MacOS/MAG')],capture_output=True,text=True,timeout=15)
            self.assertEqual(relocated.returncode,17,relocated.stderr)
            third=max((moved/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime_ns)
            env=json.loads((third/'engine-env.json').read_text())
            self.assertEqual(Path(env['gamedata']).resolve(),(moved/'home/gamedata').resolve());self.assertEqual(env['saves'],5)
            # Without its game files it says so instead of starting.
            shutil.rmtree(moved/'game')
            missing=subprocess.run([str(moved/'MAG.app/Contents/MacOS/MAG')],capture_output=True,text=True,timeout=15)
            self.assertEqual(missing.returncode,1)
            self.assertIn('game files are missing',(moved/'launch-error.log').read_text())
