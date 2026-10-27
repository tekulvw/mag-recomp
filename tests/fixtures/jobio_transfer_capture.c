/* Neutral recorder-interface fixture; no title code or guest assets. */
#include "diagnostics/job_io_recorder.h"
#include "spu/spu_context.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static spu_context a,b;
static unsigned char data[SPU_LS_SIZE],other[SPU_LS_SIZE];
int main(int argc,char**argv){
 assert(argc==3);a.ls=data;b.ls=other;a.pc=0x17a30;data[7]=12;
 assert(!ps3_jobio_begin(&a,0x17a30,argv[1]));assert(ps3_jobio_begin(&b,1,argv[1])==2);
 unsigned char block[128]={0};const unsigned repeats=!strcmp(argv[2],"transfers")?800000:3;
 for(unsigned i=0;i<repeats;i++)ps3_jobio_event(&a,PS3_JOBIO_GETLLAR,0xd0,128,0x1000,128,block,0);
 ps3_jobio_event(&a,PS3_JOBIO_PUTLLC,0xb4,128,0x1000,128,block,1);
 ps3_jobio_event(&a,PS3_JOBIO_PUTLLUC,0xb0,128,0x1000,128,block,0);
 ps3_jobio_event(&b,PS3_JOBIO_GETLLAR,0xd0,0,0,128,block,0);
 unsigned char get[4]={1,2,3,4},put[2]={5,6};
 ps3_jobio_event(&a,PS3_JOBIO_GET,0x40,64,0x2000,4,get,0);
 ps3_jobio_event(&a,PS3_JOBIO_GET,0x44,68,0x3000,4,get,0);
 ps3_jobio_event(&a,PS3_JOBIO_PUT,0x20,72,0x4000,2,put,0);
 ps3_jobio_event(&a,PS3_JOBIO_ZERO,0x2000,80,0x5000,128,0,0);
 assert(ps3_jobio_end(&b)==-1);data[7]=23;assert(!ps3_jobio_end(&a));
 ps3_jobio_event(&a,PS3_JOBIO_GET,0x40,0,0,4,get,0);
 puts("PASS neutral recorder fixture");
}
