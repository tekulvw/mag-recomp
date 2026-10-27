import copy
import unittest
from tools.derive_main_water_selector import derive


def fixture(empty=False):
    root, phase = 3, 91
    b=dict(event='native_program_identity_water_interval',root=root,source_phase=phase,operations=20,fragments=2,source_marker_kind=1,source_marker_method=0x1bcc,source_marker_value=0,source_group13_call=0x100,begin_operation=3,end_operation=12,selected=0 if empty else 1,geometry_operations=0 if empty else 1,draw_operations=0 if empty else 1,inherited_draw_operations=0,complete=True)
    e=[dict(event='session',pid=100,session='owned'),b,dict(event='native_program_identity_water_complete',root=root,selected=b['selected'],published=b['selected'],complete=True),dict(event='gpu_receipt',root=root,ticket=7,report_generation=3)]
    e += [dict(event='native_fp',root=root,index=i,phase=phase,location=1,offset=0x1000+128*i,bytes=128)for i in range(2)]
    if not empty:e += [dict(event='native_program_identity_water_fragment',root=root,index=1,structural_hash='1234567890ABCDEF',code_bytes=32),dict(event='native_program_identity_water_control',root=root,index=1,status='present',value=0x04008400)]
    return e


class WaterSelector(unittest.TestCase):
    def test_selected_own_root(self):
        d=derive(fixture(),3);self.assertEqual(d['selectors'][0]['fragment'],1);self.assertEqual(d['source_phase'],91);self.assertFalse(d['empty'])
    def test_complete_empty_census(self):
        d=derive(fixture(True),3);self.assertTrue(d['empty']);self.assertEqual(d['selectors'],[])
    def test_refuse_ambiguous(self):
        mutations=[lambda e:e.append(e[1].copy()),lambda e:e[1].update(source_marker_kind=2),lambda e:e[1].update(source_marker_value=1),lambda e:e[1].update(begin_operation=12),lambda e:e[2].update(complete=False),lambda e:e[3].update(root=1),lambda e:e[4].update(phase=92),lambda e:e[-1].update(status='inherited'),lambda e:e[-2].update(index=0),lambda e:e[-1].update(index=0),lambda e:e[1].update(inherited_draw_operations=1),lambda e:e.append(dict(event='overflow')),lambda e:e[5].update(offset=0x1001),lambda e:e[1].update(draw_operations=12)]
        for mutate in mutations:
            e=fixture();mutate(e)
            with self.assertRaises(ValueError):derive(e,3)
    def test_no_cross_root_alias(self):
        with self.assertRaises(ValueError):derive(fixture(),1)

if __name__=='__main__':unittest.main()
