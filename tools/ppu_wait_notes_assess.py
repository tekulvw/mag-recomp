"""Private bounded fixed-prefix wait-note reader; no process/device operations."""
import argparse, collections, hashlib, json, os, pathlib, stat
if __package__:
    from . import ppu_callspan_assess as span
else:
    import ppu_callspan_assess as span
MAX_BYTES=64*1024*1024
MAX_ROWS=32768
UINT64_MAX=2**64-1
NOTE_FIELDS=set('seq run_id span token us ctx thread kind phase rc arg0 arg1 arg2 arg3'.split())
AUDIO_SUMMARY=set('reason count dropped limit start_us duration_us queue_min queue_zero max_submit_interval_us inflight_at_flush coverage overlap_is_ambiguous'.split())
NOTE_SUMMARY=set('run_id start_us duration_us count dropped limit pending_recorded_sleeps coverage'.split())
def workload(rows):
 good=[r for r in rows if r['rc']==0];values=collections.Counter((r['arg3']&255,(r['arg3']>>8)&255,(r['arg3']>>16)&255)for r in good)
 top=values.most_common(16)
 return {'calls':len(rows),'success':len(good),'errors':len(rows)-len(good),'result_counts':dict(collections.Counter(str(r['rc'])for r in rows)),
 'ready_nonzero':sum(bool(r['arg3']&0xff00)for r in good),'contention_nonzero':sum(bool(r['arg3']&255)for r in good),'idle_nonzero':sum(bool(r['arg3']&0xff0000)for r in good),
 'common_value_triples':[{'contention':v[0],'ready':v[1],'idle':v[2],'count':n}for v,n in top],'other_value_triples_count':sum(values.values())-sum(n for v,n in top)}
