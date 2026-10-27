import copy,importlib.util
from pathlib import Path
s=importlib.util.spec_from_file_location('q',Path(__file__).resolve().parents[1]/'tools/derive_main_sky_selector.py');q=importlib.util.module_from_spec(s);s.loader.exec_module(q)
e=[dict(event='session',pid=42,session='synthetic')]
def add(event_kind,**kw):e.append(dict(event='native_program_identity_'+event_kind,root=1,**kw))
add('begin',operations=10,fragments=2,entries=5,source_group13_call=0x100,source_group15_call=0x200,source_group15_second_call=0x300,source_phase=7)
for j in range(2):
 add('fragment',index=j,structural_hash='123456789ABCDEF0',code_bytes=32);add('control',index=j,status='present',value=0x02008400);e.append(dict(event='native_fp',root=1,index=j,phase=7))
for op,kind,index,target in [(0,2,0,0x100),(1,6,0,0),(2,2,2,0x200),(3,2,3,0x300),(4,6,1,0)]:add('order',operation=op,kind=kind,index=index,target=target)
add('prepared',complete=True,fragments=2,entries=5);e.append(dict(event='gpu_receipt',root=1,ticket=3,report_generation=1))
assert q.derive(e)['selectors'][0]['selector']=='123456789ABCDEF0:02008400'
def empty_interval(d):
 orders=[x for x in d if x.get('event')=='native_program_identity_order']
 orders[1]['operation']=4;orders[4]['operation']=5
 slots=[i for i,x in enumerate(d)if x.get('event')=='native_program_identity_order']
 for i,x in zip(slots,sorted(orders,key=lambda x:x['operation'])):d[i]=x
for name,fn in [
 ('empty interval',empty_interval),
 ('alias',lambda d:d[1].update(source_group15_call=0x100)),
 ('overflow',lambda d:d.append(dict(event='overflow'))),
 ('absent receipt',lambda d:d.pop()),
 ('inherited group13',lambda d:next(x for x in d if x.get('event')=='native_program_identity_control').update(status='inherited')),
 ('truncated',lambda d:d.pop(2)),
 ('phase mismatch',lambda d:next(x for x in d if x.get('event')=='native_fp').update(phase=8)),
 ('getter refusal',lambda d:d.append(dict(event='native_program_identity_refused'))),
]:
 d=copy.deepcopy(e);fn(d)
 try:q.derive(d)
 except (ValueError,KeyError):pass
 else:raise AssertionError(name)
print('Main sky identity derivation: accepted bounded source interval + 8 rejection cases PASS')

# A later captured root owns its own interval/index/phase/allocation/control.
e2=copy.deepcopy(e)
e2.extend([
 dict(event='native_program_identity_sky_interval',root=2,source_phase=99,operations=20,fragments=1,entries=4,source_group13_call=0x400,source_group15_call=0x500,source_group15_second_call=0x600,begin_operation=5,end_operation=10,selected=1,complete=True),
 dict(event='native_program_identity_sky_fragment',root=2,index=0,structural_hash='FEDCBA9876543210',code_bytes=32),
 dict(event='native_program_identity_sky_control',root=2,index=0,status='present',value=0x04008400),
 dict(event='native_program_identity_sky_complete',root=2,selected=1,published=1,complete=True),
 dict(event='native_fp',root=2,index=0,phase=99,offset=0x8000,location=1,bytes=128),
 dict(event='gpu_receipt',root=2,ticket=7,report_generation=2),
])
assert q.derive_for_root(e2,2)['selectors'][0]['offset']==0x8000
for name,event,patch in [('crossroot phase','native_fp',dict(phase=7)),('unknown control','native_program_identity_sky_control',dict(status='inherited')),('unfinished','native_program_identity_sky_complete',dict(complete=False)),('wrong count','native_program_identity_sky_complete',dict(published=0)),('unmapped','native_fp',dict(location=0)),('bad source alias','native_program_identity_sky_interval',dict(source_group15_call=0x400))]:
 d=copy.deepcopy(e2);next(x for x in d if x.get('root')==2 and x['event']==event).update(patch)
 try:q.derive_for_root(d,2)
 except (ValueError,KeyError):pass
 else:raise AssertionError(name)
print('Captured-root sky association: own interval/span + six cross-root/publication refusals PASS')
