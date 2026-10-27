#include "mag_timer_gcm_pump_diagnostics.h"
#include "runtime/diagnostics/timer_gcm_pump.h"
#include "runtime/diagnostics/critical_trace.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
std::once_flag once;bool configured;int status;std::atomic<bool> armed{};
std::string arm_path,receipt_path,nonce;
struct ErrnoGuard {int value{errno};~ErrnoGuard(){errno=value;}};
bool absolute(const char*p){return p&&p[0]=='/'&&std::strlen(p)<=3900;}
void configure(){
 const char*f=std::getenv("MAG_TIMER_GCM_PUMP");if(!f||!*f||!std::strcmp(f,"0"))return;
 const char*a=std::getenv("MAG_TIMER_GCM_PUMP_ARM_FILE"),*n=std::getenv("MAG_TIMER_GCM_PUMP_NONCE"),*r=std::getenv("MAG_TIMER_GCM_PUMP_RECEIPT");
 if(std::strcmp(f,"1")||!absolute(a)||!n||!*n||std::strlen(n)>128||(r&&(!absolute(r)||!std::strcmp(a,r)))){status=-1;std::fputs("[timer-GCM-pump] invalid diagnostic configuration; disabled\n",stderr);return;}
 for(const char*p=n;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-')){status=-1;return;}
 arm_path=a;nonce=n;if(r)receipt_path=r;configured=true;ppu_timer_gcm_pump_set_enabled(0);
}
void receipt(){
 if(receipt_path.empty())return;const auto temp=receipt_path+".tmp."+std::to_string(getpid());FILE*f=std::fopen(temp.c_str(),"w");if(!f)return;
 std::fprintf(f,"{\"pid\":%d,\"status\":0,\"configured\":1,\"active\":1,\"nonce\":\"%s\",\"armed_ns\":%llu,\"meaning\":\"PR353 timer141/142 existing GCM pump invocation only; no completion bypass\"}\n",getpid(),nonce.c_str(),(unsigned long long)ps3_critical_now_ns());if(std::fclose(f)==0)std::rename(temp.c_str(),receipt_path.c_str());
}
}
int mag_timer_gcm_pump_diagnostics_init(){ErrnoGuard e;std::call_once(once,configure);return status;}
int mag_timer_gcm_pump_diagnostics_poll_arm(){
 ErrnoGuard e;if(mag_timer_gcm_pump_diagnostics_init()<0)return -1;if(!configured)return 0;if(armed.load(std::memory_order_acquire))return 1;
 const int fd=open(arm_path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;struct stat st{};char b[132]{};const bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<(off_t)sizeof b;const ssize_t n=ok?read(fd,b,sizeof b-1):-1;close(fd);if(n<=0)return 0;size_t end=(size_t)n;while(end&&(b[end-1]=='\n'||b[end-1]=='\r'))--end;if(end!=nonce.size()||std::memcmp(b,nonce.data(),end))return 0;
 ppu_timer_gcm_pump_set_enabled(1);armed.store(true,std::memory_order_release);receipt();const uint64_t w[]={1};ps3_critical_emit(97,0,0,w,1);std::fprintf(stderr,"[timer-GCM-pump] ARMED pid=%d nonce=%s pump_invocations_not_callback_deliveries=1\n",getpid(),nonce.c_str());return 1;
}
void mag_timer_gcm_pump_diagnostics_snapshot(){
 ErrnoGuard e;if(!configured||!ps3_critical_enabled())return;PpuTimerGcmPumpSnapshot s{};ppu_timer_gcm_pump_get_snapshot(&s);const uint64_t w[]={configured,s.enabled,s.eligible_141,s.eligible_142,s.pump_invocations,s.missing_pump,s.disabled_eligible,s.setter_transitions};ps3_critical_emit(96,0,0,w,8);
}
