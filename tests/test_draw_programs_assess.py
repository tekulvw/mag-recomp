import copy,json,unittest
from tools.draw_outcomes_assess import inspect_text
from tests.test_draw_outcomes_assess import ROW,ENV,text
IDENT={'program_identity_observed':1,'vp_content_valid':1,'vp_content_hash':'0123456789ABCDEF','vp_instructions':11,'fp_structure_valid':1,'fp_structural_hash':'1234567890ABCDEF','fp_bytes':224}
ZERO={'program_identity_observed':0,'vp_content_valid':0,'vp_content_hash':'0000000000000000','vp_instructions':0,'fp_structure_valid':0,'fp_structural_hash':'0000000000000000','fp_bytes':0}
def rows():return [dict(ROW,**IDENT,frame=10),dict(ROW,**IDENT,record=2,frame=11),dict(ROW,**IDENT,record=3,frame=11),dict(ROW,**IDENT,record=4,frame=12)]
class ProgramTests(unittest.TestCase):
 def result(self,rr):return inspect_text(text(rr),ENV)
 def bad(self,mut):
  rr=rows();mut(rr[1]);r=self.result(rr);self.assertTrue(r['errors']);self.assertEqual(r['qualified_complete_frames'],[])
 def test_legacy_explicit(self):
  rr=[{k:v for k,v in r.items()if k not in IDENT}for r in rows()];r=self.result(rr);self.assertFalse(r['errors']);self.assertEqual(r['row_schema'],'legacy_v1');m=r['qualified_complete_frames'][0];self.assertEqual(m['observed_program_groups'],[]);self.assertEqual(m['identity_availability'],{'legacy_unavailable':2})
 def test_content_same_raw_address_separates(self):
  rr=rows();rr[2]['fp_structural_hash']='AAAAAAAAAAAAAAAA';r=self.result(rr);self.assertFalse(r['errors']);self.assertEqual(r['row_schema'],'observed_program_v2');self.assertEqual(len(r['qualified_complete_frames'][0]['observed_program_groups']),2)
 def test_same_content_different_addresses_merges(self):
  rr=rows();rr[2]['fp_program']='00050002';rr[2]['vp_start']=17;r=self.result(rr);gg=r['qualified_complete_frames'][0]['observed_program_groups'];self.assertEqual(len(gg),1);self.assertEqual(gg[0]['records'],2);self.assertEqual(len(gg[0]['raw_fp_program_counts']),2)
 def test_control_separates(self):
  rr=rows();rr[2]['shader_control']='02008480';self.assertEqual(len(self.result(rr)['qualified_complete_frames'][0]['observed_program_groups']),2)
 def test_target_separates(self):
  rr=rows();rr[2]['color_offsets']=[4,0,0,0];self.assertEqual(len(self.result(rr)['qualified_complete_frames'][0]['observed_program_groups']),2)
 def test_vp_content_separates(self):
  rr=rows();rr[2]['vp_content_hash']='BBBBBBBBBBBBBBBB';self.assertEqual(len(self.result(rr)['qualified_complete_frames'][0]['observed_program_groups']),2)
 def test_lengths_separate(self):
  rr=rows();rr[2]['fp_bytes']=160;self.assertEqual(len(self.result(rr)['qualified_complete_frames'][0]['observed_program_groups']),2)
 def test_early_unavailable_reset(self):
  rr=rows();rr[2].update(ZERO,outcome='fetch');m=self.result(rr)['qualified_complete_frames'][0];self.assertEqual(m['identity_availability'],{'both_valid':1,'unavailable':1});self.assertEqual(m['observed_program_groups'][0]['records'],1)
 def test_partial_not_content_group(self):
  rr=rows();rr[2].update(fp_structure_valid=0,fp_structural_hash='0000000000000000',fp_bytes=0);m=self.result(rr)['qualified_complete_frames'][0];self.assertEqual(m['identity_availability'],{'both_valid':1,'partial':1});self.assertEqual(m['observed_program_groups'][0]['records'],1)
 def test_missing_field(self):self.bad(lambda r:r.pop('fp_bytes'))
 def test_mixed_schema_rejected(self):self.bad(lambda r:[r.pop(k)for k in IDENT])
 def test_bad_hash(self):
  for v in ['abc','0123456789abcdef',123,True]:self.bad(lambda r:r.update(fp_structural_hash=v))
 def test_bad_boolean(self):
  for v in [True,2,-1]:self.bad(lambda r:r.update(vp_content_valid=v))
 def test_invalid_lengths(self):
  for k,values in [('vp_instructions',[0,545,-1,True]),('fp_bytes',[0,15,65552,True])]:
   for v in values:self.bad(lambda r:r.update({k:v}))
 def test_unavailable_stale_hash(self):self.bad(lambda r:r.update(ZERO,fp_structural_hash='0000000000000001',outcome='fetch'))
 def test_unobserved_valid_rejected(self):self.bad(lambda r:r.update(program_identity_observed=0))
 def test_early_observed_rejected(self):self.bad(lambda r:r.update(outcome='fetch'))
 def test_late_unobserved_rejected(self):self.bad(lambda r:r.update(ZERO))
 def test_zero_fp_hash_rejected(self):self.bad(lambda r:r.update(fp_structural_hash='0000000000000000'))
 def test_vp_zero_hash_valid(self):
  rr=rows();rr[1]['vp_content_hash']='0000000000000000';self.assertFalse(self.result(rr)['errors'])
 def test_max_lengths(self):
  rr=rows();rr[1].update(vp_instructions=544,fp_bytes=65536);self.assertFalse(self.result(rr)['errors'])
if __name__=='__main__':unittest.main()
