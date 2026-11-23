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


REPOSITORY = Path(__file__).resolve().parents[2]


class LinuxPackageTests(unittest.TestCase):
    def fixture(self, root):
        workspace = fixtures.NativePackageTests().fixture(root)
        # The supervisor compiles the account link and settings rules from the workspace's source.
        from mag_installer.native_package_linux import SUPERVISOR_SOURCES
        (workspace / 'src').mkdir()
        for name in SUPERVISOR_SOURCES:
            (workspace / 'src' / name).write_bytes((REPOSITORY / 'src' / name).read_bytes())
        return workspace

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
 if(!getenv("PS3RECOMP_SAIL_TIMED_THX")||strcmp(getenv("PS3RECOMP_SAIL_ATRAC_AUDIO"),"1"))return 52;  /* decoder build defaults */
 const char* request=getenv("MAG_HANDOFF_REQUEST");
 const char* settings=getenv("MAG_PLAYER_SETTINGS_PATH");const char* owned=getenv("MAG_PLAYER_OVERLAY_ROOT");
 if(!settings||!strstr(settings,"/config/MAG/settings.json")||!owned||!strstr(owned,"player"))return 49;
 if(request&&(!getenv("MAG_PLAYER_SETUP")||strcmp(getenv("MAG_PLAYER_SET_SERVER"),"example.org")||getenv("MAG_PLAYER_SET_NAME")))return 50;
 if(!request&&(getenv("MAG_PLAYER_SETUP")||getenv("MAG_PLAYER_SET_SERVER")))return 51;
 const char* gamedata=getenv("PS3_GAMEDATA_ROOT");
 if(!gamedata||!strstr(gamedata,"home") || !strstr(gamedata,"gamedata"))return 53;
 if(!request){char save[4096];snprintf(save,sizeof save,"%s/save.txt",gamedata);FILE* s=fopen(save,"a");if(!s)return 54;fputs("x\n",s);fclose(s);}
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
                receipt=binary.with_suffix('.receipt.json');record=json.loads(receipt.read_text());record.update(binary_sha256=digest(binary),elf_sha256=digest(elfpath),movie_playback=True);receipt.write_text(json.dumps(record));manifest['images'][image]['elf_sha256']=digest(elfpath)
            manifest['images']['mage_g']['source_sha256']=digest(disc/'bin/mage_g.self')
            (workspace/'profiles/retail-v1.0.json').write_text(json.dumps(manifest))
            package({'host':'linux-x86_64'},workspace,install/'MAG')
            moved=root/'relocated installation';install.rename(moved)
            env=dict(os.environ,DISPLAY=':fixture',PS3_DIAGNOSTIC_TEST='1',MAG_DIAGNOSTIC_TEST='1',XDG_CONFIG_HOME=str(root/'config'))
            run=subprocess.run([str(moved/'MAG/MAG'),'--server','example.org'],env=env,capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,23,run.stderr)
            self.assertEqual((root/'config/MAG').stat().st_mode&0o777,0o700)
            save=moved/'home/gamedata/save.txt';self.assertEqual(save.read_text(),'x\n')  # saves stay in the installation
            bad=subprocess.run([str(moved/'MAG/MAG'),'--sever','example.org'],env=env,capture_output=True,text=True,timeout=15)
            self.assertEqual(bad.returncode,1);self.assertIn('Usage: MAG',bad.stderr)
            logs=list((moved/'logs').glob('launch-*'));self.assertEqual(len(logs),1)
            self.assertEqual((logs[0]/'engine.log').stat().st_mode&0o777,0o600)
            self.assertIn('engine fixture',(logs[0]/'engine.log').read_text())
            (moved/'MAG/bin/MagEngine').write_bytes(b'changed')
            run=subprocess.run([str(moved/'MAG/MAG')],env=env,capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,1);self.assertIn('identity changed',run.stderr)

    def install_fixture(self, root, game_source):
        """A packaged MAG whose launcher and engine are this synthetic C program."""
        install=root/'install';install.mkdir();workspace=self.fixture(install)
        disc=workspace/'local/retail-v1.0/PS3_GAME/USRDIR'
        elf=bytearray(136);elf[:7]=b'\x7fELF\x02\x02\x01'
        struct.pack_into('>Q',elf,32,64);struct.pack_into('>HH',elf,54,56,1)
        struct.pack_into('>I',elf,64,1);struct.pack_into('>QQ',elf,72,120,0x32A660);struct.pack_into('>Q',elf,96,16)
        elf[120:136]=bytes(range(16));(disc/'EBOOT.elf').write_bytes(elf)
        (disc/'bin/mage_g.self').write_bytes(b'synthetic self')
        source=root/'fixture.c';source.write_text(game_source)
        subprocess.run(['cc',str(source),'-o',str(root/'fixture')],check=True,timeout=30)
        digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        manifest=json.loads((workspace/'profiles/retail-v1.0.json').read_text())
        for folder,image,elfpath in [('build','EBOOT',disc/'EBOOT.elf'),('build-engine','mage_g',disc/'bin/mage_g.elf')]:
            binary=workspace/folder/'MagRecomp';binary.write_bytes((root/'fixture').read_bytes())
            receipt=binary.with_suffix('.receipt.json');record=json.loads(receipt.read_text())
            record.update(binary_sha256=digest(binary),elf_sha256=digest(elfpath));receipt.write_text(json.dumps(record))
            manifest['images'][image]['elf_sha256']=digest(elfpath)
        manifest['images']['mage_g']['source_sha256']=digest(disc/'bin/mage_g.self')
        (workspace/'profiles/retail-v1.0.json').write_text(json.dumps(manifest))
        package({'host':'linux-x86_64'},workspace,install/'MAG')
        return install

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_links_and_fetches_a_ticket_before_each_phase(self):
        import base64, http.server, threading
        TOKEN='mgd_'+'Fixture0123456789-_abcdefghijklmnopqrstuvw'
        ticket=b'MAGT\x01\x01'+bytes(8)+(100).to_bytes(8,'big')+(42).to_bytes(4,'big')+b'n'*16+b'\x0bexample.org\x06Rook_7'+b's'*64
        state={'polls':0,'revoked':False,'requests':[]}
        class Portal(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def reply(self,status,value):
                body=json.dumps(value).encode();self.send_response(status)
                self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def do_POST(self):
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])) or b'{}')
                state['requests'].append((self.path,self.headers.get('Authorization'),self.headers.get('Content-Type'),body))
                if self.path=='/api/v1/device/start':
                    state['polls']=0;page=f'http://127.0.0.1:{port}/link'
                    return self.reply(200,dict(device_code='mgl_fixture',user_code='BCDF-GHJK',verification_uri=page,
                                               verification_uri_complete=page+'?code=BCDF-GHJK',expires_in=60,interval=1))
                if self.path=='/api/v1/device/token':
                    state['polls']+=1
                    if body!={'device_code':'mgl_fixture'} or state['polls']<2:return self.reply(400,{'error':'authorization_pending'})
                    state['revoked']=False
                    return self.reply(200,{'device_token':TOKEN,'player_name':'Rook_7'})
                if self.path=='/api/v1/ticket':
                    if state['revoked'] or self.headers.get('Authorization')!='Bearer '+TOKEN:return self.reply(401,{'error':'invalid_token'})
                    return self.reply(200,dict(ticket=base64.b64encode(ticket).decode(),expires_at=1800000600,account_id=42,
                                               player_name='Rook_7',audience='example.org'))
                self.reply(404,{'error':'not_found'})
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Portal);port=server.server_address[1]
        threading.Thread(target=server.serve_forever,daemon=True).start()
        self.addCleanup(server.server_close)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            install=self.install_fixture(root,r'''
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 const char* request=getenv("MAG_HANDOFF_REQUEST");const char* ticket=getenv("MAG_PLAYER_PORTAL_TICKET");const char* name=getenv("MAG_PLAYER_SET_NAME");
 FILE* out=fopen(request?"launcher.env":"engine.env","w");if(!out)return 40;
 fprintf(out,"ticket=%s\nname=%s\nbytes=",ticket?ticket:"",name?name:"");
 if(ticket){FILE* in=fopen(ticket,"rb");int c;if(!in)return 41;while((c=fgetc(in))!=EOF)fprintf(out,"%02x",c);fclose(in);}
 fputs("\n",out);fclose(out);
 if(request){FILE* f=fopen(request,"wx");if(!f)return 46;
  fprintf(f,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n",getenv("MAG_HANDOFF_TOKEN"));fclose(f);return 90;}
 return 23;
}
''')
            config=root/'config/MAG';config.mkdir(parents=True,mode=0o700)
            (config/'settings.json').write_text(json.dumps({'server_address':'example.org','player_name':'Old_Name',
                                                            'portal_url':f'http://127.0.0.1:{port}'}))
            env={k:v for k,v in os.environ.items() if k not in ('DISPLAY','WAYLAND_DISPLAY')}  # never open a browser
            env['XDG_CONFIG_HOME']=str(root/'config')
            mag=lambda *args:subprocess.run([str(install/'MAG/MAG'),*args],env=env,capture_output=True,text=True,timeout=60)
            def phases():
                logs=max((install/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime)
                read=lambda name:dict(line.split('=',1) for line in (logs/name).read_text().splitlines())
                return read('launcher.env'),read('engine.env')
            link=config/'portal-link.json'
            # Link: the code and page are shown, the token is stored owner-only and never printed.
            run=mag('--link')
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn('BCDF-GHJK',run.stdout);self.assertIn(f'http://127.0.0.1:{port}/link?code=BCDF-GHJK',run.stdout)
            self.assertIn('Linked as Rook_7',run.stdout);self.assertNotIn(TOKEN,run.stdout+run.stderr)
            self.assertEqual(link.stat().st_mode&0o777,0o600);self.assertEqual(json.loads(link.read_text())['device_token'],TOKEN)
            self.assertEqual(state['requests'][0][3],{'client':'MAG launcher (Linux)'})
            self.assertTrue(all(r[2]=='application/json' for r in state['requests']))
            # Each phase gets a fresh ticket; the launcher takes the account's name, which wins.
            del state['requests'][:]
            run=mag()
            self.assertEqual(run.returncode,23,run.stderr)
            launcher,engine=phases();ticket_file=str(install/'home/player/portal-ticket.bin')
            self.assertEqual((launcher['ticket'],launcher['name'],launcher['bytes']),(ticket_file,'Rook_7',ticket.hex()))
            self.assertEqual((engine['ticket'],engine['name'],engine['bytes']),(ticket_file,'',ticket.hex()))
            self.assertEqual([(r[0],r[1]) for r in state['requests']],[('/api/v1/ticket','Bearer '+TOKEN)]*2)
            self.assertIn('you play as Rook_7',run.stderr);self.assertIn('(MAG had Old_Name)',run.stderr)
            self.assertEqual(Path(ticket_file).stat().st_mode&0o777,0o600);self.assertNotIn(TOKEN,run.stderr)
            # Unlink here.
            run=mag('--unlink')
            self.assertEqual(run.returncode,0);self.assertIn('no longer linked on this computer',run.stdout);self.assertFalse(link.exists())
            self.assertIn('not linked',mag('--unlink').stdout)
            # Unlinked on the website: told how to link again, the token is forgotten, the name sign-in is kept.
            self.assertEqual(mag('--link').returncode,0);state['revoked']=True
            run=mag()
            self.assertEqual(run.returncode,23,run.stderr)
            self.assertIn('no longer linked to your account',run.stderr);self.assertIn('./MAG --link',run.stderr)
            self.assertIn('signs in with your player name',run.stderr);self.assertFalse(link.exists())
            self.assertEqual([p['ticket'] for p in phases()],['',''])
            # Portal unreachable: said so, the link is kept, and the launch signs in by name.
            self.assertEqual(mag('--link').returncode,0)
            server.shutdown();server.server_close()
            run=mag()
            self.assertEqual(run.returncode,23,run.stderr)
            self.assertIn('Cannot reach the account portal',run.stderr);self.assertTrue(link.exists())
            self.assertEqual([p['ticket'] for p in phases()],['',''])
            # No portal for another server unless one is set: the link is not used at all.
            (config/'settings.json').write_text(json.dumps({'server_address':'example.org','player_name':'Rook_7'}))
            run=mag();self.assertEqual(run.returncode,23,run.stderr);self.assertNotIn('MAG account',run.stderr)
            run=mag('--link');self.assertEqual(run.returncode,1);self.assertIn('No account portal',run.stderr)
