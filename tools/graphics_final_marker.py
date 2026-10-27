"""Attribute each critical WaitToComplete to the retirement that posted its final completion marker.

Joins the R17 worker attribution kinds (34/35 service spans, 40 claim identity, 41 retirement lane
writes, 42 dependency decrements, 43 descriptor publication) to the PPU wait (kind 1 at 0x23D7D0 with
kind 38 identity) and the seven-lane predicate at 0xA047D8 (kind 39 markers plus the predicate event,
whose last word is the queue line address). Joins that cannot be proven stay explicit as unresolved."""
import argparse,json,statistics
from pathlib import Path
WAIT=0x23D7D0;PRED=0xA047D8;MODULES={0x1f6b930:'MaterialFlushMain',0x1f6ba00:'MaterialFlushDepthNormal'}
def reached(value,target):return ((value-target)&0xFFFF)<0x8000
def lanes(m0,m1):return [m0>>48&0xFFFF,m0>>32&0xFFFF,m0>>16&0xFFFF,m0&0xFFFF,m1>>32&0xFFFF,m1>>16&0xFFFF,m1&0xFFFF]
def stats(a):
 a=sorted(a);return {'n':len(a),'mean_ms':statistics.fmean(a),'median_ms':statistics.median(a),'max_ms':a[-1]} if a else {'n':0}
