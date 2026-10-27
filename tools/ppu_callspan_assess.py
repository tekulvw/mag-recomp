"""Private fixed-prefix PPU span assessor. No target/process operations."""
import argparse,collections,hashlib,json,os,pathlib,re,stat
MAX_BYTES=64*1024*1024
MAX_RECORDS=32768
FIELDS=set('seq run_id token phase us ctx thread target depth flags r3 r4 r5 r6 r7 r8 r9 r10 r31'.split())
AUDIO_FIELDS=set('seq kind us begin_us copy_us port epoch index ea size lsa cmd ctx spu pc mask nz words flags rc arg0 arg1 value'.split())
SUMMARY=set('run_id target start_us duration_us count dropped limit pending_recorded_brackets completion'.split())
HEX={'ctx','target'}
def distribution(v):
 v=sorted(v)
 if not v:return {'count':0}
 return {'count':len(v),'min':v[0],'max':v[-1],'mean':sum(v)/len(v),'p50':v[(len(v)-1)*50//100],'p95':v[(len(v)-1)*95//100],'p99':v[(len(v)-1)*99//100]}
def fields(s,required=None,hexkeys=HEX,textkeys={'completion','reason','coverage'}):
 pieces=s.split();d={}
 for x in pieces:
  if x.count('=')!=1:raise ValueError('Malformed field')
  k,v=x.split('=')
  if k in d:raise ValueError('Duplicate field '+k)
  if k in textkeys:d[k]=v
  else:
   if not re.fullmatch('[0-9a-fA-F]+' if k in hexkeys else ('-?[0-9]+' if k=='rc' else '[0-9]+'),v):raise ValueError('Invalid unsigned field '+k)
   n=int(v,16 if k in hexkeys else 10)
   if n>2**64-1 or (k=='rc' and not -2**31<=n<2**31):raise ValueError('Integer overflow')
   d[k]=n
 if required is not None and set(d)!=required:raise ValueError('Unexpected/missing schema fields')
 return d

def assess_text(text):
 errors=[];warnings=[];error_count=0
 def err(s):
  nonlocal error_count
  error_count+=1
  if len(errors)<32:errors.append(s)
 sums=[];rows=[];audio=[];dma=[];consume=[];oldseq=[];oldtimes=[]
 if text and not text.endswith('\n'):
  if text.splitlines()[-1].startswith(('[ppu-call-span','[audio-chronology')):err('Incomplete final metadata line')
  else:warnings.append('Partial unrelated trailing application line ignored')
 for line in text.splitlines():
  try:
   if line.startswith('[ppu-call-span-summary] '):sums.append(fields(line[24:],SUMMARY))
   elif line.startswith('[ppu-call-span] '):
    if len(rows)>=MAX_RECORDS:raise ValueError('Span record cap exceeded')
    rows.append(fields(line[16:],FIELDS))
   elif line.startswith('[ppu-call-span'):raise ValueError('Partial/malformed span prefix')
   elif line.startswith('[audio-chronology-summary] '):audio.append(fields(line[len('[audio-chronology-summary] '):],hexkeys=set()))
   elif line.startswith('[audio-chronology] '):
    d=fields(line[len('[audio-chronology] '):],required=AUDIO_FIELDS,hexkeys={'ea','lsa','cmd','ctx','pc','mask'},textkeys=set())
    if len(oldseq)>=MAX_RECORDS:raise ValueError('Audio record cap exceeded')
    oldseq.append(d['seq']);oldtimes.append(d['us'])
    if d.get('kind')==6 and d.get('rc')==0 and d.get('flags',0)&4:dma.append(d)
    elif d.get('kind')==2:consume.append(d)
  except ValueError as e:err(str(e))
 if len(sums)!=1:err('Exactly one PPU summary required; missing summary means unavailable telemetry, not zero callbacks')
 s=sums[0] if len(sums)==1 else None
 if len(audio)>1:err('Multiple audio capture summaries')
 old=audio[0]if len(audio)==1 else None
 if old and (old.get('count')!=len(oldseq) or oldseq!=list(range(1,len(oldseq)+1))):err('Truncated/duplicate audio buffer: count comparison disabled')
 if s:
  if s['completion']!='PPU-fn-and-trampoline-only':err('Unsupported completion semantics')
  if not 0<s['duration_us']<=10000000 or not 0<s['limit']<=MAX_RECORDS or s['count']>s['limit']:err('Invalid capture bounds')
  if not s['run_id'] or s['run_id']!=s['start_us'] or not 0<s['target']<=0xfffffffc or s['target']%4:err('Invalid run/target')
  if s['count']!=len(rows)or [r['seq']for r in rows]!=list(range(1,len(rows)+1)):err('Truncated/duplicated/noncontiguous span buffer')
  if s['dropped']:warnings.append('Dropped span records: no complete-window rate inference')
  if s['count']==s['limit']:warnings.append('Span buffer reached its limit')
  if old and (old.get('start_us')!=s['start_us']or old.get('duration_us')!=s['duration_us']):err('Audio/span window mismatch')
  if old and any(not s['start_us']<=t<s['start_us']+s['duration_us'] for t in oldtimes):err('Audio event outside shared window')
  if any(not s['start_us']<=d.get('begin_us',0)<=d.get('copy_us',0)<=d['us']<s['start_us']+s['duration_us'] for d in dma):err('Copied DMA timestamp bracket outside shared window')
  if old and old.get('reason')!='window':warnings.append('Audio capture ended before confirmed window deadline')
 starts={};ends={};pairs=[];last_us=0
 for r in rows:
  if r['phase']not in (0,1)or not r['token']or r['depth']>65535 or r['flags']not in(0,1):err('Invalid phase/token/depth/flags');continue
  if r['phase']==0 and r['flags']:err('Start marked abnormal')
  if s and (r['run_id']!=s['run_id']or r['target']!=s['target']or not s['start_us']<=r['us']<s['start_us']+s['duration_us']):err('Record outside run/target/window')
  if r['us']<last_us:err('Regressing serialized clock')
  last_us=r['us'];bucket=starts if r['phase']==0 else ends
  if r['token']in bucket:err('Duplicate token phase');continue
  bucket[r['token']]=r
  if r['phase']==1:
   begin=starts.get(r['token'])
   if not begin:err('End without observed preceding start');continue
   if r['seq']<=begin['seq']or r['us']<begin['us']:err('Invalid pair sequence/time');continue
   if any(r[k]!=begin[k]for k in FIELDS-{'seq','phase','us','flags'}):err('Pair context/thread/arguments/depth mismatch');continue
   pairs.append((begin,r))
 pending=[r for token,r in starts.items()if token not in ends]
 if s and s['pending_recorded_brackets']!=len(pending):err('Summary pending bracket mismatch')
 if starts and sorted(starts)!=list(range(1,len(starts)+1)):err('Noncontiguous admitted start tokens')
 if pending:warnings.append('Unclosed recorded starts: deadline/abnormal boundary not physical in-flight proof')
 rates_allowed=bool(s and old and old.get('reason')=='window' and not error_count and not s['dropped'])
 groups={}
 for flags in sorted({r['r8']for r in starts.values()}):
  rr=[r for r in starts.values()if r['r8']==flags];pp=[(b,e)for b,e in pairs if b['r8']==flags];times=[r['us']for r in rr];normal=[e['us']-b['us']for b,e in pp if not e['flags']]
  groups[str(flags)]={'bit_0x2_set':bool(flags&2),'starts':len(rr),'normal_ends':sum(not e['flags']for b,e in pp),'abnormal_ends':sum(bool(e['flags'])for b,e in pp),'unclosed_starts':sum(r['r8']==flags for r in pending),'normal_duration_us':distribution(normal),'start_spacing_us':distribution([y-x for x,y in zip(times,times[1:])]),'incoming_frame_counts':dict(collections.Counter(str(r['r5'])for r in rr)),'observed_starts_per_window_second':len(rr)*1000000/s['duration_us']if rates_allowed else None,'cohort_count':len({(r['ctx'],r['thread'],r['target'],r['r7'],r['r31'])for r in rr})}
 comparable=bool(rates_allowed and old and old.get('dropped')==0)
 seconds=[]
 if comparable:
  for second in range((s['duration_us']+999999)//1000000):
   lo=s['start_us']+second*1000000;hi=min(lo+1000000,s['start_us']+s['duration_us']);ss=[r for r in starts.values()if lo<=r['us']<hi];dd=[r for r in dma if lo<=r.get('copy_us',0)<hi];cc=[r for r in consume if lo<=r['us']<hi];seconds.append({'second':second,'observed_starts':len(ss),'observed_normal_ends':sum(lo<=e['us']<hi and not e['flags']for b,e in pairs),'copied_PUT_records':len(dd),'consume_samples':len(cc),'raw_r8_start_counts':dict(collections.Counter(str(r['r8'])for r in ss))})
 return {'errors':errors,'error_count':error_count,'warnings':warnings,'summary':s,'counts':{'starts':len(starts),'normal_ends':sum(not e['flags']for b,e in pairs),'abnormal_ends':sum(bool(e['flags'])for b,e in pairs),'unclosed_starts':len(pending),'copied_PUT_records':len(dma),'consume_samples':len(consume)},'rate_inference_allowed':rates_allowed,'same_window_audio_comparison_allowed':comparable,'durations_us':distribution([e['us']-b['us']for b,e in pairs if not e['flags']]),'start_spacing_us':distribution([y['us']-x['us']for x,y in zip(list(starts.values()),list(starts.values())[1:])]),'flag_groups':groups,'pending_examples':[{'token':r['token'],'raw_r8':r['r8'],'start_us':r['us'],'remaining_to_deadline_us':s['start_us']+s['duration_us']-r['us']if s else None}for r in pending[:16]],'per_second':seconds,'qualifications':['Normal end is PPU fn+all-trampoline completion, not asynchronous SPU job completion.','Raw entry arguments are opaque; matching times/addresses do not establish job ownership or generation.','Durations include diagnostic bookkeeping/metadata-lock overhead and wall waits.','All rates describe admitted records in this bounded window; flags are grouped per record, never assumed uniform.','Pending starts are not automatically deadlines or still-running calls; signals/longjmp may omit scope ends.','Audio comparison is same-clock counts only; old audio record semantic validation remains the accepted separate assessor.']}

def assess_file(path):
 p=pathlib.Path(path)
 if p.is_symlink():raise ValueError('Symlink input')
 with p.open('rb')as f:
  before=os.fstat(f.fileno())
  if not stat.S_ISREG(before.st_mode) or before.st_size>MAX_BYTES:raise ValueError('Input must be a regular file within64MiB cap')
  raw=f.read(MAX_BYTES+1);after=os.fstat(f.fileno())
 if len(raw)!=before.st_size or (before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns)!=(after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns):raise ValueError('Input changed during fixed-prefix read')
 r=assess_text(raw.decode('utf8'));r['input']={'path':str(p.resolve()),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()};return r

def main():
 ap=argparse.ArgumentParser();ap.add_argument('input');ap.add_argument('--output',required=True);args=ap.parse_args();r=assess_file(args.input)
 with pathlib.Path(args.output).open('x')as f:json.dump(r,f,indent=2);f.write('\n')
 print(json.dumps({'errors':r['error_count'],'counts':r['counts'],'input_sha256':r['input']['sha256']}));return 2 if r['error_count']else 0
if __name__=='__main__':raise SystemExit(main())
