/* Opt-in MAG-only observation around the unchanged SDK prepare operation.
 * Both arms use this wrapper. CPU time is a sum of prepare executions, not
 * frame wall time; event62 contains wall interval, CPU ns, refs and attributes.
 */
#include "libs/video/rsx_vertex_compact.h"
#include "diagnostics/critical_trace.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
static pthread_once_t mode_once=PTHREAD_ONCE_INIT;
static int mode;
static void init_mode(void){const char* v=getenv("MAG_RSX_PREPARE_METRICS");mode=v&&strcmp(v,"1")==0;}
void mag_sdk_vertex_fetch_plan_prepare(rsx_vertex_fetch_plan*,const rsx_vertex_ref*,u32);
static uint64_t cpu_now(void){struct timespec t;if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t))return 0;return(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
void rsx_vertex_fetch_plan_prepare(rsx_vertex_fetch_plan* p,const rsx_vertex_ref* r,u32 n){
 pthread_once(&mode_once,init_mode);
 if(!mode||!ps3_critical_enabled()){mag_sdk_vertex_fetch_plan_prepare(p,r,n);return;}
 const uint64_t begin=ps3_critical_now_ns(),cpu=cpu_now();
 mag_sdk_vertex_fetch_plan_prepare(p,r,n);
 const uint64_t end_cpu=cpu_now(),end=ps3_critical_now_ns();
 const uint64_t words[]={begin,end,end_cpu>=cpu?end_cpu-cpu:0,n,p?p->layout.count:0,p?!!p->inline_data:0};
 ps3_critical_emit(62,0,0,words,6);
}
