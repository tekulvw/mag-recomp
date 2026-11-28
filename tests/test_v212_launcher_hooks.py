import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

class LauncherHookTests(unittest.TestCase):
    def test_source_bound_local_profile_uses_real_setter_and_preserves_conflicts(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            (tmp/'ppu_recomp.h').write_text('''#pragma once
#include <cstdint>
struct ppu_context { uint64_t gpr[32]{}; };
extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
uint8_t vm_read8(uint64_t);
uint32_t vm_read32(uint64_t);
void vm_write8(uint64_t,uint8_t);
void vm_write32(uint64_t,uint32_t);
void ppu_register_function(uint64_t,void(*)(ppu_context*));
}
''')
            binary=tmp/'check'
            subprocess.run(['c++','-std=c++17','-I'+str(tmp),'-I'+str(ROOT/'src'),
                str(ROOT/'src/v212_launcher_hooks.cpp'),str(ROOT/'tests/v212_launcher_hooks_fixture.cpp'),'-o',str(binary)],check=True)
            for mode in ('disabled','valid','repeat','ticket-conflict','identity-conflict','locked','guard','out-of-bounds'):
                result=subprocess.run([str(binary),mode],capture_output=True,text=True)
                self.assertEqual(result.returncode,0 if mode in ('disabled','valid','repeat') else 89,(mode,result.stderr))
