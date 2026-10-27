import unittest
from tools.graphics_final_marker import assess,reached
J=0x20d2f40;Q=0x1e82e00;P=0x6000;S=0x7000;DN=0x1f6ba00;LINE=Q+0x40+16*3
def ev(i,ns,kind,words,owner=0,span=0):return dict(id=i,ns=ns,kind=kind,words=words,owner=owner,thread=1,span=span)
def m(v):return (v[0]<<48|v[1]<<32|v[2]<<16|v[3]),(v[4]<<32|v[5]<<16|v[6])
def fixture(lane_before=4):
 low,high=m([5,5,5,lane_before,5,5,5]);done=m([5]*7)
 return [ev(1,100,43,[DN,J,0x1e82e00,0x1e82e28,0x00800001<<32|J,9,0,0],S),
  ev(2,1000,1,[50,0x23D7D0,J,0,0,0,0,1],P,7),ev(3,1000,38,[DN,J,0,0x1e82e00,0,9,0,0],P,7),
  ev(4,150,42,[0x20d3000,0,0,1,0],0x7100),ev(5,200,40,[Q,0,0,0x00800001<<32|J,1,0,0,0],S,11),
  ev(6,300,39,[0x1e82e20,3,5,0,low,high,127,7],P),ev(7,300,0x99,[0xA047D8,0x1e82e20,3,5,0,0,7,LINE],P),
  ev(8,800,41,[Q,3,2,4,5,0,0,0],S,11),
  ev(9,900,39,[0x1e82e20,3,5,1,done[0],done[1],127,7],P),ev(10,900,0x99,[0xA047D8,0x1e82e20,3,5,1,0,7,LINE],P)]
def spu_fixture():
 e=[x for x in fixture() if x['kind']!=41]
 e+=[ev(20,150,13,[500,96816,12896,0,0,0,0,0],0x7100),ev(21,850,14,[500,96816,12896,0,0,0,0,0],0x7100),ev(22,860,9,[LINE+4,(211<<32)|0xFFFF,0xB4,0x3498<<32,0,0,0,0],0x7100)]
 return e
class Tests(unittest.TestCase):
 def test_wrap(self):self.assertTrue(reached(1,0xFFFF));self.assertFalse(reached(0xFFFF,1))
 def test_final_marker_joined(self):
  r=assess(fixture());w=r['waits'][0]
  self.assertEqual((w['final_marker'],w['final_job'],w['final_module'],w['final_is_waited_job'],w['straggler_markers']),(3,hex(J),'MaterialFlushDepthNormal',True,[3]))
  self.assertEqual((w['publish_to_claim_ms'],w['claim_to_retire_ms'],w['retire_to_wake_ms']),(1e-4,6e-4,1e-4))
  self.assertEqual(w['last_ready_notify_before_claim']['pointer'],hex(0x20d3000));self.assertEqual(r['unresolved_counts'],{})
 def test_spu_clear_resolves_payload(self):
  w=assess(spu_fixture())['waits'][0];self.assertEqual((w['final_writer'],w['cleared_ticket'],w['payload_entry'],w['payload_ms']),('spu_lane_clear',211,'0x17a30',3.5e-4));self.assertNotIn('unresolved',w)
 def test_unproven_writer_stays_unresolved(self):
  r=assess([e for e in fixture() if e['kind']!=41]);self.assertIn('no retirement or SPU lane clear',r['waits'][0]['unresolved'])
 def test_retirement_not_crossing_target_ignored(self):
  ev8=[e for e in fixture() if e['kind']==41][0];ev8['words'][3]=5;r=assess(fixture()[:7]+[ev8]+fixture()[8:]);self.assertIn('unresolved',r['waits'][0])
if __name__=='__main__':unittest.main()