def assess_text(text):
 # Bound the retained span assessor's flag grouping before invoking it.
 if len(text)>MAX_BYTES:return {'errors':['Text cap exceeded'],'error_count':1,'normal_span_metrics_allowed':False,'complete_window_inference_allowed':False}
 counts=collections.Counter();flags=set()
 for line in text.splitlines():
  for prefix in ('[ppu-call-span] ','[ppu-call-note] ','[audio-chronology] '):
   if line.startswith(prefix):counts[prefix]+=1
  if line.startswith('[ppu-call-span] '):
   try:flags.add(span.fields(line[len('[ppu-call-span] '):],span.FIELDS)['r8'])
   except ValueError:pass
 if any(n>MAX_ROWS for n in counts.values())or len(flags)>128:return {'errors':['Record or raw-flag diversity cap exceeded'],'error_count':1,'normal_span_metrics_allowed':False,'complete_window_inference_allowed':False}
 old=span.assess_text(text);errors=list(old['errors']);error_count=old['error_count'];warnings=list(old['warnings'])
 def err(message):
  nonlocal error_count
  error_count+=1
  if len(errors)<32:errors.append(message)
 sums=[];audio_sums=[];rows=[];starts={};ends={}
 if text and not text.endswith('\n')and text.splitlines()[-1].startswith('[ppu-call-note'):err('Incomplete final note metadata line')
 for line in text.splitlines():
  try:
   if line.startswith('[audio-chronology-summary] '):
    if len(audio_sums)>=2:raise ValueError('Too many audio summaries')
    audio_sums.append(span.fields(line[len('[audio-chronology-summary] '):],AUDIO_SUMMARY,hexkeys=set(),textkeys={'reason','coverage'}))
   elif line.startswith('[audio-chronology-summary'):raise ValueError('Malformed audio summary prefix')
   elif line.startswith('[ppu-call-note-summary] '):
    if len(sums)>=2:raise ValueError('Too many note summaries')
    sums.append(span.fields(line[len('[ppu-call-note-summary] '):],NOTE_SUMMARY,hexkeys=set(),textkeys={'coverage'}))
   elif line.startswith('[ppu-call-note] '):
    if len(rows)>=MAX_ROWS:raise ValueError('Note record cap exceeded')
    rows.append(span.fields(line[len('[ppu-call-note] '):],NOTE_FIELDS,hexkeys={'ctx'},textkeys=set()))
   elif line.startswith('[ppu-call-note'):raise ValueError('Malformed note prefix')
   elif line.startswith('[ppu-call-span] '):
    d=span.fields(line[len('[ppu-call-span] '):],span.FIELDS)
    if len(starts)+len(ends)>=MAX_ROWS:raise ValueError('Span record cap exceeded')
    if d['phase']==0:starts[d['token']]=d
    elif d['phase']==1:ends[d['token']]=d
  except ValueError as e:err(str(e))
 if len(sums)!=1:err('Exactly one note summary required; missing is unavailable telemetry, not zero waits')
 s=sums[0]if len(sums)==1 else None;ss=old['summary']
 if len(audio_sums)!=1:err('Exactly one complete production audio summary required')
 audio=audio_sums[0]if len(audio_sums)==1 else None
 if audio:
  if audio['reason']not in ('window','shutdown')or audio['coverage']!='SPU-DMA-only'or audio['overlap_is_ambiguous']!=1:err('Unsupported audio summary semantics')
  if any(audio[k]>2**32-1 for k in ('count','dropped','limit','queue_min','queue_zero'))or not 0<audio['limit']<=MAX_ROWS or audio['count']>audio['limit']:err('Invalid audio summary bounds')
  if s and any(audio[k]!=s[k]for k in ('start_us','duration_us','limit')):err('Audio/note capture mismatch')
  if audio['dropped']:warnings.append('Dropped audio records: shared complete-prefix metrics disabled')
 if s:
  if s['coverage']!='existing-sleep-and-workload-info-only':err('Unsupported note coverage')
  if not 0<s['duration_us']<=10000000 or not 0<s['limit']<=MAX_ROWS or s['count']>s['limit']or s['dropped']>2**32-1 or s['pending_recorded_sleeps']>MAX_ROWS:err('Invalid note bounds')
  if not s['run_id']or s['run_id']!=s['start_us']or s['start_us']>UINT64_MAX-s['duration_us']:err('Invalid note run')
  if ss and any(s[k]!=ss[k]for k in ('run_id','start_us','duration_us','limit')):err('Note/span capture mismatch')
  if s['count']!=len(rows)or [r['seq']for r in rows]!=list(range(1,len(rows)+1)):err('Truncated/duplicate/noncontiguous note buffer')
  if s['dropped']:warnings.append('Dropped note records: complete-span wait metrics disabled')
 sleeps={};sleep_ends={};pairs=[];apis=[];last_us=0
 for r in rows:
  if r['us']<last_us:err('Regressing note clock')
  last_us=r['us']
  if any(r[k]>2**32-1 for k in ('seq','kind','phase')):err('Overflowing 32-bit note field')
  if s and(r['run_id']!=s['run_id']or not s['start_us']<=r['us']<s['start_us']+s['duration_us']):err('Note outside run/window')
  b=starts.get(r['span']);e=ends.get(r['span'])
  if not b:err('Note without observed selected span start')
  elif any(r[k]!=b[k]for k in ('run_id','ctx','thread'))or r['us']<b['us']or(e and r['us']>e['us']):err('Note outside selected span/cohort interval')
  if r['kind']==1:
   if r['phase']not in(0,1)or not r['token']or r['arg2']or r['arg3']:err('Invalid sleep note shape');continue
   if r['phase']==0:
    if r['rc']or r['arg1']:err('Sleep begin contains result')
    if r['token']in sleeps:err('Duplicate sleep start token')
    else:sleeps[r['token']]=r
   else:
    if r['rc']not in(0,-1,-2**31)or (r['rc']!=-1 and r['arg1'])or (r['rc']==-1 and not 0<r['arg1']<=2**31-1):err('Invalid host sleep result/errno')
    if r['token']in sleep_ends:err('Duplicate sleep end token');continue
    sleep_ends[r['token']]=r;b=sleeps.get(r['token'])
    if not b:err('Sleep end without observed start');continue
    if r['seq']<=b['seq']or r['us']<b['us']or any(r[k]!=b[k]for k in ('run_id','span','ctx','thread','arg0')):err('Sleep token pair identity/time mismatch');continue
    pairs.append((b,r))
  elif r['kind']==2:
   if r['phase']!=2 or r['token']or r['arg1']>2**32-1:err('Invalid workload note phase/token')
   if (r['rc']==0 and(r['arg3']>0xffffff or not r['arg0']or not r['arg2']or r['arg1']>=16))or(r['rc']!=0 and r['arg3']!=UINT64_MAX):err('Invalid workload success/error values')
   apis.append(r)
  else:err('Unknown note kind')
 pending=[r for t,r in sleeps.items()if t not in sleep_ends]
 if sleeps and sorted(sleeps)!=list(range(1,len(sleeps)+1)):err('Noncontiguous sleep start tokens')
 if s and s['pending_recorded_sleeps']!=len(pending):err('Pending recorded sleep mismatch')
 if pending:warnings.append('Censored sleep starts have no fabricated end; not physical sleeping-thread counts')
 normal={t:b for t,b in starts.items()if t in ends and not ends[t]['flags']}
 complete=bool(s and ss and audio and not error_count and not s['dropped']and not ss['dropped']and not audio['dropped'])
 eligible=[(b,e)for b,e in pairs if b['span']in normal]
 # A normal span with any unmatched stored sleep is incomplete (e.g. end cap loss).
 censored_spans={r['span']for r in pending};normal={t:b for t,b in normal.items()if t not in censored_spans}
 eligible=[(b,e)for b,e in eligible if b['span']in normal]
 pairs_by_span=collections.defaultdict(list);api_by_span=collections.defaultdict(list)
 for b,e in eligible:pairs_by_span[b['span']].append((b,e))
 for r in apis:api_by_span[r['span']].append(r)
 per_span=[];group_data={};cohorts=set();cohort_data={}
 for t,b in normal.items():
  pp=pairs_by_span[t];aa=api_by_span[t];duration=ends[t]['us']-b['us'];sleep_sum=sum(y['us']-x['us']for x,y in pp)
  ordered=sorted(pp,key=lambda pair:(pair[0]['us'],pair[0]['seq']));previous=None
  for x,y in ordered:
   if previous is not None and x['us']<previous:err('Overlapping sleep intervals inside one selected scope')
   previous=y['us']
  if sleep_sum>duration:err('Sleep sum exceeds selected span wall interval')
  cohort=(b['ctx'],b['thread'],b['target'],b['r7'],b['r31']);cohorts.add(cohort)
  d={'span':t,'raw_r8':b['r8'],'duration_us':duration,'direct_sleep_us':sleep_sum,'direct_sleep_count':len(pp),'workload_calls':len(aa),'ctx':format(b['ctx'],'X'),'thread':b['thread'],'r7':b['r7'],'r31':b['r31']}
  per_span.append(d)
  cg=cohort_data.setdefault(cohort,{'normal_spans':0,'span_wall_us':0,'direct_sleep_us':0,'direct_sleep_count':0,'workload_calls':0,'raw_r8_counts':collections.Counter()});cg['normal_spans']+=1;cg['span_wall_us']+=duration;cg['direct_sleep_us']+=sleep_sum;cg['direct_sleep_count']+=len(pp);cg['workload_calls']+=len(aa);cg['raw_r8_counts'][str(b['r8'])]+=1
  g=group_data.setdefault(str(b['r8']),{'spans':[],'sleeps':[],'api':[],'totals':[],'cohorts':set()});g['spans'].append(duration);g['sleeps'] += [y['us']-x['us']for x,y in pp];g['api']+=aa;g['totals'].append(sleep_sum);g['cohorts'].add(cohort)
 # Errors discovered while checking interval unions also disable all derived metrics.
 complete=complete and not error_count
 groups={k:{'normal_spans':len(g['spans']),'cohort_count':len(g['cohorts']),'bit_0x2_set':bool(int(k)&2),'normal_span_duration_us':span.distribution(g['spans'])if complete else None,'direct_sleep_duration_us':span.distribution(g['sleeps'])if complete else None,'direct_sleep_sum_us':sum(g['totals'])if complete else None,'normal_span_sum_us':sum(g['spans'])if complete else None,'workload':workload(g['api'])if complete else None}for k,g in group_data.items()}
 return {'errors':errors,'error_count':error_count,'warnings':warnings,'summary':s,'span_summary':ss,'audio_summary':audio,'counts':{'note_records':len(rows),'sleep_starts':len(sleeps),'sleep_ends':len(sleep_ends),'paired_sleeps':len(pairs),'censored_sleeps':len(pending),'eligible_normal_span_sleeps':len(eligible),'workload_calls':len(apis),'normal_spans_without_censored_sleeps':len(normal)},'normal_span_metrics_allowed':bool(complete),'complete_window_inference_allowed':bool(complete and old['rate_inference_allowed']and not pending and not old['counts']['unclosed_starts']and not old['counts']['abnormal_ends']),'sleep_duration_us':span.distribution([e['us']-b['us']for b,e in eligible])if complete else None,'workload':workload([r for r in apis if r['span']in normal])if complete else None,'workload_scope':'matched-normal-spans-without-censored-sleeps','cohort_groups':[{'ctx':format(k[0],'X'),'thread':k[1],'target':format(k[2],'08X'),'r7':k[3],'r31':k[4],**v}for k,v in cohort_data.items()]if complete else [],'flag_groups':groups,'cohort_count':len(cohorts),'normal_span_examples':per_span[:32]if complete else [],'censored_sleep_examples':[{'token':r['token'],'span':r['span'],'requested_us':r['arg0'],'begin_us':r['us']}for r in pending[:16]],'qualifications':['Only matched normal selected spans are eligible; pending/abnormal spans and censored sleeps never receive invented end times.','Sleep wall duration includes observer overhead and scheduling, not CPU time or pure kernel sleep time.','Direct sleep totals belong to the current selected TLS scope; parent spans can include nested selected spans, so totals are not exclusive workload costs.','Workload values reuse already-performed loads; they are not an atomic snapshot, a completion-marker read, or proof of the particular guest caller.','No workload rows does not distinguish unfinished marker minimum from unstable terminal scan; note loss and boundary incompleteness invalidate absence inference.','Same cohort is entry context/thread/target/r7/r31 only; no job identity, generation, SPU completion, audio intent, or CPU-store absence follows.','No recorded sleep end is a censored observation, not proof the host or guest still sleeps.']}
