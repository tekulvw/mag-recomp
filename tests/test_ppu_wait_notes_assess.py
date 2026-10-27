import unittest, pathlib, tempfile
from tools import ppu_wait_notes_assess as assess
SPAN={'seq':1,'run_id':100,'token':1,'phase':0,'us':110,'ctx':'ABC','thread':9,'target':'00012000','depth':0,'flags':0,**{'r'+str(k):0 for k in (3,4,5,6,7,8,9,10,31)}}
SPAN.update(r5=256,r8=2,r7=1000,r31=2000)
NOTE={'seq':1,'run_id':100,'span':1,'token':1,'us':120,'ctx':'ABC','thread':9,'kind':1,'phase':0,'rc':0,'arg0':50,'arg1':0,'arg2':0,'arg3':0}
SUM={'run_id':100,'start_us':100,'duration_us':1000,'count':3,'dropped':0,'limit':32768,'pending_recorded_sleeps':0,'coverage':'existing-sleep-and-workload-info-only'}
def line(prefix,d):return prefix+' '+' '.join(str(k)+'='+str(v)for k,v in d.items())+'\n'
def fixture(notes=None,summary=None,spans=None):
 if notes is None:notes=[dict(NOTE),dict(NOTE,seq=2,phase=1,us=180),dict(NOTE,seq=3,token=0,kind=2,phase=2,us=185,arg0=0x100,arg1=3,arg2=0x300,arg3=2|(3<<8)|(1<<16))]
 if spans is None:spans=[dict(SPAN),dict(SPAN,seq=2,phase=1,us=200)]
 s=dict(SUM);s.update(summary or {})
 return '[audio-chronology-summary] reason=window count=0 dropped=0 limit=32768 start_us=100 duration_us=1000 queue_min=4294967295 queue_zero=0 max_submit_interval_us=0 inflight_at_flush=0 coverage=SPU-DMA-only overlap_is_ambiguous=1\n'+line('[ppu-call-span-summary]',{'run_id':100,'target':'00012000','start_us':100,'duration_us':1000,'count':len(spans),'dropped':0,'limit':32768,'pending_recorded_brackets':sum(x['phase']==0 for x in spans)-sum(x['phase']==1 for x in spans),'completion':'PPU-fn-and-trampoline-only'})+''.join(line('[ppu-call-span]',x)for x in spans)+line('[ppu-call-note-summary]',s)+''.join(line('[ppu-call-note]',x)for x in notes)
