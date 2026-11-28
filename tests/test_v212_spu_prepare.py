"""Synthetic preparation and registration boundaries for the 2.12 SPU source set."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools import v212_spu_prepare as prepare


def program(kind='embedded_elf', sha='1' * 64):
    return dict(kind=kind, sha256=sha, size=4, entry=0x3000, role=None,
                abi_verified=False, occurrences=[dict(image='mage_g', file_offset=0x100, guest_ea=0x10100)],
                expected_unsupported_count=0)


class PreparationTests(unittest.TestCase):
    def test_changed_occurrence_or_missing_program_rejects_registration_plan(self):
        expected = program()
        manifest = dict(programs=[expected])
        prepare.validate_inventory(manifest, [dict(expected)])
        changed = dict(expected, occurrences=[dict(image='mage_g', file_offset=0x110, guest_ea=0x10110)])
        with self.assertRaises(ValueError):
            prepare.validate_inventory(manifest, [changed])
        with self.assertRaises(ValueError):
            prepare.validate_inventory(manifest, [])

    def test_duplicate_content_id_rejects_instead_of_registering_wrong_functions(self):
        a = program(sha='1234567' + '0' * 57)
        b = program(sha='1234567' + 'f' * 57)
        with self.assertRaises(ValueError):
            prepare.assign_image_ids([a, b])

    def test_ids_are_bound_to_content_and_not_input_order_or_old_role(self):
        a, b = program(), program(sha='2' * 64)
        ids = prepare.assign_image_ids([b, a])
        self.assertEqual(ids[a['sha256']], 0x11111111)
        self.assertEqual(ids[b['sha256']], 0x12222222)
        self.assertEqual(ids, prepare.assign_image_ids([a, b]))

    def test_ambiguous_full_or_text_runtime_fingerprint_rejects_registration(self):
        a = dict(program(), _raw=bytes(4) + b'a', _layout=dict(executable=[dict(off=0, filesz=4)]))
        b = dict(program(sha='2' * 64), _raw=bytes(4) + b'b', _layout=dict(executable=[dict(off=0, filesz=4)]))
        with self.assertRaises(ValueError):
            prepare.registration_plan([a, b], prepare.assign_image_ids([a, b]))

    @unittest.skipUnless(shutil.which('cc'), 'C compiler required')
    def test_executed_unsupported_site_halts_with_diagnostic(self):
        source = 'int main(void) { /* TODO spu: .word 0x12345678 */ spu_unsupported(0x20, ".word"); return 0; }'
        guarded, count = prepare.guard_source(source)
        self.assertEqual(count, 1)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'test.c').write_text(guarded)
            subprocess.run(['cc', str(root / 'test.c'), '-o', str(root / 'test')], check=True, capture_output=True)
            run = subprocess.run([str(root / 'test')], capture_output=True, text=True)
            self.assertEqual(run.returncode, 86)
            self.assertIn('unsupported 2.12 SPU', run.stderr)

    @unittest.skipUnless(shutil.which('cc'), 'C compiler required')
    def test_compiled_registry_binds_exact_workload_job_and_explicit_taskset_policy(self):
        elf = dict(program(), _raw=bytes(4), _layout=dict(executable=[dict(off=0, filesz=4)]))
        job = dict(program('raw_wws', '2' * 64), _raw=bytes(0x34), size=0x34,
                   entry=0x4030, unexamined_tail_bytes=0,
                   occurrences=[dict(image='mage_g', file_offset=0x200, guest_ea=0x20200)])
        rows = [elf, job]
        generated = prepare.render_registration(rows, prepare.assign_image_ids(rows))
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'mag_v212_spu_registration.h').write_text(prepare.CALLBACK_HEADER)
            (root / 'spu_workload.h').write_text("""#include <stdint.h>
                typedef struct { int unused; } spu_context;
                void spu_workload_register_img(uint64_t, void(*)(spu_context*), int, const char*);
                """)
            (root / 'spu_overlay_residency.h').write_text("""typedef struct {
                uint32_t lsa, size; const uint8_t *bytes; } spu_overlay_code_segment;
                int spu_overlay_register_verified_region(uint32_t,uint32_t,uint32_t,int,const spu_overlay_code_segment*,unsigned);
                """)
            (root / 'registry.c').write_text(generated)
            (root / 'driver.c').write_text("""#include <stdint.h>
                #include <assert.h>
                #include "mag_v212_spu_registration.h"
                typedef struct { int unused; } spu_context;
                typedef struct { uint32_t lsa,size; const uint8_t *bytes; } segment;
                static int images,workloads,jobs,tasks,last_image;
                void spu_begin_image(int id) { last_image=id; }
                void audit_1111111111111111_spu_recomp_register(void) { assert(last_image==0x11111111);images++; }
                void audit_2222222222222222_spu_recomp_register(void) { assert(last_image==0x12222222);images++; }
                void audit_1111111111111111_spu_func_00003000(spu_context *c) { (void)c; }
                void audit_2222222222222222_spu_func_00004030(spu_context *c) { (void)c; }
                void spu_workload_register_img(uint64_t fp,void(*entry)(spu_context*),int id,const char *name) {
                    uint64_t want=1469598103934665603ULL;
                    for(int i=0;i<4;i++) want*=1099511628211ULL;
                    assert(fp==want);assert(id==0x11111111);assert(entry==audit_1111111111111111_spu_func_00003000);
                    assert(name);workloads++;
                }
                int spu_overlay_register_verified_region(uint32_t ea,uint32_t load,uint32_t span,int id,const segment *code,unsigned count) {
                    assert(ea==0x20200);assert(load==0x4000);assert(span==0x34);assert(id==0x12222222);
                    assert(count==1);assert(code->lsa==0x4030);assert(code->size==4);assert(code->bytes[0]==0);jobs++;return 1;
                }
                void spu_taskset_register_task_elf(uint32_t ea,int image,int policy) {
                    assert(ea==0x10100);assert(image==0x11111111);assert(policy==777);tasks++;
                }
                int main(void) {
                    mag_v212_spu_register_all();mag_v212_spu_register_tasksets(777);mag_v212_spu_register_tasksets(777);
                    assert(images==2);assert(workloads==1);assert(jobs==1);assert(tasks==1);assert(last_image==0);return 0;
                }
                """)
            subprocess.run(['cc', '-I', str(root), str(root / 'registry.c'), str(root / 'driver.c'),
                            '-o', str(root / 'test')], check=True, capture_output=True)
            subprocess.run([str(root / 'test')], check=True, capture_output=True)

    def test_source_check_rejects_escape_drift_and_added_translation_units(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'one.c').write_text('int x;')
            outputs = {'one.c': prepare.sha256_file(root / 'one.c')}
            self.assertEqual(prepare.checked_source_files(root, outputs), [root.resolve() / 'one.c'])
            with self.assertRaises(ValueError):
                prepare.checked_source_files(root, {'../outside.c': '0' * 64})
            (root / 'one.c').write_text('int y;')
            with self.assertRaises(ValueError):
                prepare.checked_source_files(root, outputs)
            (root / 'one.c').write_text('int x;')
            (root / 'extra.c').write_text('int z;')
            with self.assertRaises(ValueError):
                prepare.checked_source_files(root, outputs)

    def test_cmake_paths_rebase_to_checked_directory(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / 'moved'
            output.mkdir()
            (output / 'one.c').write_text('int x;')
            prepare.write_sources(root / 'sources.cmake', output, [output / 'one.c'])
            if shutil.which('cmake'):
                script = root / 'probe.cmake'
                script.write_text('include("' + (root / 'sources.cmake').as_posix() + '")\n'
                                  'if(NOT EXISTS "${MAG_V212_SPU_SOURCES}")\nmessage(FATAL_ERROR "missing")\nendif()\n')
                subprocess.run(['cmake', '-P', str(script)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
