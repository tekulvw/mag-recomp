import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


class EngineSpuLiftTests(unittest.TestCase):
    def module(self):
        spec=importlib.util.find_spec('tools.engine_spu_lift')
        self.assertIsNotNone(spec, 'Engine policy generator is required')
        from tools import engine_spu_lift
        return engine_spu_lift

    def fixture(self):
        # Real big-endian ELF64 with one load segment; bytes map to EA 0x10200.
        image=bytearray(0x240)
        image[:6]=b'\x7fELF\x02\x02'
        struct.pack_into('>Q',image,32,64)
        struct.pack_into('>HH',image,54,56,1)
        struct.pack_into('>IIQQQQQQ',image,64,1,5,0,0x10000,0x10000,len(image),len(image),0x10000)
        image[0x200:0x210]=bytes.fromhex('40200000350000000000000000000000')
        raw=bytes(image[0x200:0x210])
        profile=dict(elf_sha256=hashlib.sha256(image).hexdigest(),policy=dict(
            offset=0x200,guest_ea=0x10200,size=16,sha256=hashlib.sha256(raw).hexdigest(),
            runtime_fingerprint='0xD34DA3B18F54F906'))
        return bytes(image),raw,profile

    def test_extraction_binds_full_elf_slice_and_load_mapping(self):
        mod=self.module();image,raw,profile=self.fixture()
        self.assertEqual(mod.extract_policy(image,profile),raw)
        for field,value in [('offset',0x204),('guest_ea',0x10204),('size',17),('sha256','0'*64),
                            ('runtime_fingerprint','0x0000000000000000')]:
            bad=json.loads(json.dumps(profile));bad['policy'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):mod.extract_policy(image,bad)
        changed=bytearray(image);changed[-1]=1
        with self.assertRaises(ValueError):mod.extract_policy(bytes(changed),profile)

    def test_fingerprint_matches_pinned_sdk_algorithm(self):
        mod=self.module()
        # SDK's historical basis omits the final digit of standard FNV-1a.
        self.assertEqual(mod.runtime_fingerprint(b''),1469598103934665603)
        self.assertEqual(mod.runtime_fingerprint(b'hello'),0x005A0D15131EC7A1)

    @unittest.skipUnless(shutil.which('cc'),'C compiler required')
    def test_every_generated_unsupported_site_exits_before_following_code(self):
        mod=self.module()
        cases=['/* TODO spu: unresolved branch */;',
               '/* TODO spu: .word 0xffffffff */ spu_unsupported(0xA04, ".word");',
               'spu_unsupported(0xA08, "unknown");']
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for i,site in enumerate(cases):
                source='void spu_unsupported(unsigned pc,const char* name) {}\nint main(void) { '+site+' return 12; }\n'
                guarded,count=mod.guard_policy_source(source)
                self.assertEqual(count,1)
                path=root/f'case{i}.c';path.write_text(guarded);binary=root/f'case{i}'
                subprocess.run(['cc',str(path),'-o',str(binary)],check=True,capture_output=True)
                run=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(run.returncode,86)
                self.assertIn('unsupported retail SPU policy',run.stderr)