class Tests(unittest.TestCase):
 def ok(self,text):
  r=assess.assess_text(text);self.assertEqual(r['errors'],[]);return r
 def bad(self,text):
  r=assess.assess_text(text);self.assertTrue(r['error_count']);self.assertFalse(r['normal_span_metrics_allowed']);return r
 def test_complete(self):
  r=self.ok(fixture());self.assertEqual(r['counts']['paired_sleeps'],1);self.assertEqual(r['sleep_duration_us']['mean'],60);self.assertEqual(r['flag_groups']['2']['normal_span_duration_us']['mean'],90);self.assertEqual(r['flag_groups']['2']['workload']['ready_nonzero'],1);self.assertTrue(r['complete_window_inference_allowed'])
 def test_missing_summary_not_zero(self):self.bad(fixture().replace(line('[ppu-call-note-summary]',SUM),''))
 def test_missing_fields(self):
  for key in NOTE:
   n=dict(NOTE);n.pop(key);self.bad(fixture([n],{'count':1,'pending_recorded_sleeps':1}))
 def test_wrong_schema_duplicate_fields(self):self.bad(fixture().replace('kind=1 phase=0','kind=1 kind=1 phase=0',1))
 def test_partial_note(self):self.bad(fixture()[:-1])
 def test_unknown_kind(self):self.bad(fixture([dict(NOTE,kind=3)],{'count':1,'pending_recorded_sleeps':1}))
 def test_bad_pair_identity(self):
  for k,v in [('ctx','ABD'),('thread',10),('arg0',51),('run_id',101),('span',2)]:self.bad(fixture([dict(NOTE),dict(NOTE,seq=2,phase=1,us=180,**{k:v})],{'count':2}))
 def test_outside_span_and_window(self):
  for us in [99,109,201,1100]:self.bad(fixture([dict(NOTE,us=us)],{'count':1,'pending_recorded_sleeps':1}))
 def test_end_without_start(self):self.bad(fixture([dict(NOTE,phase=1)],{'count':1}))
 def test_pending_boundary_censored(self):
  r=self.ok(fixture([dict(NOTE)],{'count':1,'pending_recorded_sleeps':1},[dict(SPAN)]));self.assertEqual(r['counts']['censored_sleeps'],1);self.assertEqual(r['counts']['paired_sleeps'],0);self.assertFalse(r['complete_window_inference_allowed'])
 def test_unwind_excluded(self):
  r=self.ok(fixture(spans=[dict(SPAN),dict(SPAN,seq=2,phase=1,us=200,flags=1)]));self.assertEqual(r['counts']['eligible_normal_span_sleeps'],0)
 def test_drop_disables_complete_metrics(self):
  r=self.ok(fixture(summary={'dropped':1}));self.assertFalse(r['normal_span_metrics_allowed']);self.assertFalse(r['complete_window_inference_allowed'])
 def test_count_pending_seq_mismatch(self):
  self.bad(fixture(summary={'count':2}));self.bad(fixture(summary={'pending_recorded_sleeps':1}));self.bad(fixture([dict(NOTE,seq=2)],{'count':1,'pending_recorded_sleeps':1}))
 def test_api_error_unknown_only(self):
  n=dict(NOTE,token=0,kind=2,phase=2,rc=-2143221999,arg3=2**64-1);r=self.ok(fixture([n],{'count':1}));self.assertEqual(r['workload']['errors'],1)
  self.bad(fixture([dict(n,arg3=0)],{'count':1}));self.bad(fixture([dict(n,rc=0)],{'count':1}))
 def test_sleep_error_and_windows_unknown(self):
  for rc,err in [(-1,4),(-2**31,0)]:self.ok(fixture([dict(NOTE),dict(NOTE,seq=2,phase=1,us=180,rc=rc,arg1=err)],{'count':2}))
  self.bad(fixture([dict(NOTE),dict(NOTE,seq=2,phase=1,us=180,rc=7)],{'count':2}))
 def test_multiple_cohorts_separated(self):
  sp=[dict(SPAN),dict(SPAN,seq=2,phase=1,us=200),dict(SPAN,seq=3,token=2,us=210,ctx='ABD',thread=10,r8=3),dict(SPAN,seq=4,token=2,phase=1,us=300,ctx='ABD',thread=10,r8=3)];ns=[dict(NOTE),dict(NOTE,seq=2,phase=1,us=180),dict(NOTE,seq=3,span=2,token=2,us=220,ctx='ABD',thread=10),dict(NOTE,seq=4,span=2,token=2,phase=1,us=280,ctx='ABD',thread=10)];r=self.ok(fixture(ns,{'count':4},sp));self.assertEqual(r['cohort_count'],2);self.assertEqual(set(r['flag_groups']),{'2','3'})
 def test_fresh_file_read_no_mutation(self):
  with tempfile.TemporaryDirectory()as d:
   p=pathlib.Path(d)/'log';p.write_text(fixture());raw=p.read_bytes();r=assess.assess_file(p);self.assertEqual(p.read_bytes(),raw);self.assertEqual(r['input']['bytes'],len(raw));link=pathlib.Path(d)/'link';link.symlink_to(p)
   with self.assertRaises(ValueError):assess.assess_file(link)
 def test_diversity_cap_failclosed(self):
  rows=[]
  for n in range(129):rows += [dict(SPAN,seq=n*2+1,token=n+1,r8=n,us=110+n*2),dict(SPAN,seq=n*2+2,token=n+1,r8=n,phase=1,us=111+n*2)]
  self.bad(fixture([],{'count':0},rows))
 def test_unknown_coverage(self):self.bad(fixture(summary={'coverage':'markers'}))
 def test_legacy_span_corruption_blocks_metrics(self):self.bad(fixture(spans=[dict(SPAN),dict(SPAN,seq=2,phase=1,us=200,r7=999)]))
 def test_equal_clock_valid(self):self.ok(fixture([dict(NOTE),dict(NOTE,seq=2,phase=1)],{'count':2}))
 def test_zero_enabled_stream(self):
  r=self.ok(fixture([],{'count':0},[]));self.assertEqual(r['counts']['sleep_starts'],0);self.assertTrue(r['normal_span_metrics_allowed'])
 def test_missing_audio_summary(self):self.bad(fixture().split('\n',1)[1])
 def test_duplicate_audio_summary(self):self.bad(fixture().split('\n',1)[0]+'\n'+fixture())
 def test_malformed_audio_summary(self):
  for old,new in [(' count=0',''),(' limit=32768',' limit=32769'),(' overlap_is_ambiguous=1',' overlap_is_ambiguous=0'),(' coverage=SPU-DMA-only',' coverage=ALL'),(' dropped=0',' dropped=4294967296'),(' count=0',' count=1'),(' duration_us=1000',' duration_us=999'),(' reason=window',' reason=unknown')]:self.bad(fixture().replace(old,new,1))
 def test_audio_drops_disable_normal_metrics(self):
  r=self.ok(fixture().replace(' dropped=0',' dropped=1',1));self.assertFalse(r['normal_span_metrics_allowed'])
 def test_fixed_cap(self):self.bad(fixture(summary={'limit':32769}))
if __name__=='__main__':unittest.main()
