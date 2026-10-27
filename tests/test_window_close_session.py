"""Run the actual frame_clock and main-queue wrapper with a blocked guest."""
import pathlib,subprocess,tempfile,time,json,sys
ROOT=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else pathlib.Path(__file__).resolve().parents[1]
R=pathlib.Path(tempfile.mkdtemp(prefix='mag-window-close-session-'))
s=(ROOT/'src/main.cpp').read_text();start=s.index('static DWORD WINAPI frame_clock(LPVOID)');end=s.index('\n/* ---------------------------------------------------------------------------',start);clock=s[start:end]
prefix=r'''#include "macos_runloop.h"
#include <dispatch/dispatch.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
using DWORD=unsigned long;using LPVOID=void*;using ULONGLONG=unsigned long long;
#define WINAPI
#define WINDOW_WIDTH 64
#define WINDOW_HEIGHT 64
#define RSX_BACKEND_NAME "fixture"
static int mode;static std::atomic<unsigned> pumps{0};static std::atomic<bool> stderr_locked{false};
static void Sleep(unsigned n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
static ULONGLONG GetTickCount64(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static int rsx_backend_init(int,int,const char*){return mode==2?-1:0;}
static void mag_queue_snapshot(){}static void mag_job_marker_snapshot(){}
static void cellGcmTickVBlank(){}static void cellGcmTickFlip(){}
static int cellGcm_take_flip_pending(){return 0;}
static unsigned cellGcm_flip_request_count(){return 0;}
static void cellGcm_rsx_process_fifo(){}static void present_guest_frame(){}
static void rsx_backend_present(){}
static int rsx_backend_pump(){if(mode==2)std::abort();__block int result=0;dispatch_sync(dispatch_get_main_queue(), ^{unsigned n=++pumps; if(((mode==0||mode==4)&&n>=3)||(mode==6&&stderr_locked.load()))result=mode==4?1:-1;});return result;}
'''
suffix=r'''
static void hang_at_exit(){if(mode==0||mode==4||mode==6){std::fprintf(stderr,"UNSAFE_ATEXIT\n");for(;;)Sleep(10);}}
int main(int argc,char**argv){mode=argc>1?std::atoi(argv[1]):0;std::atexit(hang_at_exit);
std::thread([]{for(;;){std::puts("audio-alive");std::fflush(stdout);Sleep(10);}}).detach();
std::thread([]{frame_clock(nullptr);}).detach();
int result=mag_run_with_main_queue([](void*){dispatch_sync(dispatch_get_main_queue(), ^{std::fprintf(stderr,"guest-main-queue-ready\n");});if(mode==6){while(!pumps.load())Sleep(1);flockfile(stderr);stderr_locked.store(true);}if(mode==1||mode==2||mode==3){Sleep(100);return 37;}for(;;)Sleep(10);return 99;},nullptr);
std::fprintf(stderr,"guest-returned-%d\n",result);return result;}
'''
source=R/'frame_clock_child.cpp';source.write_text(prefix+clock+suffix);binary=R/'frame_clock_child'
cmd=['clang++','-std=c++20','-O2','-fsanitize=undefined','-fno-sanitize-recover=all','-fblocks','-I'+str(ROOT/'src'),str(source),'-framework','CoreFoundation','-o',str(binary)]
q=subprocess.run(cmd,capture_output=True);(R/'child-build.log').write_bytes(q.stdout+q.stderr);assert q.returncode==0,q.stderr.decode()
results=[]
for mode in [0,4,1,2,3,5,6]:
 begin=time.monotonic();p=subprocess.Popen([str(binary),str(mode)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 timedout=False
 try:o,e=p.communicate(timeout=.6)
 except subprocess.TimeoutExpired:timedout=True;p.kill();o,e=p.communicate()
 dt=time.monotonic()-begin;(R/('mode%d.stdout'%mode)).write_bytes(o);(R/('mode%d.stderr'%mode)).write_bytes(e)
 expected=(not timedout and p.returncode==0 and b'UNSAFE_ATEXIT' not in e) if mode in(0,4,6) else (not timedout and p.returncode==37) if mode in(1,2,3) else timedout
 results.append({'mode':mode,'pass':expected,'timedout':timedout,'exit':p.returncode,'elapsed':dt,'audio_heartbeat_lines':o.count(b'audio-alive'),'stderr':e.decode()})
(R/'child-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2));sys.exit(0 if all(x['pass'] for x in results) else 1)