def assess_file(path):
 p=pathlib.Path(path)
 if p.is_symlink():raise ValueError('Symlink input')
 try:fd=os.open(p,os.O_RDONLY|getattr(os,'O_NOFOLLOW',0))
 except OSError as e:raise ValueError('Cannot open regular input')from e
 with os.fdopen(fd,'rb')as f:
  before=os.fstat(f.fileno())
  if not stat.S_ISREG(before.st_mode)or before.st_size>MAX_BYTES:raise ValueError('Input must be a regular fixed prefix within64MiB')
  raw=f.read(MAX_BYTES+1);after=os.fstat(f.fileno())
 if len(raw)!=before.st_size or(before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns)!=(after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns):raise ValueError('Input changed during read')
 result=assess_text(raw.decode('utf8'));result['input']={'path':str(p.resolve()),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()};return result
def main():
 ap=argparse.ArgumentParser();ap.add_argument('input');ap.add_argument('--output',required=True);a=ap.parse_args();r=assess_file(a.input)
 with pathlib.Path(a.output).open('x')as f:json.dump(r,f,indent=2);f.write('\n')
 print(json.dumps({'errors':r['error_count'],'counts':r['counts'],'input_sha256':r['input']['sha256']}));return 2 if r['error_count']else 0
if __name__=='__main__':raise SystemExit(main())
