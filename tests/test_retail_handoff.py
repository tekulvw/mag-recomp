"""Exercise the actual handoff HLE adapters with bounded synthetic guest memory."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
GUEST_PATH='/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self'


@unittest.skipUnless(sys.platform=='darwin','MAG adapter uses macOS CommonCrypto')
class RetailHandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        root=Path(cls.build.name)
        # The adapter accesses only the leading architectural GPR array.
        (root/'ppu_recomp.h').write_text('#include <stdint.h>\nstruct ppu_context { uint64_t gpr[32]; };\n')
        (root/'main.cpp').write_text(r'''
#include "ppu_recomp.h"
#include "retail_handoff.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
uint8_t memory[65536];
uint8_t* vm_base=memory;
uint32_t ppu_vm_size=sizeof(memory);
}
static void (*available)(ppu_context*);
static void (*spawn)(ppu_context*);
extern "C" void ps3_hle_register_ctx(uint32_t nid,const char*,void (*fn)(ppu_context*)) {
    if(nid==0xAD218FAF) { assert(!available);available=fn; }
    else if(nid==0xE6C8F3F9) { assert(!spawn);spawn=fn; }
    else assert(false);
}
int main(int argc,char** argv) {
    const char* mode=argc>1?argv[1]:"available";
    mag_register_handoff();
    if(!strcmp(mode,"disabled")) return available||spawn?1:0;
    if(!available||!spawn) return 2;
    for(unsigned i=0;i<16;i++) memory[0x100+i]=(uint8_t)i;
    strcpy((char*)memory+0x200,"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self");
    ppu_context ctx{};
    ctx.gpr[3]=0x100;ctx.gpr[4]=0x200;
    if(!strcmp(mode,"bad-key"))memory[0x100]^=1;
    if(!strcmp(mode,"bad-path"))memory[0x200+1]='x';
    if(!strcmp(mode,"null-key"))ctx.gpr[3]=0;
    if(!strcmp(mode,"key-oob"))ctx.gpr[3]=65530;
    if(!strcmp(mode,"wide-pointer"))ctx.gpr[3]=0x100000100ull;
    if(!strcmp(mode,"path-oob"))ctx.gpr[4]=65536;
    if(!strcmp(mode,"unterminated"))memset(memory+0x200,'x',1024);
    if(!strcmp(mode,"full-vm"))ppu_vm_size=0;
    if(!strcmp(mode,"full-vm-oob")) { ppu_vm_size=0;ctx.gpr[3]=0x100000000ull; }
    if(!strcmp(mode,"full-vm-crossing")) { ppu_vm_size=0;ctx.gpr[3]=0xfffffff8ull; }
    if(!strcmp(mode,"null-vm"))vm_base=nullptr;
    available(&ctx);
    assert(ctx.gpr[3]==0);
    if(!strcmp(mode,"available") || !strcmp(mode,"full-vm"))return 0;
    if(!strcmp(mode,"changed-file") || !strcmp(mode,"changed-before-spawn")) {
        FILE* f=fopen(getenv("MAG_ENGINE_ELF"),"ab");assert(f);fputc('x',f);fclose(f);
        if(!strcmp(mode,"changed-file")) {
            ctx.gpr[3]=0x100;available(&ctx);return 3;
        }
    }
    ctx.gpr[3]=0x100;ctx.gpr[5]=0x300;ctx.gpr[9]=1001;
    ctx.gpr[10]=0x4000000000000000ull;
    if(!strcmp(mode,"null-argv"))ctx.gpr[5]=0;
    if(!strcmp(mode,"argv-oob"))ctx.gpr[5]=65534;
    if(!strcmp(mode,"nonempty-argv"))memory[0x303]=1;
    if(!strcmp(mode,"envp"))ctx.gpr[6]=0x400;
    if(!strcmp(mode,"data"))ctx.gpr[7]=0x400;
    if(!strcmp(mode,"data-size"))ctx.gpr[8]=1;
    if(!strcmp(mode,"priority"))ctx.gpr[9]=1002;
    if(!strcmp(mode,"flags"))ctx.gpr[10]=0x20;
    spawn(&ctx);
    return 4; // A successful spawn must not return to the launcher.
}
''')
        cls.binary=root/'adapter-test'
        subprocess.run(['c++','-std=c++17','-UNDEBUG','-I'+str(root),'-I'+str(ROOT/'src'),
            str(root/'main.cpp'),str(ROOT/'src/retail_handoff.cpp'),'-o',str(cls.binary)],check=True,timeout=30)

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.env={k:v for k,v in os.environ.items() if not k.startswith(('MAG_HANDOFF_','MAG_ENGINE_'))}
        self.request=self.root/'request.json'
        self.env.update(MAG_HANDOFF_REQUEST=str(self.request),MAG_HANDOFF_TOKEN='0123456789abcdef'*2,
            MAG_ENGINE_KEY_SHA256=hashlib.sha256(bytes(range(16))).hexdigest())
        for name in ('BINARY','ELF','SELF'):
            path=self.root/name;data=('synthetic '+name).encode();path.write_bytes(data)
            self.env['MAG_ENGINE_'+name]=str(path)
            self.env['MAG_ENGINE_'+name+'_SHA256']=hashlib.sha256(data).hexdigest()

    def run_adapter(self,mode,expected):
        result=subprocess.run([str(self.binary),mode],env=self.env,capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
        return result

    def test_handlers_are_disabled_without_complete_environment(self):
        del self.env['MAG_HANDOFF_REQUEST']
        self.run_adapter('disabled',0)

    def test_availability_validates_without_creating_request(self):
        self.run_adapter('available',0)
        self.assertFalse(self.request.exists())

    def test_full_guest_vm_size_sentinel_accepts_valid_low_memory(self):
        self.run_adapter('full-vm',0)
        self.assertFalse(self.request.exists())

    def test_spawn_exits_with_atomic_exact_request(self):
        for mode in ('spawn','null-argv'):
            with self.subTest(mode=mode):
                self.run_adapter(mode,90)
                self.assertEqual(json.loads(self.request.read_text()),dict(version=1,
                    token=self.env['MAG_HANDOFF_TOKEN'],guest_path=GUEST_PATH,kind='mage_g'))
                self.assertEqual(self.request.stat().st_mode&0o777,0o600)
                self.assertFalse(list(self.root.glob('request.json.tmp.*')))
                self.request.unlink()

    def test_hash_mismatch_is_rejected_for_every_prepared_file(self):
        for name in ('BINARY','ELF','SELF'):
            key='MAG_ENGINE_'+name+'_SHA256';old=self.env[key];self.env[key]='0'*64
            with self.subTest(name=name): self.run_adapter('available',89)
            self.env[key]=old
        self.assertFalse(self.request.exists())

    def test_every_availability_rechecks_file_contents(self):
        self.run_adapter('changed-file',89)
        self.assertFalse(self.request.exists())

    def test_spawn_rechecks_mapping_after_successful_availability(self):
        self.run_adapter('changed-before-spawn',89)
        self.assertFalse(self.request.exists())

    def test_invalid_guest_memory_and_unsupported_spawn_are_rejected(self):
        for mode in ('bad-key','bad-path','null-key','key-oob','wide-pointer','path-oob',
                     'unterminated','full-vm-oob','full-vm-crossing','null-vm','argv-oob','nonempty-argv',
                     'envp','data','data-size','priority','flags'):
            with self.subTest(mode=mode): self.run_adapter(mode,89)
        self.assertFalse(self.request.exists())

    def test_existing_request_is_never_replaced(self):
        self.request.write_text('previous')
        self.run_adapter('spawn',89)
        self.assertEqual(self.request.read_text(),'previous')
        self.assertFalse(list(self.root.glob('request.json.tmp.*')))

    def test_invalid_token_or_relative_path_is_rejected(self):
        self.env['MAG_HANDOFF_TOKEN']='invalid'
        self.run_adapter('spawn',89)
        self.env['MAG_HANDOFF_TOKEN']='0'*32
        self.env['MAG_ENGINE_ELF']='relative.elf'
        self.run_adapter('spawn',89)
        self.assertFalse(self.request.exists())
