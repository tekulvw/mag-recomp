#include "ppu_recomp.h"
#include "v212_launcher_hooks.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
static std::vector<uint8_t> memory(0x500000);
uint8_t* vm_base=memory.data();
uint32_t ppu_vm_size=0x500000;
static std::map<uint32_t,void(*)(ppu_context*)> callbacks;
uint8_t vm_read8(uint64_t a){return memory.at(a);}
uint32_t vm_read32(uint64_t a){return uint32_t(vm_read8(a))<<24|uint32_t(vm_read8(a+1))<<16|uint32_t(vm_read8(a+2))<<8|vm_read8(a+3);}
void vm_write8(uint64_t a,uint8_t b){memory.at(a)=b;}
void vm_write32(uint64_t a,uint32_t b){for(int i=0;i<4;++i)vm_write8(a+i,b>>(24-i*8));}
void ppu_register_function(uint64_t a,void(*f)(ppu_context*)){callbacks[a]=f;}
static unsigned setter_calls=0;
void func_000153D0(ppu_context*){}
void func_00011088(ppu_context*){}
void func_0026FE00(ppu_context* c){c->gpr[3]=vm_read8(uint32_t(c->gpr[3])+0x2289);}
void func_000594C8(ppu_context* c){
    ++setter_calls;
    auto dest=uint32_t(c->gpr[3])+0x4c,source=uint32_t(c->gpr[4]);
    auto len=vm_read32(source+0x18);
    for(unsigned i=0;i<=len;++i)vm_write8(dest+8+i,vm_read8(source+8+i));
    vm_write32(dest+0x18,len);
}
int main(int argc,char**argv){
    assert(argc==2);const char* mode=argv[1];
    unsetenv("MAG_LOCAL_NO_TICKET");unsetenv("MAG_LOCAL_ACCOUNT_NAME");unsetenv("MAG_TRACE_NETWORK");unsetenv("PS3RECOMP_NP_TICKET_FILE");
    if(!std::strcmp(mode,"disabled")){mag_v212_launcher_register();assert(callbacks.empty());return 0;}
    setenv("MAG_LOCAL_NO_TICKET","1",1);setenv("MAG_LOCAL_ACCOUNT_NAME","FixtureUser",1);
    vm_write32(0x13780,0x38000001);vm_write32(0x13784,0x98092289);
    vm_write32(0x58bb4,0x3863004c);vm_write32(0x26fe00,0x88632289);vm_write32(0x594c8,0xf821ff51);
    vm_write32(0x428fe4,0x3000);unsigned config=0x3028,obj=0x8000;
    vm_write32(config+0x4c+0x1c,15);vm_write8(obj+0x2289,1);
    if(!std::strcmp(mode,"guard"))vm_write32(0x13784,0x98092280);
    if(!std::strcmp(mode,"ticket-conflict"))setenv("PS3RECOMP_NP_TICKET_FILE","fixture",1);
    if(!std::strcmp(mode,"locked"))vm_write8(config+0xdd,1);
    if(!std::strcmp(mode,"identity-conflict"))vm_write32(config+0x4c+0x18,4);
    if(!std::strcmp(mode,"out-of-bounds"))vm_write32(0x428fe4,0xfffffff0);
    mag_v212_launcher_register();assert(callbacks.count(0x26fe00));
    ppu_context c;c.gpr[1]=0x7000;c.gpr[3]=obj;callbacks.at(0x26fe00)(&c);
    assert(c.gpr[3]==0);assert(setter_calls==1);
    assert(vm_read32(config+0x4c+0x18)==11);
    if(!std::strcmp(mode,"repeat")){c.gpr[3]=obj;callbacks.at(0x26fe00)(&c);assert(setter_calls==1);}
}
