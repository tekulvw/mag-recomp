"""Reject unsafe image combinations before configuring the SDK or compiling."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cmake'), 'CMake is required')
class BuildConfigTests(unittest.TestCase):
    def test_unknown_image_is_rejected_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            result=subprocess.run(['cmake','-S',str(ROOT),'-B',tmp,
                '-DMAG_IMAGE=prototype','-DRECOMP_DIR=/unused/lift'],capture_output=True,text=True,timeout=30)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('MAG_IMAGE must be EBOOT or mage_g',result.stderr)


@unittest.skipUnless(shutil.which('c++'), 'C++ compiler is required')
class HookIsolationTests(unittest.TestCase):
    def test_engine_loads_firmware_without_launcher_hooks(self):
        self.check_hooks(False)

    def test_launcher_retains_network_hooks(self):
        self.check_hooks(True)

    def check_hooks(self, launcher):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            header = """#include <atomic>
#include <cstdint>
struct ppu_context { uint64_t gpr[32]; double fpr[32]; uint64_t lr; };
inline uint16_t vm_read16(uint32_t) { return 0; }
inline uint32_t vm_read32(uint32_t) { return 0; }
inline uint64_t vm_read64(uint32_t) { return 0; }
inline uint8_t vm_read8(uint32_t) { return 0; }
inline void vm_write8(uint32_t,uint8_t) {}
inline void vm_write32(uint32_t,uint32_t) {}
"""
            if launcher:
                for name in ('00014AD0','00087108','002D6530','00010EC8','000D1E48'):
                    header += 'inline void func_'+name+'(ppu_context*) {}\n'
            (tmp/'ppu_recomp.h').write_text(header)
            (tmp/'check.cpp').write_text("""#include "ppu_recomp.h"
#include <cstdlib>
#include <set>
static std::set<uint64_t> registered;
static bool loaded;
extern "C" { uint8_t* vm_base=nullptr; uint32_t ppu_vm_size=0; }
extern "C" void sail_graphics_picture_capture_begin(){std::abort();}
extern "C" uint64_t sail_graphics_picture_capture_end(){std::abort();}
extern "C" int rsx_picture_bind_descriptor(uint32_t,uint64_t){std::abort();}
extern "C" int rsx_picture_capture_command(uint32_t,uint32_t,uint32_t,uint32_t){std::abort();}
extern "C" const uint8_t* rsx_picture_data(uint64_t,uint32_t,uint32_t,uint32_t){std::abort();}
extern "C" void rsx_picture_release(uint64_t){std::abort();}
extern "C" void mag_load_queue_firmware() { loaded=true; }
extern "C" void ppu_register_function(uint64_t a,void(*)(ppu_context*)) { registered.insert(a); }
void func_003D5E5C(ppu_context*) {}
void func_003C65E0(ppu_context*) {}
void func_0027268C(ppu_context*) {}
void func_003CE224(ppu_context*) {}
void func_003BFD90(ppu_context*) {}
void func_00E4C960(ppu_context*) {std::abort();}
void func_002633E4(ppu_context*) {std::abort();}
void func_00263458(ppu_context*) {std::abort();}
void func_0026374C(ppu_context*) {std::abort();}
void func_002637C0(ppu_context*) {std::abort();}
void func_00263AD0(ppu_context*) {std::abort();}
void mag_sre_func_210127CC(ppu_context*) {}
void mag_sre_func_210171B8(ppu_context*) {}
void mag_sre_func_210174F8(ppu_context*) {}
void mag_sre_func_210178D8(ppu_context*) {}
extern "C" void mag_register_handoff() {}
extern "C" void ps3_load_prx_modules();
int main() {
    setenv("MAG_TRACE_MOVIE","1",1); // Invalid/unloaded engine pins must prevent registration.
    setenv("MAG_TRACE_STARTUP_UI","1",1);
    setenv("MAG_TRACE_FRONTEND","1",1);
    setenv("MAG_TRACE_VRAM_TRANSFER","1",1);
    setenv("MAG_TRACE_NETWORK","1",1);
    setenv("MAG_LOCAL_NO_TICKET","1",1);
    ps3_load_prx_modules();
    std::set<uint64_t> expected={0x210174F8,0x210178D8,0x210127CC,0x210171B8};
#ifdef MAG_LAUNCHER_HOOKS
    expected.insert({0x14AD0,0x2D6530,0x10EC8,0xD1E48});
#endif
    return loaded && registered==expected ? 0 : 1;
}
""")
            flags=['-DMAG_QUEUE_FIRMWARE']
            if launcher: flags+=['-DMAG_LAUNCHER_HOOKS']
            result=subprocess.run(['c++','-std=c++17',*flags,'-I'+str(tmp),
                str(ROOT/'src/stubs.cpp'),str(tmp/'check.cpp'),'-o',str(tmp/'check')],
                capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(tmp/'check')],check=True,timeout=5)
