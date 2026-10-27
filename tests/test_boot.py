import unittest
import os
import sys
import tempfile
from pathlib import Path
from tools.boot import classify_boot, capture_process, clone_disc


class BootTests(unittest.TestCase):
    def test_guest_frames_are_evidence_without_claiming_engine_startup(self):
        result=classify_boot('[mag] guest frame presented 1\n[mag] guest frame presented 120',-9,True)
        self.assertEqual(result['guest_frames_presented_lower_bound'],120)
        self.assertFalse(result['engine_initialized'])

    def test_unresolved_import_preserves_context_in_both_log_formats(self):
        for case in ['UNRESOLVED', 'unresolved']:
            line=f'[hle] {case} NID 0x8A85674D lr=0x12345678'
            result=classify_boot(line,88,False)
            self.assertEqual(result['outcome'],'unresolved_import')
            self.assertEqual(result['failure_detail'],line)

    def test_guest_assertion_is_not_a_clean_process_exit(self):
        line='edgezlib_inflate_queue.cpp:568 (0 == ret) -- assertion failed'
        for code,timed_out in [(0,False),(-9,True)]:
            result=classify_boot(line,code,timed_out)
            self.assertEqual(result['outcome'],'guest_assertion')
            self.assertEqual(result['failure_detail'],line)

    def test_unresolved_function_survives_exit_or_timeout(self):
        line='[ppu] unresolved indirect call -> 0x00123456 lr=0x00123400 tid=2'
        for code,timed_out in [(3,False),(-9,True)]:
            with self.subTest(code=code,timed_out=timed_out):
                result=classify_boot('[mag] retail_entry 0x00010230\n'+line,code,timed_out)
                self.assertEqual(result['outcome'],'unresolved_function')
                self.assertEqual(result['failure_detail'],line)
                self.assertEqual(result['timed_out'],timed_out)

    def test_noisy_process_is_stopped_at_log_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            log=Path(tmp)/'console.log'
            result=capture_process([sys.executable, '-c', 'while True: print("x"*4096, flush=True)'],
                                   Path(tmp), os.environ.copy(), log, 3, 32768)
            self.assertEqual(result['stop_reason'], 'log_limit')
            self.assertLessEqual(log.stat().st_size, 32768)
            self.assertIsNotNone(result['returncode'])

    def test_quiet_process_times_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            result=capture_process([sys.executable, '-c', 'import time; time.sleep(20)'],
                                   Path(tmp), os.environ.copy(), Path(tmp)/'log', .1, 32768)
            self.assertEqual(result['stop_reason'], 'timeout')

    def test_disc_copy_cannot_modify_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'source'; source.mkdir()
            (source/'asset').write_text('original')
            copied=Path(tmp)/'vfs'
            clone_disc(source, copied)
            (copied/'PS3_GAME/asset').write_text('changed')
            self.assertEqual((source/'asset').read_text(), 'original')

    def test_disc_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'source'; source.mkdir()
            (source/'escape').symlink_to('/tmp')
            with self.assertRaises(ValueError):
                clone_disc(source, Path(tmp)/'vfs')

    def test_loader_dispatch_message_is_not_executed_entry(self):
        r=classify_boot('[boot] dispatching entry OPD 0x376f38', 0, False)
        self.assertFalse(r['entry_reached'])
        self.assertFalse(r['engine_initialized'])

    def test_checkpoint_survives_timeout_without_becoming_success(self):
        r=classify_boot('[mag] retail_entry 0x00010230', None, True)
        self.assertTrue(r['entry_reached'])
        self.assertEqual(r['outcome'], 'timeout')
        self.assertFalse(r['engine_initialized'])

    def test_failures_are_distinguished(self):
        for log,code,want in [('[hle] UNRESOLVED NID 0x1234',1,'unresolved_import'),
                              ('[ppu] UNRESOLVED syscall 144',87,'unresolved_syscall'),
                              ('[mag] unsupported lifted code in func_1',86,'unsupported_lift'),
                              ('[mag] unsupported retail SPU policy in func_15D0',86,'unsupported_lift'),
                              ('[mag] unsupported retail SPU training task in func_3070',86,'unsupported_lift'),
                              ('',-11,'signal'),
                              ('[boot] ppu_run returned 0 (entry function unwound)',0,'entry_returned')]:
            with self.subTest(want=want):
                self.assertEqual(classify_boot(log,code,False)['outcome'],want)

class BootImageTests(unittest.TestCase):
    def test_nested_engine_uses_same_disc_root_and_correct_guest_path(self):
        from tools.boot import select_boot_image
        profile={'images':{'EBOOT':{'source':'/games/PS3_GAME/USRDIR/EBOOT.BIN'},
                           'mage_g':{'source':'/games/PS3_GAME/USRDIR/bin/mage_g.self'}}}
        image,disc,guest=select_boot_image(profile,'mage_g')
        self.assertEqual(disc,Path('/games/PS3_GAME'))
        self.assertEqual(guest,'/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self')
        self.assertIs(image,profile['images']['mage_g'])

    def test_unknown_or_outside_disc_image_is_rejected(self):
        from tools.boot import select_boot_image
        profile={'images':{'EBOOT':{'source':'/games/PS3_GAME/USRDIR/EBOOT.BIN'},
                           'mage_g':{'source':'/other/bin/mage_g.self'}}}
        for name in ['prototype','mage_g']:
            with self.assertRaises(ValueError):
                select_boot_image(profile,name)
