#include "ppu_recomp.h"
#include "v212_engine_hooks.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

static std::vector<uint8_t> memory(0x1700000);
uint8_t* vm_base=memory.data();
uint32_t ppu_vm_size=0x1700000;
static std::map<uint32_t,void(*)(ppu_context*)> callbacks;
uint8_t vm_read8(uint64_t a){return memory.at(a);}
uint32_t vm_read32(uint64_t a){return uint32_t(vm_read8(a))<<24|uint32_t(vm_read8(a+1))<<16|uint32_t(vm_read8(a+2))<<8|vm_read8(a+3);}
void vm_write8(uint64_t a,uint8_t b){memory.at(a)=b;}
void vm_write32(uint64_t a,uint32_t b){for(int i=0;i<4;++i)vm_write8(a+i,b>>(24-i*8));}
void ppu_register_function(uint64_t a,void(*f)(ppu_context*)){callbacks[a]=f;}
static unsigned setter_calls=0, reader_calls=0, connect_calls=0;
static bool setter_failure=false;
static const char* mode;
constexpr unsigned cfg=0x3028,name=cfg+0x4c,obj=0x8000;
void configured_identity(){
    const char* selected="FixtureUser";
    for(unsigned i=0;i<=11;++i)vm_write8(name+8+i,selected[i]);
    vm_write32(name+0x18,11);
}
void func_00449F88(ppu_context* c){
    ++reader_calls;c->gpr[3]=!std::strcmp(mode,"unhandled")?0:7;
    if(reader_calls==2){
        if(!std::strcmp(mode,"repeat-locked"))vm_write8(cfg+0xdd,1);
        if(!std::strcmp(mode,"repeat-length"))vm_write32(name+0x18,4);
        if(!std::strcmp(mode,"repeat-capacity"))vm_write32(name+0x1c,31);
        if(!std::strcmp(mode,"repeat-identity"))vm_write8(name+8,'X');
    }
}
void func_00109FA8(ppu_context* c){
    ++setter_calls;assert(c->gpr[2]==0x15eca10);
    if(setter_failure)return;
    auto dest=uint32_t(c->gpr[3])+0x4c,source=uint32_t(c->gpr[4]);
    auto len=vm_read32(source+0x18);
    for(unsigned i=0;i<=len;++i)vm_write8(dest+8+i,vm_read8(source+8+i));
    vm_write32(dest+0x18,len);c->gpr[3]=0xdeadbeef;
}
void func_00156CB4(ppu_context* c){
    ++connect_calls;
    assert(c->gpr[1]==0x7000&&c->gpr[2]==0x15eca10&&c->gpr[3]==obj);
    assert(c->gpr[4]==0x9000&&c->gpr[5]==10073&&c->lr==0xABC00004);
    c->gpr[3]=0x12345678;
}
int main(int argc,char**argv){
    assert(argc==2);mode=argv[1];
    unsetenv("MAG_LOCAL_NO_TICKET");unsetenv("MAG_LOCAL_ACCOUNT_NAME");
    unsetenv("MAG_TRACE_NETWORK");unsetenv("PS3RECOMP_NP_TICKET_FILE");
    if(!std::strcmp(mode,"disabled")){mag_v212_engine_register();assert(callbacks.empty());return 0;}
    if(!std::strcmp(mode,"trace-only"))setenv("MAG_TRACE_NETWORK","1",1);
    else {setenv("MAG_LOCAL_NO_TICKET","1",1);setenv("MAG_LOCAL_ACCOUNT_NAME","FixtureUser",1);}
    vm_write32(0x449f88,0x81221780);vm_write32(0x1093bc,0x3863004c);
    vm_write32(0x109fa8,0xf821ff51);vm_write32(0x109fb8,0x880300dd);
    vm_write32(0x109410,0x886300d5);vm_write32(0x152660,0x987f2289);
    vm_write32(0xc26ab0,0x88632289);vm_write32(0x15b9fa8,0x449f88);
    vm_write32(0x15b9fac,0x15fc994);vm_write32(0x163c178,0x3000);
    vm_write32(0x156cb4,0xf821fe11);vm_write32(0x15a1e68,0x156cb4);
    vm_write32(0x15a1e6c,0x15eca10);vm_write32(0x153ea48,0x15a1e68);
    vm_write32(name+0x1c,15);vm_write8(obj+0x2289,1);
    if(!std::strcmp(mode,"guard"))vm_write32(0x1093bc,0x3863006c);
    if(!std::strcmp(mode,"opd-guard"))vm_write32(0x15b9fa8,0x153d0);
    if(!std::strcmp(mode,"connect-guard"))vm_write32(0x156cb4,0);
    if(!std::strcmp(mode,"connect-opd-guard"))vm_write32(0x15a1e68,0);
    if(!std::strcmp(mode,"connect-vtable-guard"))vm_write32(0x153ea48,0);
    if(!std::strcmp(mode,"ticket-file"))setenv("PS3RECOMP_NP_TICKET_FILE","fixture",1);
    if(!std::strcmp(mode,"ticket-config")||!std::strcmp(mode,"settings-ticket-only"))vm_write8(cfg+0xd5,1);
    if(!std::strcmp(mode,"locked"))vm_write8(cfg+0xdd,1);
    if(!std::strcmp(mode,"identity-conflict")||!std::strcmp(mode,"settings-conflict-only"))vm_write32(name+0x18,4);
    if(!std::strcmp(mode,"out-of-bounds"))vm_write32(0x163c178,0xfffffff0);
    if(!std::strcmp(mode,"invalid-name"))setenv("MAG_LOCAL_ACCOUNT_NAME","invalid name",1);
    if(!std::strcmp(mode,"setter-failure"))setter_failure=true;
    if(!std::strcmp(mode,"account-without-local"))unsetenv("MAG_LOCAL_NO_TICKET");
    if(!std::strcmp(mode,"implicit-local"))setenv("MAG_LOCAL_NO_TICKET","0",1);
    if(!std::strcmp(mode,"capacity"))vm_write32(name+0x1c,16);
    if(!std::strcmp(mode,"configured-name")||!std::strncmp(mode,"repeat-",7))configured_identity();
    mag_v212_engine_register();assert(callbacks.size()==2&&callbacks.count(0x449f88)&&callbacks.count(0x156cb4));
    ppu_context c;c.lr=0xABC00004;c.gpr[1]=0x7000;c.gpr[2]=0x15fc994;c.gpr[3]=obj;
    callbacks.at(0x449f88)(&c);
    assert(setter_calls==0&&reader_calls==1&&c.gpr[1]==0x7000&&c.gpr[2]==0x15fc994);
    assert(c.gpr[3]==(!std::strcmp(mode,"unhandled")?0:7));
    if(!std::strcmp(mode,"settings-conflict-only")||!std::strcmp(mode,"settings-ticket-only"))return 0;
    if(!std::strcmp(mode,"repeat")||!std::strncmp(mode,"repeat-",7)){
        c.gpr[3]=obj;callbacks.at(0x449f88)(&c);assert(setter_calls==0&&reader_calls==2);
    }
    c.gpr[2]=0x15eca10;c.gpr[3]=obj;c.gpr[4]=0x9000;c.gpr[5]=10073;
    if(!std::strcmp(mode,"bad-stack"))c.gpr[1]=0x240;
    callbacks.at(vm_read32(0x15a1e68))(&c);
    assert(connect_calls==1&&c.gpr[3]==0x12345678);
    assert(vm_read8(obj+0x2289)==1); // No connection/NP state rewrite.
    if(!std::strcmp(mode,"trace-only")){assert(setter_calls==0);return 0;}
    assert(setter_calls==(!std::strcmp(mode,"configured-name")?0:1));
    assert(vm_read32(name+0x18)==11);
}
