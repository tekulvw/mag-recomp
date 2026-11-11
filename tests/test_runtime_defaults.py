"""Fresh launches use the pinned SDK baseline; explicit diagnostic overrides survive."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
DEFAULTS={'SPU_DISPATCH_CACHE':'1','SPU_MFC_LOOKUP_CACHE':'1','SPU_FLOAT_RZ':'1','PS3_VERBOSE':'0',
          'PS3RECOMP_GCM_INLINE_NO_GPU_WAIT':'1','MAG_NATIVE_CONSUMER':'1',
          'PS3RECOMP_RSX_DEFERRED_REPORTS':'1','PS3RECOMP_RSX_DEFERRED_REPORT_BATCH':'256',
          'PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY':'8192',
          'PS3RECOMP_RSX_EARLY_LABEL_RELEASE':'1','PS3RECOMP_RSX_EARLY_FIFO_SYNC':'1','PS3RECOMP_RSX_GPU_DEPTH_VIEW':'1',
          'PS3RECOMP_PPU_STORE_FASTPATH':'1','SPU_REGION_FRAMED':'1','SPU_INTERP_REJOIN_PREFILTER':'1',
          'PS3RECOMP_SPURS_WORKLOAD_STACK_ARG9':'1','PS3RECOMP_GCM_FAST_WALK':'1','PS3RECOMP_SPU_TASK_POOL':'1',
          'PS3RECOMP_COH_VERSIONS':'1'}
LINUX_VULKAN={'PS3RECOMP_VK_BATCHED_ENGINE':'1','PS3RECOMP_VK_MERGE_PASSES':'1','PS3RECOMP_VK_DESCRIPTOR_REUSE':'1',
              'PS3RECOMP_VK_PIN_RELEASE_NOWAIT':'1'}
if sys.platform.startswith('linux'): DEFAULTS.update(LINUX_VULKAN)
KEYS=tuple(DEFAULTS)+('RUST_MIN_STACK',)+tuple(LINUX_VULKAN)

@unittest.skipUnless(shutil.which('clang++'),'clang++ required')
class RuntimeDefaultsTests(unittest.TestCase):
    def test_defaults_and_explicit_overrides(self):
        self.assertTrue((ROOT/'src/mag_runtime_defaults.h').is_file(), 'runtime defaults are missing')
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            source=tmp/'test.cpp'
            source.write_text('''#include "mag_runtime_defaults.h"
#include <cstdio>
int main() { if (mag_apply_runtime_defaults()) return 2;
for (auto k : {'''+','.join(f'"{k}"' for k in DEFAULTS)+'''}) { const char* v=std::getenv(k); std::printf("%s=%s\\n",k,v?v:"<unset>"); } }
''')
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(source),'-o',str(tmp/'test')],check=True,capture_output=True,text=True)
            base={k:v for k,v in os.environ.items() if k not in KEYS}
            result=subprocess.run([str(tmp/'test')],env=base,check=True,capture_output=True,text=True)
            self.assertEqual(result.stdout.splitlines(),[f'{k}={v}' for k,v in DEFAULTS.items()])
            # Every switch can be turned off (or otherwise overridden) from the environment.
            overrides={k:('shadow' if k=='SPU_DISPATCH_CACHE' else '0') for k in DEFAULTS}
            result=subprocess.run([str(tmp/'test')],env={**base,**overrides},check=True,capture_output=True,text=True)
            self.assertEqual(result.stdout.splitlines(),[f'{k}={v}' for k,v in overrides.items()])

    @unittest.skipUnless(shutil.which('clang') and shutil.which('ar'),'clang and ar required')
    def test_defaults_reach_sdk_constructors(self):
        """An SDK constructor that reads its switches before main() sees the defaults."""
        keys=('PS3RECOMP_RSX_DEFERRED_REPORTS','PS3RECOMP_RSX_DEFERRED_REPORT_BATCH','PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY',
              'PS3RECOMP_RSX_EARLY_LABEL_RELEASE','PS3RECOMP_RSX_EARLY_FIFO_SYNC','PS3RECOMP_COH_VERSIONS')
        if sys.platform.startswith('linux'): keys+=('PS3RECOMP_VK_PIN_RELEASE_NOWAIT',)
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            # Stands in for cellGcmSys.c: a default-priority constructor in a static library.
            (tmp/'sdk.c').write_text('#include <stdio.h>\n#include <stdlib.h>\nchar seen[512];\n'
                '__attribute__((constructor)) static void sdk_init(void) { size_t n = 0;\n'
                + ''.join(f'  {{ const char* v = getenv("{k}"); n += snprintf(seen + n, sizeof seen - n, "{k}=%s\\n", v ? v : "<unset>"); }}\n' for k in keys)
                + '}\n')
            (tmp/'main.cpp').write_text('#include <cstdio>\nextern "C" char seen[512];\nint main() { std::fputs(seen, stdout); }\n')
            run=lambda *argv: subprocess.run(list(argv),check=True,capture_output=True,text=True,cwd=tmp)
            run('clang','-c','sdk.c','-o','sdk.o'); run('ar','rcs','libsdk.a','sdk.o')
            run('clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),'-c',
                str(ROOT/'src/mag_runtime_defaults_early.cpp'),'-o','early.o')
            run('clang++','-std=c++17','-c','main.cpp','-o','main.o')
            builds=[['main.o','early.o','libsdk.a']]  # as the game links: its objects, then the SDK library
            if sys.platform.startswith('linux'):
                builds.append(['sdk.o','main.o','early.o'])  # ELF honours the constructor priority in any order
            base={k:v for k,v in os.environ.items() if k not in KEYS}
            for i,objects in enumerate(builds):
                run('clang++',*objects,'-o',f'game{i}')
                result=subprocess.run([str(tmp/f'game{i}')],env=base,check=True,capture_output=True,text=True)
                self.assertEqual(result.stdout.splitlines(),[f'{k}={DEFAULTS[k]}' for k in keys],objects)
                result=subprocess.run([str(tmp/f'game{i}')],env={**base,**{k:'0' for k in keys}},check=True,capture_output=True,text=True)
                self.assertEqual(result.stdout.splitlines(),[f'{k}=0' for k in keys])  # explicit values still win

    @unittest.skipUnless(sys.platform.startswith('linux'),'glibc static TLS is carved from thread stacks only on Linux')
    def test_rust_threads_keep_their_stack_beside_static_tls(self):
        # A large static TLS block, as the runtime has: a Rust-style 2 MiB
        # thread stack would keep almost none of it usable.
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            source=tmp/'test.cpp'
            source.write_text('''#include "mag_runtime_defaults.h"
#include <pthread.h>
// External linkage and volatile access keep the block from being optimized away.
__attribute__((used)) thread_local unsigned char big_tls[3u << 20];
static void* run(void* out) {
    volatile unsigned char frame[6u << 20];
    volatile unsigned char* tls = big_tls;
    frame[0] = 1; frame[sizeof frame - 1] = 2; tls[0] = 3; tls[sizeof big_tls - 1] = 4;
    *(int*)out = frame[0] + frame[sizeof frame - 1] + tls[0] + tls[sizeof big_tls - 1];
    return nullptr;
}
int main() {
    if (mag_apply_runtime_defaults()) return 2;
    const char* value = std::getenv("RUST_MIN_STACK");
    std::printf("RUST_MIN_STACK=%s\\n", value ? value : "");
    if (!value) return 0;
    pthread_attr_t attr; pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, std::strtoull(value, nullptr, 10))) return 3;
    pthread_t t; int sum = 0;
    if (pthread_create(&t, &attr, run, &sum)) return 4;
    pthread_join(t, nullptr);
    return sum == 10 ? 0 : 5;
}
''')
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(source),'-o',str(tmp/'test'),'-pthread','-ldl'],check=True,capture_output=True,text=True)
            base={k:v for k,v in os.environ.items() if k not in KEYS}
            result=subprocess.run([str(tmp/'test')],env=base,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertGreaterEqual(int(result.stdout.split('=',1)[1]),(8<<20)+(3<<20))
            result=subprocess.run([str(tmp/'test')],env={**base,'RUST_MIN_STACK':'123'},capture_output=True,text=True)
            self.assertEqual(result.stdout.strip(),'RUST_MIN_STACK=123')

if __name__=='__main__': unittest.main()
