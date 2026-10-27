/* Synthetic-data integration fixture. No title PPU code, game assets, or
 * replacement queue/completion logic. Firmware and EDGE objects are external. */
#include "ppu_recomp.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>
void mag_sre_func_21017134(ppu_context*);
void mag_sre_func_210171B8(ppu_context*);
void mag_sre_func_210174F8(ppu_context*);

extern "C" {
uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void ppu_hle_init(void);
void ppu_sysprx_register(void);
void lv2_init_syscalls(void);
void mag_load_queue_firmware(void);
int32_t cellSpursInitialize(void*, int32_t, int32_t, int32_t, uint8_t);
int32_t cellSpursCreateTaskset(void*, void*, uint64_t, const uint8_t*, uint32_t);
int32_t cellSpursCreateTask(void*, void*, void*, void*, uint32_t, uint32_t, uint32_t);
int32_t cellSpursEventFlagInitialize(void*, void*, uint32_t, uint32_t);
int32_t cellSpursEventFlagWait(void*, void*, uint32_t);
}
/* The PPU loader's diagnostic resolver needs a table; this fixture has no
 * generated title PPU functions. The real firmware has its own registry. */
const func_entry function_table[] = {{0, nullptr, "fixture"}};
const uint64_t function_table_count = 0;

static constexpr uint32_t SPURS=0x100000, TASKSET=0x110000, FLAG=0x120000;
static constexpr uint32_t QUEUE=0x120080, RING=0x120100, ARGS=0x130000;
static constexpr uint32_t CONTEXT=0x140000, ELF=0x180000;
/* Retail's adjacent 44-byte slot records put both counters on one reservation
 * line. Preserve that contention rather than separating the producer counters. */
static constexpr uint32_t PENDING=0x15002C, SLOT_STRIDE=44;
static constexpr unsigned JOBS=48, SIZE=4096;
static bool corrupt;
static std::atomic<unsigned> submitted{0}, checked{0};
static void* ea(uint32_t n) { return reinterpret_cast<void*>(uintptr_t(n)); }
static void require(bool ok, const char* what, unsigned slot=99, unsigned job=99) {
    if (ok) return;
    if(!vm_base || vm_base==MAP_FAILED) {
        fprintf(stderr,"EDGE_QUEUE_FAIL %s before guest VM initialization\n",what);
        _Exit(1);
    }
    fprintf(stderr,"EDGE_QUEUE_FAIL %s slot=%u job=%u submitted=%u checked=%u "
        "pending=%08X,%08X flag=%04X head=%016llX tail=%016llX\n",
        what,slot,job,submitted.load(),checked.load(),
        vm_read32(PENDING),vm_read32(PENDING+SLOT_STRIDE),vm_read16(FLAG),
        (unsigned long long)vm_read64(QUEUE),(unsigned long long)vm_read64(QUEUE+8));
    fflush(stderr); _Exit(1);
}
static void call(ppu_context& c, void (*fn)(ppu_context*),
                 uint64_t a, uint64_t b=0, uint64_t d=0, uint64_t e=0,
                 uint64_t f=0, uint64_t g=0) {
    c.gpr[2]=0x21039AB0;
    c.gpr[3]=a;c.gpr[4]=b;c.gpr[5]=d;c.gpr[6]=e;c.gpr[7]=f;c.gpr[8]=g;
    fn(&c);
    if(uint32_t(c.gpr[3])) fprintf(stderr,"firmware function=%p rc=%08X\n",(void*)fn,uint32_t(c.gpr[3]));
    require(uint32_t(c.gpr[3])==0,"firmware return");
}
static void producer(unsigned slot) {
    ppu_context c{}; c.gpr[1]=0x400000+slot*0x10000;
    const uint32_t input=0x200000+slot*0x20000, output=input+0x10000;
    const uint32_t descriptor=0x160000+slot*0x80, pending=PENDING+slot*SLOT_STRIDE;
    const uint32_t maskptr=descriptor+0x40; const uint16_t mask=uint16_t(1u<<slot);
    uint8_t plain[SIZE];
    for(unsigned job=0;job<JOBS;++job) {
        for(unsigned i=0;i<SIZE;++i) plain[i]=uint8_t((i*17+(i>>4)+job*31+slot*101)&255);
        uLongf bytes=0x8000;
        require(compress2(vm_base+input,&bytes,plain,SIZE,1)==Z_OK,"synthetic compression");
        memset(vm_base+output,0xA5,SIZE+128);
        memset(vm_base+descriptor,0,32);
        vm_write32(descriptor,input+2);vm_write32(descriptor+4,output);
        vm_write32(descriptor+8,uint32_t(bytes-2));vm_write32(descriptor+12,SIZE);
        vm_write32(descriptor+16,pending|1);vm_write32(descriptor+20,FLAG);
        vm_write16(descriptor+24,mask);vm_write32(pending,1);
        ++submitted;
        call(c,mag_sre_func_210171B8,QUEUE,descriptor,1);
        vm_write16(maskptr,mask);
        require(cellSpursEventFlagWait(ea(FLAG),ea(maskptr),1)==0,"event wait",slot,job);
        require(vm_read16(maskptr)==mask,"completion mask",slot,job);
        require(vm_read32(pending)==0,"pending counter",slot,job);
        if(corrupt && slot==0 && job==0) vm_base[output]^=1;
        require(memcmp(vm_base+output,plain,SIZE)==0,"decoded bytes",slot,job);
        for(unsigned i=SIZE;i<SIZE+128;++i)
            require(vm_base[output+i]==0xA5,"output canary",slot,job);
        ++checked;
        if(job%8==0) usleep(1000); // also exercise enqueue after the consumer parks
    }
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    require(argc==3,"arguments");corrupt=strcmp(argv[2],"corrupt")==0;
    alarm(12);
    vm_base=(uint8_t*)mmap(nullptr,0x100010000ULL,PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANON,-1,0);
    if(vm_base==MAP_FAILED) return 2;
    ppu_vm_size=0;
    FILE* f=fopen(argv[1],"rb");require(f!=nullptr,"EDGE ELF open");
    require(fread(vm_base+ELF,1,38544,f)==38544,"EDGE ELF read");fclose(f);
    ppu_hle_init();ppu_sysprx_register();lv2_init_syscalls();mag_load_queue_firmware();
    require(cellSpursInitialize(ea(SPURS),1,100,100,0)==0,"SPURS initialize");
    require(cellSpursCreateTaskset(ea(SPURS),ea(TASKSET),0,nullptr,1)==0,"taskset create");
    require(cellSpursEventFlagInitialize(nullptr,ea(FLAG),0,1)==0,"event initialize");
    ppu_context c{};c.gpr[1]=0x300000;
    call(c,mag_sre_func_21017134,SPURS|1,QUEUE,RING,32,12,3);
    call(c,mag_sre_func_210174F8,QUEUE);
    vm_write32(ARGS,QUEUE);vm_write32(ARGS+4,SPURS);vm_write32(ARGS+12,FLAG);
    vm_write32(ARGS+28,1);
    require(cellSpursCreateTask(ea(TASKSET),ea(ARGS+32),ea(ELF),ea(CONTEXT),0x1000,
                               ARGS+16,ARGS)==0,"task create");
    std::thread one(producer,0),two(producer,1);one.join();two.join();
    require(checked==96,"all jobs checked");
    puts("EDGE_QUEUE_PASS jobs=96 producers=2 bytes=393216 counters=0 masks=1,2 wraps=8");
    fflush(stdout);
    /* Persistent retail worker has no fixture shutdown descriptor. Process
     * isolation is intentional; no claim is made about task teardown. */
    _Exit(0);
}