class TrainingSpuLiftTests(unittest.TestCase):
    def fixture(self):
        from tools import engine_spu_lift
        task=bytearray(0xA0)
        task[:7]=b"\x7fELF\x01\x02\x01"
        struct.pack_into('>H',task,18,23)
        struct.pack_into('>I',task,24,0x30)
        struct.pack_into('>I',task,32,0x70)
        struct.pack_into('>HH',task,46,40,1)
        struct.pack_into('>IIIIIIIIII',task,0x70,0,1,6,0x30,0x40,8,0,0,4,0)
        image=bytearray(0x400);image[:6]=b"\x7fELF\x02\x02"
        struct.pack_into('>Q',image,32,64);struct.pack_into('>HH',image,54,56,1)
        struct.pack_into('>IIQQQQQQ',image,64,1,5,0,0x10000,0x10000,len(image),len(image),0x10000)
        image[0x200:0x200+len(task)]=task
        profile=dict(elf_sha256=hashlib.sha256(image).hexdigest(),training_task=dict(
            offset=0x200,guest_ea=0x10200,size=len(task),sha256=hashlib.sha256(task).hexdigest(),
            runtime_fingerprint=hex(engine_spu_lift.runtime_fingerprint(task)),entry=0x30,code_end=0x38,
            image_id=102,expected_unsupported_count=0))
        return bytes(image),bytes(task),profile

    def test_training_extraction_checks_full_identity_mapping_and_elf_code_boundary(self):
        from tools import engine_spu_lift as mod
        image,task,profile=self.fixture()
        self.assertEqual(mod.extract_training(image,profile),task)
        for field,value in [('guest_ea',0x10204),('size',159),('entry',0x34),('code_end',0x3C),
                            ('sha256','0'*64),('runtime_fingerprint','0x0')]:
            bad=json.loads(json.dumps(profile));bad['training_task'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):mod.extract_training(image,bad)
        changed=bytearray(image);changed[-1]=1
        with self.assertRaises(ValueError):mod.extract_training(bytes(changed),profile)

    def test_queued_training_elf_uses_its_own_identity_and_boundary(self):
        from tools import engine_spu_lift as mod
        image,task,profile=self.fixture()
        profile['queued_training_task']=profile.pop('training_task')
        self.assertEqual(mod.extract_training(image,profile,'queued_training_task'),task)
        for field,value in [('guest_ea',0x10204),('size',159),('entry',0x34),('code_end',0x3C),
                            ('sha256','0'*64),('runtime_fingerprint','0x0')]:
            bad=json.loads(json.dumps(profile));bad['queued_training_task'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):
                mod.extract_training(image,bad,'queued_training_task')

    @unittest.skipUnless(shutil.which('cc'),'C compiler required')
    def test_generated_constructor_registers_training_without_replacing_existing_images(self):
        import os
        from tools import engine_spu_lift as mod
        sdk=Path(os.environ.get('MAG_TEST_SDK') or os.environ.get('PS3RECOMP_DIR') or '/nonexistent/ps3recomp')
        if not (sdk/'runtime/spu/spu_workload.c').is_file():self.skipTest('SDK source required')
        profile=dict(policy=dict(entry=0xA00,image_id=101,runtime_fingerprint='0x111'),
                     training_task=dict(entry=0x3070,image_id=102,runtime_fingerprint='0x222'),
                     queued_training_task=dict(entry=0x3070,image_id=103,runtime_fingerprint='0x444'))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);reg=root/'registration.c';reg.write_text(mod.render_engine_registration(profile))
            test=root/'test.c';test.write_text(r"""
#include "spu_workload.h"
#include <assert.h>
static int current_image,policy_table,task_table,queued_table;
void spu_begin_image(int id) { current_image=id; }
void mag_wws_policy_spu_recomp_register(void) { policy_table=current_image; }
void mag_training_spu_recomp_register(void) { task_table=current_image; }
void mag_training_queued_spu_recomp_register(void) { queued_table=current_image; }
void mag_wws_policy_spu_func_00000A00(spu_context* c) {(void)c;}
void mag_training_spu_func_00003070(spu_context* c) {(void)c;}
void mag_training_queued_spu_func_00003070(spu_context* c) {(void)c;}
static void edge(spu_context* c) {(void)c;}
int main(void) {
  spu_workload_register_img(0x333,edge,100,"synthetic EDGE");
  int id=-1;
  assert(spu_workload_find_img(0x111,&id)==mag_wws_policy_spu_func_00000A00 && id==101);
  assert(spu_workload_find_img(0x222,&id)==mag_training_spu_func_00003070 && id==102);
  assert(spu_workload_find_img(0x333,&id)==edge && id==100);
  assert(spu_workload_find_img(0x444,&id)==mag_training_queued_spu_func_00003070 && id==103);
  assert(spu_workload_find_img(0x224,&id)==0 && id==0);
  assert(policy_table==101 && task_table==102 && queued_table==103 && current_image==0);
  assert(spu_workload_count()==4);
  return 0;
}
""")
            binary=root/'fixture'
            import sys
            dead=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']
            command=['cc','-std=gnu11','-ffunction-sections','-fdata-sections',
                     '-I'+str(sdk/'runtime/spu'),'-I'+str(sdk/'runtime'),'-I'+str(sdk/'include'),str(reg),str(test),
                     str(sdk/'runtime/spu/spu_workload.c'),*dead,'-pthread','-lm','-o',str(binary)]
            subprocess.run(command,check=True,capture_output=True,text=True)
            run=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
