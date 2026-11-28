"""Synthetic execution of the narrow original-PRX lifecycle adapter."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PPU = '''#pragma once
#include <cstdint>
struct ppu_context { uint64_t gpr[32]{}; uint64_t lr=0,ctr=0; };
extern "C" uint8_t vm_read8(uint64_t);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" void vm_write32(uint64_t,uint32_t);
'''
PRX = '''#pragma once
#include <cstdint>
struct prx_func_entry { uint64_t addr; void (*func)(void*); const char* name; };
struct prx_export { uint32_t nid,vaddr; };
struct prx_module { const char* name; uint32_t base; const uint8_t* image; uint32_t image_size;
 const prx_func_entry* funcs; uint64_t func_count; const prx_export* exports; uint32_t export_count; };
struct prx_load_result { int ok; uint32_t funcs_registered,base,image_size; };
using prx_register_fn=void(*)(uint32_t,void(*)(void*));
extern "C" prx_load_result prx_load_module(const prx_module*,prx_register_fn);
'''
FIXTURE = r'''
#include "ppu_recomp.h"
#include "v212_pace.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
extern "C" { uint8_t* vm_base=reinterpret_cast<uint8_t*>(1); uint32_t ppu_vm_size=0x30000000; }
static std::map<uint32_t,void(*)(ppu_context*)> handlers,functions;
static std::map<uint64_t,uint8_t> bytes;
static unsigned loaded=0,started=0,stopped=0;
extern "C" uint8_t vm_read8(uint64_t a) { return bytes[a]; }
extern "C" uint32_t vm_read32(uint64_t a) { return (uint32_t(bytes[a])<<24)|(uint32_t(bytes[a+1])<<16)|(uint32_t(bytes[a+2])<<8)|bytes[a+3]; }
extern "C" void vm_write32(uint64_t a,uint32_t v) { for(unsigned i=0;i<4;++i) bytes[a+i]=uint8_t(v>>(24-8*i)); }
extern "C" void ps3_hle_register_ctx(uint32_t n,const char*,void(*fn)(ppu_context*)) { handlers[n]=fn; }
extern "C" void ppu_register_function(uint64_t ea,void(*fn)(ppu_context*)) { functions[uint32_t(ea)]=fn; }
extern "C" void ps3_indirect_call(ppu_context* c) { assert(functions.count(uint32_t(c->ctr))); functions[uint32_t(c->ctr)](c); }
static void start(void* p) { auto*c=static_cast<ppu_context*>(p); assert(c->gpr[3]==0&&c->gpr[4]==0); assert(c->gpr[2]==0x21200080); assert(c->gpr[1]<0x100000); vm_write32(c->gpr[1]+0x28,0xdeadbeef); ++started; c->gpr[3]=0; }
static void stop(void* p) { ++stopped; static_cast<ppu_context*>(p)->gpr[3]=0; }
extern "C" prx_load_result prx_load_module(const prx_module* m,prx_register_fn reg) {
 ++loaded; assert(m->base==0x21200000&&m->export_count==1);
 for(unsigned i=0;i<m->func_count;++i) reg(uint32_t(m->funcs[i].addr),m->funcs[i].func);
 vm_write32(0x21200040,0x21200020); vm_write32(0x21200044,0x21200080);
 vm_write32(0x21200048,0x21200024); vm_write32(0x2120004c,0x21200080);
 return {1,2,m->base,m->image_size};
}
extern "C" const MagV212PaceModule* mag_v212_pace_prepared_module() {
 static const uint8_t image[0x100]{};
 static const prx_func_entry funcs[]={{0x21200020,start,"start"},{0x21200024,stop,"stop"}};
 static const prx_export exports[]={{0x12345678,0x40}};
 static const prx_module module={"pace",0x21200000,image,sizeof(image),funcs,2,exports,1};
 static const MagV212PaceModule descriptor={&module,"/dev_hdd0/game/BCUS98110/USRDIR/bin/libpace_g.sprx",0x21200040,0x21200048};
 return &descriptor;
}
static void path(const char* s) { for(unsigned i=0;i<=std::strlen(s);++i)bytes[0x1000+i]=s[i]; }
int main() {
 mag_v212_pace_register(); mag_v212_pace_register(); assert(handlers.size()==4);
 ppu_context c{}; c.gpr[1]=0x100000;c.gpr[2]=0x345600;c.lr=0x777;c.ctr=0x888;vm_write32(c.gpr[1]+0x28,0x345600);
 path("/dev_hdd0/game/BCUS98110/USRDIR/bin/libpacePrx_g.sprx");c.gpr[3]=0x1000;
 handlers[0x26090058](&c);assert(uint32_t(c.gpr[3])==0x8001112e&&loaded==0);
 path("/dev_hdd0/game/BCUS98110/USRDIR/bin/libpace_g.sprx");c.gpr[3]=0x1000;
 handlers[0x26090058](&c);auto id=c.gpr[3];assert(id>0&&id<0x80000000&&loaded==1&&started==0);
 c.gpr[3]=id;c.gpr[4]=0;c.gpr[5]=0;c.gpr[6]=0x2000;c.gpr[7]=0;c.gpr[8]=0;
 handlers[0x9f18429d](&c);assert(c.gpr[3]==0&&started==1&&vm_read32(0x2000)==0);
 assert(c.gpr[1]==0x100000&&c.gpr[2]==0x345600&&c.lr==0x777&&c.ctr==0x888&&vm_read32(0x100028)==0x345600);
 c.gpr[3]=id;handlers[0x9f18429d](&c);assert(uint32_t(c.gpr[3])==0x80011133&&started==1);
 c.gpr[3]=id;handlers[0x80fb0c19](&c);assert(c.gpr[3]==0&&stopped==1);
 c.gpr[3]=id;c.gpr[4]=0;c.gpr[5]=0;handlers[0xf0aece0d](&c);
 assert(uint32_t(c.gpr[3])==0x80011138); // Registry has no removal API: never fake unloading.
}
'''


class PaceLifecycleTests(unittest.TestCase):
    def test_original_module_lifecycle_executes_and_unknown_paths_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp)
            (out/'ppu_recomp.h').write_text(PPU)
            (out/'prx_loader.h').write_text(PRX)
            (out/'fixture.cpp').write_text(FIXTURE)
            result=subprocess.run(['/usr/bin/clang++','-std=c++17','-I',str(out),'-I',str(ROOT/'src'),
                                   str(ROOT/'src/v212_pace.cpp'),str(out/'fixture.cpp'),'-o',str(out/'fixture')],
                                  capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(out/'fixture')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__=='__main__':unittest.main()
