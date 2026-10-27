import unittest,importlib.util,pathlib,tempfile,hashlib
D=pathlib.Path(__file__).parent
spec=importlib.util.spec_from_file_location('a',D.parent/'tools/ppu_callspan_assess.py');a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
S='[ppu-call-span-summary] run_id=100 target=00012000 start_us=100 duration_us=1000 count=2 dropped=0 limit=32768 pending_recorded_brackets=0 completion=PPU-fn-and-trampoline-only\n'
def row(seq,phase,us,token=1,flags=0,r8=3):return f'[ppu-call-span] seq={seq} run_id=100 token={token} phase={phase} us={us} ctx=1000 thread=4294967303 target=00012000 depth=0 flags={flags} r3=5 r4=0 r5=256 r6=0 r7=7 r8={r8} r9=0 r10=0 r31=31\n'
class Tests(unittest.TestCase):
 def result(self,text):return a.assess_text(text)
 def good(self):return S+row(1,0,101)+row(2,1,701)
 def test_serialized_fixture(self):
  r=self.result((D/'fixtures/ppu-callspan/normal.stderr').read_text());self.assertFalse(r['errors']);self.assertEqual(r['counts']['normal_ends'],1);self.assertEqual(r['durations_us']['mean'],600)
 def test_pair_and_rawflags(self):
  r=self.result(self.good());self.assertFalse(r['errors']);self.assertEqual(r['flag_groups']['3']['starts'],1);self.assertTrue(r['flag_groups']['3']['bit_0x2_set'])
 def test_unwind_not_normal(self):
  r=self.result(S+row(1,0,101)+row(2,1,701,flags=1));self.assertEqual(r['counts']['abnormal_ends'],1);self.assertEqual(r['counts']['normal_ends'],0)
 def test_partial_pending(self):
  r=self.result(S.replace('count=2','count=1').replace('brackets=0','brackets=1')+row(1,0,1099));self.assertFalse(r['errors']);self.assertEqual(r['counts']['unclosed_starts'],1);self.assertEqual(r['pending_examples'][0]['remaining_to_deadline_us'],1)
 def test_truncation_reject(self):self.assertTrue(self.result(S+row(1,0,101))['errors'])
 def test_corruption_reject(self):
  for t in [self.good().replace('seq=2','seq=3'),self.good().replace('token=1 phase=1','token=2 phase=1'),self.good().replace('r7=7 r8=3 r9=0 r10=0 r31=31\n','r7=8 r8=3 r9=0 r10=0 r31=31\n',1),self.good().replace('phase=1 us=701','phase=1 us=1100'),self.good().replace('depth=0 flags=0 r3=5','depth=1 flags=0 r3=5',1),self.good()+S,self.good().replace('seq=2','seq=2 seq=2'),self.good()[:-1]]:
   self.assertTrue(self.result(t)['errors'],t)
 def test_equalclock_seq_valid(self):self.assertFalse(self.result(S+row(1,0,101)+row(2,1,101))['errors'])
 def test_dropped_disables_rate(self):
  r=self.result(self.good().replace('dropped=0','dropped=1'));self.assertFalse(r['rate_inference_allowed']);self.assertTrue(r['warnings'])
 def test_audio_window_must_match(self):
  r=self.result(self.good()+'[audio-chronology-summary] reason=window count=0 dropped=0 limit=32768 start_us=101 duration_us=1000 queue_min=0 queue_zero=0 max_submit_interval_us=0 inflight_at_flush=0 coverage=SPU-DMA-only overlap_is_ambiguous=1\n');self.assertTrue(r['errors'])
 def test_summary_pending_checked(self):self.assertTrue(self.result(self.good().replace('brackets=0','brackets=1'))['errors'])
 def test_distinct_flag_cohorts(self):
  r=self.result(S.replace('count=2','count=4')+row(1,0,101,r8=1)+row(2,1,201,r8=1)+row(3,0,301,token=2,r8=3)+row(4,1,1001,token=2,r8=3));self.assertFalse(r['errors']);self.assertFalse(r['flag_groups']['1']['bit_0x2_set']);self.assertTrue(r['flag_groups']['3']['bit_0x2_set']);self.assertEqual(r['flag_groups']['3']['normal_duration_us']['mean'],700)
 def test_unrelated_partial_tail_does_not_invent_span_loss(self):
  r=self.result(self.good()+'unrelated unfinished app text');self.assertFalse(r['errors']);self.assertTrue(r['warnings'])
 def test_error_cap(self):
  r=self.result(self.good()+'[ppu-call-span broken\n'*100);self.assertEqual(len(r['errors']),32);self.assertEqual(r['error_count'],100)
 def test_fixed_input_readonly_and_symlink(self):
  with tempfile.TemporaryDirectory(dir=D) as t:
   f=pathlib.Path(t)/'input';f.write_text(self.good());before=f.read_bytes();r=a.assess_file(f);self.assertEqual(r['input']['sha256'],hashlib.sha256(before).hexdigest());self.assertEqual(f.read_bytes(),before);link=pathlib.Path(t)/'link';link.symlink_to(f)
   with self.assertRaises(ValueError):a.assess_file(link)
 def test_malformed_audio_missing_seq_or_us_structured_error(self):
  zero=S.replace('count=2','count=0')
  for line in ['[audio-chronology] us=101 kind=2\n','[audio-chronology] seq=1 kind=2\n']:
   r=self.result(zero+line);self.assertTrue(r['errors']);self.assertFalse(r['rate_inference_allowed']);self.assertFalse(r['same_window_audio_comparison_allowed'])
 def test_no_summary(self):self.assertTrue(self.result(row(1,0,101))['errors'])
if __name__=='__main__':unittest.main(verbosity=2)
