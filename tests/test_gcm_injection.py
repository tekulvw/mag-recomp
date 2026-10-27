"""MAG's resource writes must not overwrite runtime GCM structures."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'local/ps3recomp-login-validation'

@unittest.skipUnless(shutil.which('cc') and SDK.is_dir(), 'requires the local SDK and C compiler')
class GcmInjectionTests(unittest.TestCase):
    def test_actual_runner_setup_preserves_gcm_across_resource_output(self):
        # Compile the actual pre-ELF startup prefix; the dependency substitutes
        # only avoid loading a game image or starting the host frame thread.
        runner = (ROOT / 'src/main.cpp').read_text()
        start = runner.index('int main(int argc, char** argv)')
        end = runner.index('    uint32_t entry = ppu_load_elf', start)
        prefix = runner[start:end].replace('int main(', 'int runner_setup(') + '    return 0;\n}\n'
        fixture = r'''#include "libs/video/cellGcmSys.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) { abort(); }
void ppu_resv_break_store(uint64_t a) { abort(); }
int spu_coh_is_reserved(uint32_t a) { return 0; }
void spu_coh_notify_write(uint32_t a) { abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
int alloc_guest_vm(void) { return vm_base != NULL; }
''' + prefix + r'''
int main(void) {
    vm_base=calloc(1,0x80000000ull); assert(vm_base);
    char* argv[]={"fixture","synthetic.elf",NULL};
    assert(runner_setup(2,argv)==0);
    memset(s_io_address_table,0xff,sizeof(s_io_address_table));
    memset(s_ea_address_table,0xff,sizeof(s_ea_address_table));
    assert(cellGcmMapEaIoAddress(0x44e00000,0x0e000000,0x100000)==CELL_OK);
    assert(cellGcmGetOffsetTable((CellGcmOffsetTable*)(uintptr_t)0x100)==CELL_OK);
    uint32_t forward=vm_read32(0x100),reverse=vm_read32(0x104);
    uint32_t control=(uint32_t)(uintptr_t)cellGcmGetControlRegister();
    uint32_t label=(uint32_t)(uintptr_t)cellGcmGetLabelAddress(72);
    vm_write32(control,0x1200); vm_write32(control+4,0x1000);
    vm_write32(control+8,0x55); vm_write32(label,0x12345678);
    /* Same destination and length as the completed retail EdgeZlib PUT,
     * using synthetic bytes instead of any original resource payload. */
    unsigned char output[0x4000]; memset(output,0xA5,sizeof(output));
    memcpy(vm_base+0x20000080,output,sizeof(output));
    assert(vm_read16(forward+0x44e*2)==0xe0);
    assert(vm_read16(reverse+0xe0*2)==0x44e);
    assert(vm_read32(control)==0x1200 && vm_read32(control+4)==0x1000);
    assert(vm_read32(control+8)==0x55 && vm_read32(label)==0x12345678);
    const uint32_t windows[][2]={{0x20000000,0x30000000},
        {0x30000000,0x40000000},{0x40000000,0x50000000},
        {0x50000000,0x58000000},{0x58000000,0x59000000},
        {0x60000000,0x70000000},{0xC0000000,0xD0000000},
        {0xD0000000,0xE0000000}};
    for(unsigned i=0;i<sizeof(windows)/sizeof(windows[0]);i++)
        assert(!((uint64_t)ppu_hle_inject_base < windows[i][1] &&
            (uint64_t)ppu_hle_inject_base+0x01000000 > windows[i][0]));
    assert((uint64_t)ppu_hle_inject_base+0x01000000 <= 0xE0000000ull);
    assert(label==ppu_hle_inject_base+72*16);
    assert(control==ppu_hle_inject_base+0x2000);
    assert(forward==ppu_hle_inject_base+0x3000);
    assert(reverse==ppu_hle_inject_base+0x5000);
    free(vm_base);
}
'''
        with tempfile.TemporaryDirectory(prefix='mag-inject-') as tmp:
            tmp=Path(tmp); source=tmp/'fixture.c'; binary=tmp/'fixture'
            source.write_text(fixture)
            linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
            subprocess.run([os.environ.get('CC','cc'),'-std=c17','-O1','-UNDEBUG',
                '-ffunction-sections','-fdata-sections','-I'+str(SDK),'-I'+str(SDK/'include'),
                str(source),*linker,'-o',str(binary)],check=True,timeout=60)
            env={k:v for k,v in os.environ.items() if not k.startswith(('A2O_','PS3_','PS3RECOMP_'))}
            subprocess.run([str(binary)],env=env,check=True,timeout=10)
