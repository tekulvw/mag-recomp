"""Synthetic checks for the Linux-only pinned diagnostic API compatibility."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform=='linux','Linux ELF load bias and monotonic clock')
class LinuxCompatTests(unittest.TestCase):
    def test_clock_and_actual_executable_bias_for_pie_and_exec(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'probe.c'
            source.write_text(r'''
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <assert.h>
#include <string.h>
int main(int argc,char** argv) {
    uint64_t before=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    struct timespec pause={0,2000000};assert(nanosleep(&pause,0)==0);
    uint64_t after=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    assert(after>before);
    intptr_t bias=_dyld_get_image_vmaddr_slide(0);
    if(argc>1&&!strcmp(argv[1],"bad-index")){_dyld_get_image_vmaddr_slide(1);return 9;}
#ifdef EXPECT_PIE
    Dl_info info;assert(dladdr((void*)&main,&info));
    assert(bias!=0 && bias==(intptr_t)info.dli_fbase);
#else
    assert(bias==0);
#endif
    return 0;
}
''')
            for name,flags in [('pie',['-fPIE','-pie','-DEXPECT_PIE']),('exec',['-fno-pie','-no-pie'])]:
                output=root/name
                subprocess.run(['cc','-std=c11','-Wall','-Werror',
                    '-I'+str(ROOT/'src/linux/compat'),'-include',str(ROOT/'src/linux/compat/mag_linux_compat.h'),
                    str(source),'-o',str(output),'-ldl',*flags],check=True,capture_output=True,timeout=30)
                subprocess.run([str(output)],check=True,capture_output=True,timeout=5)
                if name=='pie':
                    bad=subprocess.run([str(output),'bad-index'],capture_output=True,text=True,timeout=5)
                    self.assertNotEqual(bad.returncode,0)
                    self.assertIn('load bias unavailable',bad.stderr)
