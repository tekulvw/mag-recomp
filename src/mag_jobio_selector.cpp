#include "mag_jobio_selector.h"
#include "runtime/diagnostics/job_io_recorder.h"
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
std::once_flag once;std::string path;std::mutex mutex;std::atomic<unsigned> target{};unsigned long long generation{},taken{};
struct Error {int saved{errno};~Error(){errno=saved;}};
void init(){const char*p=std::getenv("MAG_JOB_IO_ENTRY_CONTROL");if(p&&*p=='/'&&std::strlen(p)<3900)path=p;}
}
bool mag_jobio_selector_configured(){Error e;std::call_once(once,init);return !path.empty();}
void mag_jobio_selector_poll(){
 Error e;if(!mag_jobio_selector_configured())return;
 std::lock_guard<std::mutex> lock(mutex);if(__atomic_load_n(&ps3_jobio_active,__ATOMIC_ACQUIRE))return;
 const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;char text[80]{};struct stat st{};const bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<79;const auto n=ok?read(fd,text,79):-1;close(fd);if(n<=0)return;
 unsigned long long seq=0;unsigned entry=0;char extra=0;if(std::sscanf(text,"%llu %x %c",&seq,&entry,&extra)!=2||!seq||seq<=generation||(entry!=0x7748&&entry!=0x7498&&entry!=0x7848))return;
 generation=seq;target.store(entry,std::memory_order_release);
}
bool mag_jobio_selector_begin(void*context,unsigned entry,const char*directory){
 Error e;if(!mag_jobio_selector_configured()||target.load(std::memory_order_acquire)!=entry)return false;
 std::lock_guard<std::mutex>lock(mutex);if(target.load(std::memory_order_relaxed)!=entry||!generation||taken==generation||__atomic_load_n(&ps3_jobio_active,__ATOMIC_ACQUIRE))return false;
 if(ps3_jobio_begin((spu_context*)context,entry,directory))return false;taken=generation;return true;
}
bool mag_jobio_selector_end(void*context){
 Error e;if(!mag_jobio_selector_configured())return false;std::lock_guard<std::mutex>lock(mutex);if(!ps3_jobio_is(context))return false;ps3_jobio_end((spu_context*)context);return true;
}
