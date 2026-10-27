"""Conservative initial trace assessor; producer and presentation joins stay explicit."""
import argparse,json,statistics
from pathlib import Path
def interval_union(intervals):
 total=0;end=None
 for a,b in sorted(intervals):
  if end is None or a>end:total+=b-a;end=b
  elif b>end:total+=b-end;end=b
 return total
def assess(events,manifest):
 events=sorted(events,key=lambda e:(e['ns'],e['id']));loss=manifest.get('dropped',0)+manifest.get('unpublished',0)
 unresolved=['successful job publication/claim/payload joins not yet captured','PPU wait to guest-flip causal join not yet captured','context and array incarnations unresolved']
 if manifest.get('clock')!='monotonic_ns':unresolved.append('clock domain mismatch')
 if loss:unresolved.append('lost events')
 bykind={}
 for e in events:bykind[e['kind']]=bykind.get(e['kind'],0)+1
 ppu=[];reports=[];flush=[];flips=[]
 for e in events:
  w=e['words']
  if e['kind']==1 and len(w)==8:ppu.append(dict(event_id=e['id'],function=hex(w[1]),start_ns=w[0],end_ns=e['ns'],duration_ns=e['ns']-w[0],object=w[2],arguments=w[2:5],context=e['owner'],thread=e['thread'],span=e['span'],parent=w[6],genuine_exit=bool(w[7])))
  if e['kind']==2 and w:reports.append((w[0],e['ns'],e['id'],w[1:]))
  if e['kind']==3 and w:flush.append((w[0],e['ns'],e['id'],w[1:]))
  if e['kind']==4:flips.append(e)
 chains=[]
 for a,b in zip(flips,flips[1:]):
  start=a['ns'];end=b['ns'];
  if not a.get('owner') or b.get('owner',0)<=a.get('owner',0) or end<=start:
   unresolved.append('duplicate/nonmonotonic guest identity or time');continue
  waits=[dict(p,boundary_censored=p['start_ns']<start or p['end_ns']>end,intersection_ns=max(0,min(end,p['end_ns'])-max(start,p['start_ns']))) for p in ppu if p['start_ns']<end and p['end_ns']>start]
  chains.append({'guest_events':[a['id'],b['id']],'start_ns':start,'end_ns':end,'duration_ns':end-start,'ppu_scopes':waits,'observed_ppu_union_ns':interval_union([(max(start,p['start_ns']),min(end,p['end_ns'])) for p in waits]),'report_count':sum(start<=r[0]<end for r in reports),'flush_count':sum(start<=r[0]<end for r in flush),'causal_complete':False})
 durations=[c['duration_ns'] for c in chains]
 selected=[]
 if chains:
  med=statistics.median(durations);selected=[min(chains,key=lambda c:abs(c['duration_ns']-med)),max(chains,key=lambda c:c['duration_ns'])]
 return {'status':'incomplete','frame_chains':chains,'representative_chains':selected,'unresolved_edges':unresolved,'lost_events':loss,'clock_domains':{'events':manifest.get('clock'),'presentation':'CA seconds; bracket records required'},'candidate_costs':{'ppu_scopes_longest':sorted(ppu,key=lambda p:p['duration_ns'],reverse=True)[:20],'reports_total_ns':sum(e-s for s,e,*_ in reports),'flush_total_ns':sum(e-s for s,e,*_ in flush),'overlap_warning':'totals overlap; not additive critical costs'},'presentation_qualified':False,'event_counts':bykind,'presentation_events':sum(e['kind']==6 for e in events)}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args();m=json.loads(a.capture.read_text());a.output.write_text(json.dumps(assess(m['events'],m),indent=2)+'\n')
