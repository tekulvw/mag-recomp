"""Synthetic Linux package and real supervisor tests; no retail assets."""
import hashlib
import json
import os
from pathlib import Path
import shutil
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
    def test_logs_folder_for_the_debug_page(self):
        # MAG Settings' Debug page opens this MAG folder's logs, which the supervisor names.
        from mag_installer.native_package_linux import _SUPERVISOR
        self.assertIn('{"MAG_PLAYER_LOGS_ROOT",(root/"logs").string()}', _SUPERVISOR)

    def setUp(self):
        # Supervisor runs here send no reports, except in the reports test.
        environment = patch.dict(os.environ, {'MAG_REPORTS': '0'}); environment.start(); self.addCleanup(environment.stop)

    def fixture(self, root):
        workspace = fixtures.NativePackageTests().fixture(root)
        # The supervisor compiles the account link and settings rules from the workspace's source.
        from mag_installer.native_package_linux import SUPERVISOR_SOURCES
        (workspace / 'src').mkdir(exist_ok=True)
        for name in SUPERVISOR_SOURCES:
            (workspace / 'src' / name).write_bytes((REPOSITORY / 'src' / name).read_bytes())
        return workspace

    def test_dispatch_layout_and_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); workspace=self.fixture(root/'build'); folder=root/'MAG'; folder.mkdir()
            def tool(argv, token):
                if '-o' in argv:
                    Path(argv[argv.index('-o')+1]).write_bytes(b'native supervisor')
                return ''
            with patch('mag_installer.native_package._run_tool',side_effect=tool):
                app=package({'host':'linux-x86_64'},workspace,folder/'MAG')
            self.assertTrue(os.access(app/'MAG',os.X_OK))
            self.assertTrue(os.access(app/'bin/MagEngine',os.X_OK))
            runtime=json.loads((app/'resources/runtime.json').read_text())
            self.assertEqual(runtime['game'],'../game');self.assertNotIn('workspace',runtime)
            self.assertTrue((folder/'game/PS3_GAME/USRDIR/EBOOT.elf').samefile(workspace/'local/retail-v1.0/PS3_GAME/USRDIR/EBOOT.elf'))
            self.assertFalse((app/'resources/source-receipts').exists())
            receipt=json.loads((app/'resources/package-receipts/launcher.json').read_text())
            self.assertEqual(receipt['source_receipt_sha256'],hashlib.sha256((workspace/'build/MagRecomp.receipt.json').read_bytes()).hexdigest())
            self.assertFalse((app/'.supervisor-build').exists())
            with self.assertRaises(InstallerError):
                package({'host':'linux-x86_64'},workspace,app)

    def test_dynamic_ffmpeg_roots_use_sonames_and_copy_transitive_dependencies(self):
        from mag_installer.native_package_linux import _copy_dependencies
        from mag_installer.safety import Cancellation
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();lib=root/'lib';lib.mkdir()
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
            root=Path(tmp).resolve();source=root/'managed';source.mkdir();dest=root/'lib';dest.mkdir()
            helper=source/'libfixture_helper.so.1'
            (root/'helper.c').write_text('int helper(void){return 17;}')
            subprocess.run(['cc','-shared','-fPIC',str(root/'helper.c'),'-Wl,-soname,libfixture_helper.so.1','-o',str(helper)],check=True,timeout=30)
            dynamic=source/'libfixture_codec.so.63.1'
            (root/'codec.c').write_text('int helper(void);int fixture(void){return helper();}')
            subprocess.run(['cc','-shared','-fPIC',str(root/'codec.c'),str(helper),'-Wl,-soname,libfixture_codec.so.63','-o',str(dynamic)],check=True,timeout=30)
            _copy_dependencies([],dest,Cancellation(),[dynamic])
            (root/'probe.c').write_text('#include <dlfcn.h>\nint main(void){void* h=dlopen("libfixture_codec.so.63",RTLD_NOW);if(!h)return 91;int(*fn)(void)=dlsym(h,"fixture");return fn?fn():92;}')
            subprocess.run(['cc',str(root/'probe.c'),'-ldl','-o',str(root/'probe')],check=True,timeout=30)
            shutil.rmtree(source)
            result=subprocess.run([str(root/'probe')],env=dict(os.environ,LD_LIBRARY_PATH=str(dest)),capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,17,result.stderr)

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_handoff_relocation_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); install=root/'install';install.mkdir();workspace=self.fixture(root/'build')
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
            # Self-contained: the moved MAG folder starts with its build folder deleted.
            moved=root/'relocated installation';install.rename(moved);shutil.rmtree(root/'build')
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
            shutil.rmtree(moved/'game')
            run=subprocess.run([str(moved/'MAG/MAG')],env=env,capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,1);self.assertIn('game files are missing',run.stderr)

    def install_fixture(self, root, game_source):
        """A packaged MAG whose launcher and engine are this synthetic C program."""
        # The build workspace sits beside the MAG folder, never inside it.
        install=root/'install';install.mkdir();workspace=self.fixture(root/'build')
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

    # A launcher and engine stand-in that records each phase's ticket and name, then hands off.
    PHASE_RECORDER=r'''
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
'''

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_opens_setup_at_every_launch(self):
        """MAG Settings with Play MAG before every launch (the engine's settings window), unless the caller
        opts out; Cancel plays nothing; a missing name reopens it with a notice."""
        source = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv) {
 const char* home=getenv("HOME");char path[4096];snprintf(path,sizeof path,"%s/calls.txt",home);
 FILE* f=fopen(path,"a");if(!f)return 60;
 const char* skip=getenv("MAG_SKIP_SETUP");
 if(argc>1&&!strcmp(argv[1],"--mag-player-settings-setup")){
  fprintf(f,"setup %s skip=%s settings=%s\n",argc>2?argv[2]:"-",skip?skip:"0",getenv("MAG_PLAYER_SETTINGS_PATH")?"1":"0");fclose(f);
  snprintf(path,sizeof path,"%s/named",home);FILE* named=fopen(path,"w");if(named)fclose(named);
  snprintf(path,sizeof path,"%s/answer",home);FILE* a=fopen(path,"r");int code=0;if(a){if(fscanf(a,"%d",&code)!=1)code=0;fclose(a);}
  return code;}
 const char* request=getenv("MAG_HANDOFF_REQUEST");
 fprintf(f,"%s skip=%s\n",request?"launcher":"engine",skip?skip:"0");fclose(f);
 if(!request)return 23;
 snprintf(path,sizeof path,"%s/needs-name",home);
 if(access(path,F_OK)==0){char named[4096];snprintf(named,sizeof named,"%s/named-after",home);
  if(access(named,F_OK)!=0){FILE* n=fopen(named,"w");if(n)fclose(n);
   snprintf(path,sizeof path,"%s/player/needs-player-name",home);FILE* m=fopen(path,"w");if(m)fclose(m);return 9;}}
 FILE* r=fopen(request,"wx");if(!r)return 46;
 fprintf(r,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n",getenv("MAG_HANDOFF_TOKEN"));fclose(r);return 90;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            install = self.install_fixture(root, source)
            home = install / 'home'
            base = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY', 'MAG_SKIP_SETUP')}
            base['XDG_CONFIG_HOME'] = str(root / 'config')

            def launch(*args, **extra):
                calls = home / 'calls.txt'
                calls.unlink(missing_ok=True)
                run = subprocess.run([str(install / 'MAG/MAG'), *args], env={**base, **extra}, capture_output=True, text=True, timeout=30)
                return run, calls.read_text().splitlines() if calls.exists() else []

            run, calls = launch(DISPLAY=':fixture')
            self.assertEqual(run.returncode, 23, run.stderr)
            self.assertEqual(calls, ['setup - skip=1 settings=1', 'launcher skip=1', 'engine skip=0'])
            run, calls = launch(DISPLAY=':fixture')  # every launch, not only the first
            self.assertEqual(calls[0], 'setup - skip=1 settings=1')
            (home / 'answer').write_text('3')  # Cancel: nothing plays
            run, calls = launch(DISPLAY=':fixture')
            self.assertEqual((run.returncode, calls), (0, ['setup - skip=1 settings=1']))
            (home / 'answer').write_text('1')  # the window could not open: play with the saved settings
            run, calls = launch(DISPLAY=':fixture')
            self.assertEqual((run.returncode, calls[1:]), (23, ['launcher skip=1', 'engine skip=0']))
            (home / 'answer').unlink()
            for args, extra in ((['--no-prompt'], {'DISPLAY': ':fixture'}), ([], {'DISPLAY': ':fixture', 'MAG_SKIP_SETUP': '1'}),
                                ([], {}), (['--server', 'example.org', '--name', 'Rook_7'], {'DISPLAY': ':fixture'})):
                run, calls = launch(*args, **extra)
                self.assertEqual((run.returncode, calls), (23, ['launcher skip=1', 'engine skip=0']), (args, extra))
            # The game needs a name: the window opens again saying so, then the launcher starts again.
            (home / 'needs-name').write_text('')
            run, calls = launch(DISPLAY=':fixture')
            self.assertEqual(run.returncode, 23, run.stderr)
            self.assertEqual(calls, ['setup - skip=1 settings=1', 'launcher skip=1', 'setup --notice=player-name skip=1 settings=1',
                                     'launcher skip=1', 'engine skip=0'])

    # A launcher and engine stand-in that records the input method variables each phase receives.
    INPUT_METHOD_RECORDER=r'''
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 const char* request=getenv("MAG_HANDOFF_REQUEST");
 FILE* out=fopen(request?"launcher.env":"engine.env","w");if(!out)return 40;
 const char* names[]={"XMODIFIERS","SDL_IM_MODULE","XDG_SESSION_TYPE","IBUS_ADDRESS","GTK_IM_MODULE"};
 for(int i=0;i<5;i++){const char* v=getenv(names[i]);fprintf(out,"%s=%s\n",names[i],v?v:"(unset)");}
 fclose(out);
 if(request){FILE* f=fopen(request,"wx");if(!f)return 46;
  fprintf(f,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n",getenv("MAG_HANDOFF_TOKEN"));fclose(f);return 90;}
 return 23;
}
'''

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_passes_the_desktop_input_method_on(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            install=self.install_fixture(root,self.INPUT_METHOD_RECORDER)
            base={k:v for k,v in os.environ.items()
                  if k not in ('DISPLAY','WAYLAND_DISPLAY','XMODIFIERS','SDL_IM_MODULE','XDG_SESSION_TYPE','IBUS_ADDRESS','GTK_IM_MODULE')}
            base['XDG_CONFIG_HOME']=str(root/'config')
            def launch(**extra):
                run=subprocess.run([str(install/'MAG/MAG'),'--no-prompt'],env={**base,**extra},capture_output=True,text=True,timeout=60)
                self.assertEqual(run.returncode,23,run.stderr)
                logs=max((install/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime)
                read=lambda name:dict(line.split('=',1) for line in (logs/name).read_text().splitlines())
                launcher,engine=read('launcher.env'),read('engine.env')
                self.assertEqual(launcher,engine,'both phases get the same input method')
                return launcher
            # The input method's choice reaches the game; other toolkits' modules are not SDL's.
            seen=launch(XMODIFIERS='@im=fcitx',SDL_IM_MODULE='fcitx',XDG_SESSION_TYPE='x11',IBUS_ADDRESS='unix:path=/run/ibus-fixture',
                        GTK_IM_MODULE='ibus')
            self.assertEqual(seen,{'XMODIFIERS':'@im=fcitx','SDL_IM_MODULE':'fcitx','XDG_SESSION_TYPE':'x11',
                                   'IBUS_ADDRESS':'unix:path=/run/ibus-fixture','GTK_IM_MODULE':'(unset)'})
            # Nothing set, nothing passed (and no IBus address file to read).
            self.assertEqual(set(launch().values()),{'(unset)'})
            # IBus's address file under the player's own configuration folder, where SDL would look
            # without the game's private HOME: <machine id>-<host or unix>-<display number>.
            machine_id=''
            for path in ('/var/lib/dbus/machine-id','/etc/machine-id'):
                try: machine_id=Path(path).read_text().splitlines()[0].strip()
                except (OSError,IndexError): machine_id=''
                if machine_id: break
            if not machine_id:
                self.skipTest('no D-Bus machine id on this host for the IBus address file')
            bus=root/'config/ibus/bus';bus.mkdir(parents=True)
            (bus/f'{machine_id}-unix-7').write_text('# This file is created by ibus-daemon, please do not modify it.\n'
                                                    'IBUS_ADDRESS=unix:path=/tmp/ibus-fixture,guid=00\nIBUS_DAEMON_PID=1\n')
            (bus/f'{machine_id}-unix-wayland-0').write_text('IBUS_ADDRESS=unix:path=/tmp/ibus-wayland\n')
            self.assertEqual(launch(DISPLAY=':7.0')['IBUS_ADDRESS'],'unix:path=/tmp/ibus-fixture,guid=00')
            self.assertEqual(launch(DISPLAY=':0',XDG_SESSION_TYPE='wayland')['IBUS_ADDRESS'],'unix:path=/tmp/ibus-wayland')
            self.assertEqual(launch(DISPLAY=':9')['IBUS_ADDRESS'],'(unset)','no file for that display')
            self.assertEqual(launch(DISPLAY=':7',IBUS_ADDRESS='unix:path=/run/own')['IBUS_ADDRESS'],'unix:path=/run/own',
                             'an address the session sets wins')

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
                    if body!={'audience':'example.org'}:return self.reply(400,{'error':'unknown_audience'})
                    return self.reply(200,dict(ticket=base64.b64encode(ticket).decode(),expires_at=1800000600,account_id=42,
                                               player_name='Rook_7',audience='example.org'))
                self.reply(404,{'error':'not_found'})
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Portal);port=server.server_address[1]
        threading.Thread(target=server.serve_forever,daemon=True).start()
        self.addCleanup(server.server_close)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            install=self.install_fixture(root,self.PHASE_RECORDER)
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
            # Each ticket is asked for the configured server's audience.
            self.assertEqual([(r[0],r[1],r[3]) for r in state['requests']],[('/api/v1/ticket','Bearer '+TOKEN,{'audience':'example.org'})]*2)
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

    # A game stand-in for friends (docs/player-social.md): the engine phase publishes presence through
    # the spool and waits for the friends list the supervisor's worker polled from the portal.
    SOCIAL_GAME=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern char** environ;
int main(void) {
 const char* request=getenv("MAG_HANDOFF_REQUEST");const char* spool=getenv("MAG_SOCIAL_SPOOL");
 FILE* out=fopen(request?"launcher.env":"engine.env","w");if(!out)return 40;
 fprintf(out,"social=%s\n",spool?spool:"");
 int token=0;for(char** e=environ;*e;e++)if(strstr(*e,"mgd_"))token=1;
 fprintf(out,"token_in_env=%d\n",token);
 if(request){fclose(out);FILE* f=fopen(request,"wx");if(!f)return 46;
  fprintf(f,"{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n",getenv("MAG_HANDOFF_TOKEN"));fclose(f);return 90;}
 if(spool){
  char path[4096];snprintf(path,sizeof path,"%s/from-game",spool);
  FILE* cmd=fopen(path,"a");if(!cmd)return 41;fputs("{\"t\":\"presence\",\"data_b64\":\"AQI=\"}\n",cmd);fclose(cmd);
  snprintf(path,sizeof path,"%s/to-game",spool);
  char line[4096]="";
  for(int i=0;i<200&&!strstr(line,"\"event\"");i++){FILE* in=fopen(path,"r");if(in){if(!fgets(line,sizeof line,in))line[0]=0;fclose(in);}usleep(50000);}
  fprintf(out,"event=%s",line[0]?line:"\n");
  usleep(300000);   /* the presence publish */
 }
 fclose(out);return 23;
}
'''

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_runs_friends_beside_the_engine(self):
        """Friends and presence (docs/player-social.md): for a linked account the supervisor runs the
        portal worker during the engine phase only, the game talks to it through the spool folder and
        never sees the device token, and the player is marked offline after the game exits."""
        import base64, http.server, threading
        TOKEN='mgd_'+'Fixture0123456789-_abcdefghijklmnopqrstuvw'
        ticket=b'MAGT\x01\x01'+bytes(8)+(100).to_bytes(8,'big')+(42).to_bytes(4,'big')+b'n'*16+b'\x0bexample.org\x06Rook_7'+b's'*64
        seen=[]
        class Portal(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def reply(self,status,value):
                body=json.dumps(value).encode();self.send_response(status)
                self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def body(self):
                length=self.headers.get('Content-Length');return json.loads(self.rfile.read(int(length)) or b'{}') if length else None
            def do_POST(self):
                self.body()
                if self.path=='/api/v1/ticket':
                    return self.reply(200,dict(ticket=base64.b64encode(ticket).decode(),expires_at=1800000600,account_id=42,
                                               player_name='Rook_7',audience='example.org'))
                self.reply(404,{'error':'not_found'})
            def do_GET(self):
                seen.append(('GET',self.path,self.headers.get('Authorization'),None))
                if self.path=='/api/v1/social':
                    return self.reply(200,{'self':'Rook_7','friends':[{'name':'alice','online':True,
                                           'presence':{'data_b64':'BQY=','updated_at':'2026-11-24T00:00:00Z'}}],
                                           'incoming':[],'outgoing':[],'blocked':[],'poll_seconds':15})
                self.reply(404,{'error':'not_found'})
            def do_PUT(self):
                seen.append(('PUT',self.path,self.headers.get('Authorization'),self.body()))
                self.send_response(204);self.send_header('Content-Length','0');self.end_headers()
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Portal);port=server.server_address[1]
        threading.Thread(target=server.serve_forever,daemon=True).start()
        self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            install=self.install_fixture(root,self.SOCIAL_GAME)
            config=root/'config/MAG';config.mkdir(parents=True,mode=0o700)
            portal=f'http://127.0.0.1:{port}'
            (config/'settings.json').write_text(json.dumps({'server_address':'example.org','player_name':'Rook_7','portal_url':portal}))
            link=config/'portal-link.json'
            link.write_text(json.dumps({'schema_version':1,'portal':portal,'device_token':TOKEN,'player_name':'Rook_7'}));link.chmod(0o600)
            env={k:v for k,v in os.environ.items() if k not in ('DISPLAY','WAYLAND_DISPLAY','MAG_SOCIAL')}
            env['XDG_CONFIG_HOME']=str(root/'config');env['MAG_REPORTS']='0'
            def launch(extra=None):
                run=subprocess.run([str(install/'MAG/MAG'),'--no-prompt'],env={**env,**(extra or {})},capture_output=True,text=True,timeout=90)
                self.assertEqual(run.returncode,23,run.stdout+run.stderr)
                logs=max((install/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime)
                read=lambda name:dict(line.split('=',1) for line in (logs/name).read_text().splitlines() if '=' in line)
                return read('launcher.env'),read('engine.env'),run
            launcher,engine,run=launch()
            spool=install/'home/player/social'
            self.assertEqual(launcher['social'],'');self.assertEqual(engine['social'],str(spool))
            self.assertEqual((launcher['token_in_env'],engine['token_in_env']),('0','0'))
            event=json.loads(engine['event'])
            self.assertEqual((event['t'],event['event'],event['name'],event['data_b64']),('event',1,'alice','BQY='))
            self.assertIn(('GET','/api/v1/social','Bearer '+TOKEN,None),seen)
            puts=[r[3] for r in seen if r[0]=='PUT']
            self.assertEqual(puts[0],{'data_b64':'AQI=','state':'online'});self.assertEqual(puts[-1],{'data_b64':'AQI=','state':'offline'})
            self.assertEqual(spool.stat().st_mode&0o777,0o700)
            for name in ('to-game','from-game'):self.assertNotIn(TOKEN,(spool/name).read_text())
            self.assertNotIn(TOKEN,run.stdout+run.stderr)
            # MAG_SOCIAL=0: no worker, no spool for the game, nothing asked of the portal.
            del seen[:]
            launcher,engine,run=launch({'MAG_SOCIAL':'0'})
            self.assertEqual(engine['social'],'');self.assertEqual(seen,[])
            # Not linked: quiet.
            link.unlink()
            launcher,engine,run=launch()
            self.assertEqual(engine['social'],'');self.assertEqual(seen,[]);self.assertNotIn('MAG friends',run.stderr)

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_imports_a_service_account_token(self):
        """--link-token: a staff-minted token from standard input, checked against a ticket-enforcing test server's portal."""
        import base64, http.server, threading
        TOKEN='mgd_'+'Service0123456789-_abcdefghijklmnopqrstuvw'
        def ticket_bytes(audience,name):
            return (b'MAGT\x01\x01'+bytes(8)+(100).to_bytes(8,'big')+(7).to_bytes(4,'big')+b'n'*16+
                    bytes([len(audience)])+audience.encode()+bytes([len(name)])+name.encode()+b's'*64)
        state={'audiences':{'test.example.org'},'revoked':False,'requests':[]}
        class Portal(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def reply(self,status,value):
                body=json.dumps(value).encode();self.send_response(status)
                self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def do_POST(self):
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])) or b'{}')
                state['requests'].append((self.path,self.headers.get('Authorization'),body))
                if self.path!='/api/v1/ticket':return self.reply(404,{'error':'not_found'})
                if state['revoked'] or self.headers.get('Authorization')!='Bearer '+TOKEN:return self.reply(401,{'error':'invalid_token'})
                audience=body.get('audience','play.example.net')  # no body: the portal's default audience
                if audience not in state['audiences']:return self.reply(400,{'error':'unknown_audience'})
                return self.reply(200,dict(ticket=base64.b64encode(ticket_bytes(audience,'Svc_Bot')).decode(),expires_at=1800000600,
                                           account_id=7,player_name='Svc_Bot',audience=audience))
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Portal);port=server.server_address[1]
        threading.Thread(target=server.serve_forever,daemon=True).start()
        self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
        portal=f'http://127.0.0.1:{port}'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            install=self.install_fixture(root,self.PHASE_RECORDER)
            config=root/'config/MAG';config.mkdir(parents=True,mode=0o700)
            settings=config/'settings.json';link=config/'portal-link.json'
            settings.write_text(json.dumps({'server_address':'example.org','player_name':'Old_Name'}))
            env={k:v for k,v in os.environ.items() if k not in ('DISPLAY','WAYLAND_DISPLAY')}
            env['XDG_CONFIG_HOME']=str(root/'config')
            def mag(*args,stdin=''):
                return subprocess.run([str(install/'MAG/MAG'),*args],env=env,input=stdin,capture_output=True,text=True,timeout=60)
            # No portal applies to this server yet: refused before reading the token or contacting anyone.
            run=mag('--link-token',stdin=TOKEN+'\n')
            self.assertEqual(run.returncode,1);self.assertIn('No account portal applies to example.org',run.stderr)
            self.assertFalse(link.exists());self.assertEqual(state['requests'],[])
            # The token never goes on the command line, and is not echoed back when it is put there.
            run=mag('--link-token',TOKEN)
            self.assertEqual(run.returncode,1);self.assertNotIn(TOKEN,run.stdout+run.stderr);self.assertFalse(link.exists())
            run=mag('--portal-url',portal)  # --portal-url belongs to --link-token
            self.assertEqual(run.returncode,1);self.assertIn('Usage',run.stderr)
            # Garbage on standard input: nothing stored or sent.
            run=mag('--server','test.example.org','--portal-url',portal,'--link-token',stdin='hunter2\n')
            self.assertEqual(run.returncode,1);self.assertIn('no device token',run.stderr)
            self.assertFalse(link.exists());self.assertEqual(state['requests'],[])
            # Linked: the server and portal are saved, the token stored owner-only and checked with one ticket for that server.
            run=mag('--server','Test.Example.org','--portal-url',portal+'/','--link-token',stdin='  '+TOKEN+'\r\n')
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn(f'Linked as Svc_Bot at {portal} for test.example.org',run.stdout)
            self.assertNotIn(TOKEN,run.stdout+run.stderr)
            self.assertEqual(link.stat().st_mode&0o777,0o600)
            self.assertEqual(json.loads(link.read_text()),{'schema_version':1,'portal':portal,'device_token':TOKEN,'player_name':'Svc_Bot'})
            saved=json.loads(settings.read_text())
            self.assertEqual((saved['server_address'],saved['portal_url'],saved['player_name']),('test.example.org',portal,'Svc_Bot'))
            self.assertEqual(state['requests'],[('/api/v1/ticket','Bearer '+TOKEN,{'audience':'test.example.org'})])
            # A launch then fetches a ticket for that server before each phase.
            del state['requests'][:]
            run=mag()
            self.assertEqual(run.returncode,23,run.stderr)
            logs=max((install/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime)
            launcher=dict(line.split('=',1) for line in (logs/'launcher.env').read_text().splitlines())
            self.assertEqual(launcher['bytes'],ticket_bytes('test.example.org','Svc_Bot').hex())
            self.assertEqual([r[2] for r in state['requests']],[{'audience':'test.example.org'}]*2)
            # A portal that does not issue tickets for the server: said so, and the token is kept for the next try.
            state['audiences']=set()
            run=mag('--link-token',stdin=TOKEN)
            self.assertEqual(run.returncode,1)
            self.assertIn(f"This account portal ({portal}) doesn't issue tickets for test.example.org",run.stderr)
            self.assertIn('The token is saved',run.stderr);self.assertTrue(link.exists());self.assertNotIn(TOKEN,run.stderr)
            # A revoked token is refused and not kept.
            state['audiences']={'test.example.org'};state['revoked']=True
            run=mag('--link-token',stdin=TOKEN)
            self.assertEqual(run.returncode,1);self.assertIn('does not accept this token',run.stderr);self.assertFalse(link.exists())

    @unittest.skipUnless(sys.platform=='linux','requires Linux ELF loader and OpenSSL development headers')
    def test_native_supervisor_reports_a_crash_to_the_linked_portal(self):
        """The engine stand-in exits 23: a crash report is queued, and a detached sender posts it to
        the linked account's (fake) portal; the supervisor never waits for it."""
        import http.server, threading, time
        TOKEN='mgd_'+'Report0123456789-_abcdefghijklmnopqrstuvw'
        seen=[]
        class Portal(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_POST(self):
                body=self.rfile.read(int(self.headers['Content-Length']));seen.append((self.path,self.headers.get('Authorization'),body))
                reply=json.dumps({'report_id':json.loads(body).get('report_id'),'received_at':'2026-11-23T00:00:00Z'}).encode()
                self.send_response(201);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(reply)))
                self.end_headers();self.wfile.write(reply)
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Portal)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
        portal=f'http://127.0.0.1:{server.server_address[1]}'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            install=self.install_fixture(root,self.PHASE_RECORDER)
            config=root/'config/MAG';config.mkdir(parents=True,mode=0o700)
            (config/'settings.json').write_text(json.dumps({'server_address':'example.org','player_name':'Rook_7','portal_url':portal}))
            (config/'portal-link.json').write_text(json.dumps({'schema_version':1,'portal':portal,'device_token':TOKEN,'player_name':'Rook_7'}))
            env={k:v for k,v in os.environ.items() if k not in ('DISPLAY','WAYLAND_DISPLAY','MAG_REPORTS')}
            env['XDG_CONFIG_HOME']=str(root/'config')
            run=subprocess.run([str(install/'MAG/MAG')],env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(run.returncode,23,run.stderr)
            logs=max((install/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime)
            deadline=time.monotonic()+60
            while time.monotonic()<deadline and 'reports: sent' not in ((logs/'supervisor.log').read_text() if (logs/'supervisor.log').exists() else ''):
                time.sleep(.2)
            log=(logs/'supervisor.log').read_text()
            self.assertIn('reports: queued a crash report',log);self.assertIn('reports: sent 1',log);self.assertNotIn(TOKEN,log)
            reports=[r for r in seen if r[0]=='/api/v1/reports']
            self.assertEqual(len(reports),1)
            self.assertEqual(reports[0][1],'Bearer '+TOKEN)
            report=json.loads(reports[0][2])
            self.assertEqual((report['trigger'],report['client']['platform']),('crash','linux'))
            self.assertEqual((report['session']['launcher_exit'],report['session']['engine_exit']),(90,23))
            self.assertNotIn(TOKEN.encode(),reports[0][2]);self.assertNotIn(str(root).encode(),reports[0][2])
            self.assertEqual(list((install/'home/reports').glob('*.json')),[])
            # MAG_REPORTS=0: no report at all.
            env['MAG_REPORTS']='0'
            run=subprocess.run([str(install/'MAG/MAG')],env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(run.returncode,23,run.stderr)
            logs=max((install/'logs').glob('launch-*'),key=lambda p:p.stat().st_mtime)
            self.assertFalse((logs/'supervisor.log').exists())
