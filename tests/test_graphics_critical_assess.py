import unittest
from tools.graphics_critical_assess import assess
class Assess(unittest.TestCase):
 def test_missing_join_and_zero_presentation(self):
  a=assess([{"id":0,"kind":6,"ns":100,"words":[0,0]}],{"clock":"monotonic_ns","dropped":0})
  self.assertFalse(a["presentation_qualified"]);self.assertEqual(a["status"],"incomplete");self.assertTrue(a["unresolved_edges"])
 def test_loss_never_complete(self):
  a=assess([],{"clock":"wrong","dropped":1});self.assertEqual(a["status"],"incomplete");self.assertEqual(a["lost_events"],1)

 def test_nested_and_crossing_scopes_use_interval_union(self):
  events=[{'id':0,'kind':4,'ns':20,'owner':1,'words':[]},
          {'id':1,'kind':1,'ns':90,'owner':100,'thread':8,'span':1,'words':[10,0x23D7D0,4,0,0,0,0,1]},
          {'id':2,'kind':1,'ns':40,'owner':100,'thread':8,'span':2,'words':[30,0xDFD2DC,4,0,0,0,1,1]},
          {'id':3,'kind':4,'ns':80,'owner':2,'words':[]}]
  a=assess(events,{'clock':'monotonic_ns','dropped':0})
  chain=a['frame_chains'][0]
  self.assertEqual(chain['observed_ppu_union_ns'],60)
  self.assertEqual(len(chain['ppu_scopes']),2)
  self.assertTrue(next(p for p in chain['ppu_scopes'] if p['span']==1)['boundary_censored'])
  self.assertFalse(chain['causal_complete'])
 def test_duplicate_guest_identity_is_unresolved(self):
  events=[{'id':0,'kind':4,'ns':20,'owner':1,'words':[]},{'id':1,'kind':4,'ns':80,'owner':1,'words':[]}]
  a=assess(events,{'clock':'monotonic_ns','dropped':0})
  self.assertEqual(a['frame_chains'],[])
  self.assertTrue(any('guest identity' in s for s in a['unresolved_edges']))
