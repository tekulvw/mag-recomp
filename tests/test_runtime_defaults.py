"""Fresh launches use the pinned SDK baseline; explicit diagnostic overrides survive."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
KEYS=('SPU_DISPATCH_CACHE','SPU_MFC_LOOKUP_CACHE','SPU_FLOAT_RZ','PS3_VERBOSE')

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
for (auto k : {"SPU_DISPATCH_CACHE", "SPU_MFC_LOOKUP_CACHE", "SPU_FLOAT_RZ", "PS3_VERBOSE"})
std::printf("%s=%s\\n",k,std::getenv(k)); }
''')
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(source),'-o',str(tmp/'test')],check=True,capture_output=True,text=True)
            base={k:v for k,v in os.environ.items() if k not in KEYS}
            result=subprocess.run([str(tmp/'test')],env=base,check=True,capture_output=True,text=True)
            self.assertEqual(result.stdout.splitlines(),['SPU_DISPATCH_CACHE=1','SPU_MFC_LOOKUP_CACHE=1','SPU_FLOAT_RZ=1','PS3_VERBOSE=0'])
            result=subprocess.run([str(tmp/'test')],env={**base,'SPU_DISPATCH_CACHE':'shadow','SPU_MFC_LOOKUP_CACHE':'0','SPU_FLOAT_RZ':'0','PS3_VERBOSE':'1'},check=True,capture_output=True,text=True)
            self.assertEqual(result.stdout.splitlines(),['SPU_DISPATCH_CACHE=shadow','SPU_MFC_LOOKUP_CACHE=0','SPU_FLOAT_RZ=0','PS3_VERBOSE=1'])

if __name__=='__main__': unittest.main()
