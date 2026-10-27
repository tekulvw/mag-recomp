import copy,json,unittest
from tools.draw_outcomes_assess import inspect_text
ROW={'record':1,'frame':10,'outcome':'emitted','primitive':5,'packets':1,'array_batches':1,'index_batches':0,'array_vertices':3,'requested_indices':0,'index_offset':0,'index_location':0,'index_u32':0,'inline_bytes':0,'immediate_vertices':0,'fetch_ok':1,'source_refs':3,'vertices':3,'cuts':0,'vp_start':0,'fp_program':'00040002','shader_control':'02008400','target_select':1,'color_offsets':[196608,0,0,0],'color_locations':[0,0,0,0],'clip':[2560,720],'attributes':[['00000000','00000000']for _ in range(16)]}
ENV={'RSX_ENGINE_DRAW_OBSERVER':'/tmp/example.jsonl','RSX_ENGINE_DRAW_OBSERVER_TRIGGER':'/tmp/example.trigger','RSX_ENGINE_DRAW_OBSERVER_MAX':'4096'}
def text(rows):return ''.join(json.dumps(r)+'\n' for r in rows)
def fixture():return [dict(ROW),dict(ROW,record=2,frame=11,outcome='fetch'),dict(ROW,record=3,frame=11),dict(ROW,record=4,frame=12)]
class Tests(unittest.TestCase):
 def bad(self,s):self.assertTrue(inspect_text(s,ENV)['errors'])
 def test_interior_only(self):
  r=inspect_text(text(fixture()),ENV);self.assertEqual(r['errors'],[]);self.assertEqual([x['frame']for x in r['qualified_complete_frames']],[11]);self.assertEqual(r['qualified_complete_frames'][0]['outcomes'],{'fetch':1,'emitted':1})
 def test_environment_required(self):r=inspect_text(text(fixture()));self.assertEqual(r['qualified_complete_frames'],[]);self.assertEqual(r['frame_candidates'],[11])
 def test_filtered_rejected(self):r=inspect_text(text(fixture()),dict(ENV,RSX_ENGINE_DRAW_OBSERVER_TARGET='0x30000'));self.assertEqual(r['qualified_complete_frames'],[])
 def test_only_two_partial_frames(self):r=inspect_text(text(fixture()[:2]),ENV);self.assertEqual(r['qualified_complete_frames'],[])
 def test_row_sequence(self):rows=fixture();rows[2]['record']=4;self.bad(text(rows))
 def test_schema(self):r=dict(ROW);r.pop('packets');self.bad(text([r]))
 def test_duplicate_json_key(self):self.bad(text([ROW]).replace('"record": 1','"record": 1,"record": 1'))
 def test_unknown_outcome(self):self.bad(text([dict(ROW,outcome='terrain_missing')]))
 def test_negative_or_bool_integer(self):
  for n in [-1,True]:self.bad(text([dict(ROW,frame=n)]))
 def test_hex(self):self.bad(text([dict(ROW,fp_program='4000xyz')]))
 def test_array_width(self):self.bad(text([dict(ROW,color_offsets=[1,2,3])]))
 def test_attribute_width(self):self.bad(text([dict(ROW,attributes=[['0','1']])]))
 def test_truncated_line(self):self.bad(text([ROW])[:-1])
 def test_regressing_frames(self):rows=fixture();rows[-1]['frame']=9;self.bad(text(rows))
 def test_skipped_empty_frames(self):rows=fixture();rows[1]['frame']=11;rows[2]['frame']=11;rows[3]['frame']=14;r=inspect_text(text(rows),ENV);self.assertEqual([x['frame']for x in r['qualified_complete_frames']],[11])
 def test_cap_tail_never_complete(self):rows=[dict(ROW,record=i+1,frame=10 if i==0 else 11)for i in range(4096)];r=inspect_text(text(rows),ENV);self.assertEqual(r['qualified_complete_frames'],[])
 def test_cap_overflow(self):self.bad(text([dict(ROW,record=i+1)for i in range(4097)]))
 def test_empty_unavailable(self):self.bad('')
if __name__=='__main__':unittest.main()