def assess(events):
 events=sorted(events,key=lambda e:(e['ns'],e['id']));kinds={}
 for e in events:kinds[e['kind']]=kinds.get(e['kind'],0)+1
 pubs={};claims={};writes=[];pstart={};pspans=[];retires=[];deps=[];preds=[];markers=[];waits={};identity={};services={}
 for e in events:
  k,w=e['kind'],e['words']
  if k==43:pubs.setdefault(w[4]&0xFFFFFFFF,[]).append(dict(ns=e['ns'],module=w[0],job=w[1],ticket=w[5],slot_address=w[3]))
  elif k==40:claims.setdefault(e['owner'],[]).append(dict(ns=e['ns'],service=e['span'],queue=w[0],job=w[3]&0xFFFFFFFF,header=w[3],slot=w[4],marker=w[6]))
  elif k==34:services[e['span']]=dict(ns=e['ns'],entry=w[0],owner_span=w[1],parent=w[2],slot=w[5])
  elif k==35 and e['span'] in services:services[e['span']]['end_ns']=e['ns']
  elif k==9 and len(w)==8:writes.append(dict(ns=e['ns'],owner=e['owner'],ea=w[0],before=w[1]>>32,after=w[1]&0xFFFF,pc=w[3]>>32))
  elif k==13:pstart[(e['owner'],w[0])]=e
  elif k==14 and (e['owner'],w[0]) in pstart:pspans.append(dict(owner=e['owner'],start_ns=w[0],end_ns=e['ns'],entry=w[1]))
  elif k==41:retires.append(dict(ns=e['ns'],owner=e['owner'],span=e['span'],line=w[0]+0x40+16*w[1],lane=w[2],before=w[3],after=w[4],records=w[5:8]))
  elif k==42:deps.append(dict(ns=e['ns'],owner=e['owner'],pointer=w[0],after=w[2],notify=w[3]))
  elif k==39:markers.append(dict(ns=e['ns'],owner=e['owner'],object=w[0],array=w[1],target=w[2],result=w[3],lanes=lanes(w[4],w[5]) if w[6]==127 else None,span=w[7]))
  elif k==1 and len(w)==8 and w[1]==WAIT:waits[e['span']]=dict(start_ns=w[0],end_ns=e['ns'],owner=e['owner'],job=w[2])
  elif k==38:identity[e['span']]=dict(module=w[0],job=w[1],ticket=w[5],mutated=bool(w[6]>>63))
  elif len(w)==8 and w[0]==PRED:preds.append(dict(ns=e['ns'],owner=e['owner'],object=w[1],array=w[2],target=w[3],result=w[4],line=w[7]))
 lines={(p['owner'],p['object'],p['array']):p['line'] for p in preds}
 def claim_for(owner,ns):
  c=[x for x in claims.get(owner,[]) if x['ns']<=ns];return c[-1] if c else None
 def publication(job,ns):
  p=[x for x in pubs.get(job,[]) if x['ns']<=ns];return p[-1] if p else None
 rows=[];unresolved=[]
 for token,wait in sorted(waits.items(),key=lambda x:x[1]['start_ns']):
  ident=identity.get(token)
  if not ident or ident['module'] not in MODULES:continue
  row=dict(wait_span=token,module=MODULES[ident['module']],job=hex(ident['job']),ticket=ident['ticket'],duration_ms=(wait['end_ns']-wait['start_ns'])/1e6,identity_mutated=ident['mutated'])
  inside=[m for m in markers if m['owner']==wait['owner'] and wait['start_ns']<=m['ns']<=wait['end_ns']]
  flip=next((m for m in reversed(inside) if m['result']==1),None)
  if not flip:row['unresolved']='no satisfied predicate inside wait';rows.append(row);continue
  line=lines.get((flip['owner'],flip['object'],flip['array']))
  prior=[m for m in inside if m['ns']<flip['ns'] and m['result']==0 and m['lanes']]
  if prior and flip['lanes']:row['straggler_markers']=[i for i,(a,b) in enumerate(zip(prior[-1]['lanes'],flip['lanes'])) if not reached(a,flip['target']) and reached(b,flip['target'])]
  row.update(target=flip['target'],line=hex(line) if line is not None else None,predicate_polls=len(inside))
  if line is None:row['unresolved']='predicate line address not captured';rows.append(row);continue
  final=[r for r in retires if r['line']==line and r['ns']<=flip['ns'] and not reached(r['before'],flip['target']) and reached(r['after'],flip['target'])]
  if not final:
   wr=[x for x in writes if line-0x70<=x['ea']<line+16 and x['ns']<=flip['ns'] and x['after']==0xFFFF and x['before']!=0xFFFF]
   if not wr:row['unresolved']='no retirement or SPU lane clear on predicate line';rows.append(row);continue
   x=wr[-1];ps=[q for q in pspans if q['owner']==x['owner'] and q['end_ns']<=x['ns']]
   row.update(final_writer='spu_lane_clear',final_worker=hex(x['owner']),cleared_ticket=x['before'],clear_to_wake_ms=(flip['ns']-x['ns'])/1e6)
   if ps:
    q=ps[-1];row.update(payload_entry=hex(q['entry']),payload_ms=(q['end_ns']-q['start_ns'])/1e6,payload_share_of_wait=(q['end_ns']-q['start_ns'])/1e6/row['duration_ms'])
   else:row['unresolved']='SPU lane clear without payload span on writer'
   rows.append(row);continue
  r=final[-1];claim=claim_for(r['owner'],r['ns']);row.update(final_marker=r['lane']+1,final_worker=hex(r['owner']),retire_to_wake_ms=(flip['ns']-r['ns'])/1e6)
  if claim:
   pub=publication(claim['job'],claim['ns']);row.update(final_job=hex(claim['job']),final_is_waited_job=claim['job']==ident['job'],claim_to_retire_ms=(r['ns']-claim['ns'])/1e6)
   if pub:row.update(final_module=MODULES.get(pub['module'],hex(pub['module'])),final_ticket=pub['ticket'],publish_to_claim_ms=(claim['ns']-pub['ns'])/1e6)
   else:row['unresolved']='final job publication not captured (retained publisher or before capture)'
   fed=[d for d in deps if d['notify'] and d['ns']<=claim['ns']]
   if fed:row['last_ready_notify_before_claim']=dict(pointer=hex(fed[-1]['pointer']),worker=hex(fed[-1]['owner']),lead_ms=(claim['ns']-fed[-1]['ns'])/1e6)
  else:row['unresolved']='no claim identity on final worker before retirement'
  rows.append(row)
 for row in rows:
  if 'unresolved' in row:unresolved.append(row['unresolved'])
 summary={}
 for name in MODULES.values():
  mine=[r for r in rows if r['module']==name];summary[name]={'waits':len(mine),'resolved':sum('final_job' in r for r in mine),'final_is_waited_job':sum(r.get('final_is_waited_job') is True for r in mine),'final_modules':{m:sum(r.get('final_module')==m for r in mine) for m in {r.get('final_module') for r in mine if r.get('final_module')}},'payload_ms':stats([r['payload_ms'] for r in mine if 'payload_ms' in r]),'lane_clear_resolved':sum('final_writer' in r for r in mine),'payload_entries':sorted({r['payload_entry'] for r in mine if 'payload_entry' in r}),'final_markers':{str(m):sum(r.get('final_marker')==m for r in mine) for m in sorted({r['final_marker'] for r in mine if 'final_marker' in r})}}
  for seg in ('duration_ms','publish_to_claim_ms','claim_to_retire_ms','retire_to_wake_ms'):summary[name][seg]=stats([r[seg] for r in mine if seg in r])
 return {'status':'final-marker attribution','event_counts':kinds,'summary':summary,'waits':rows,'unresolved_counts':{u:unresolved.count(u) for u in set(unresolved)},'qualification':'Claim attribution uses the latest kind-40 claim on the retiring worker context; a worker interleaving claims before retirement would misattribute and is not detected here.'}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 r=assess(json.loads(a.capture.read_text())['events']);a.output.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({'summary':r['summary'],'unresolved_counts':r['unresolved_counts'],'event_counts':r['event_counts']},indent=1))
