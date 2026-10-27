"""Private offline inspector for existing sink_end observer JSONL. No controls."""
import argparse,collections,hashlib,json,os,pathlib,re,stat
MAX_ROWS=4096
MAX_BYTES=MAX_ROWS*4096
FIELDS=set('record frame outcome primitive packets array_batches index_batches array_vertices requested_indices index_offset index_location index_u32 inline_bytes immediate_vertices fetch_ok source_refs vertices cuts vp_start fp_program shader_control target_select color_offsets color_locations clip attributes'.split())
OUTCOMES=set('entry not_ready empty_packets prefetch mixed_sources topology fetch index_topology zero_geometry targets pipeline picture_readiness index_allocation constant_allocation emitted'.split())
HEX={'fp_program','shader_control'}
ARRAYS={'color_offsets':4,'color_locations':4,'clip':2}
PROGRAM_FIELDS=set('program_identity_observed vp_content_valid vp_content_hash vp_instructions fp_structure_valid fp_structural_hash fp_bytes'.split())
LATE_OUTCOMES=set('pipeline picture_readiness index_allocation constant_allocation emitted'.split())
ZERO_HASH='0000000000000000'
def validate_programs(r):
 for k in ('program_identity_observed','vp_content_valid','fp_structure_valid'):
  if type(r[k])is not int or r[k]not in(0,1):raise ValueError('Invalid identity boolean '+k)
 for k in ('vp_content_hash','fp_structural_hash'):
  if not isinstance(r[k],str)or not re.fullmatch('[0-9A-F]{16}',r[k]):raise ValueError('Invalid identity hash '+k)
 for prefix,valid,hashkey,length,maximum in [('VP','vp_content_valid','vp_content_hash','vp_instructions',544),('FP','fp_structure_valid','fp_structural_hash','fp_bytes',65536)]:
  n=r[length]
  if not uint(n)or n>maximum:raise ValueError('Invalid program length '+length)
  if r[valid]:
   if not n or(prefix=='FP'and(n%16 or r[hashkey]==ZERO_HASH)):raise ValueError('Invalid valid program '+prefix)
  elif n or r[hashkey]!=ZERO_HASH:raise ValueError('Unavailable program has stale data '+prefix)
 if not r['program_identity_observed']and(r['vp_content_valid']or r['fp_structure_valid']):raise ValueError('Unobserved program marked valid')
 if bool(r['program_identity_observed'])!=(r['outcome']in LATE_OUTCOMES):raise ValueError('Identity availability disagrees with source stage')
def program_metrics(rows):
 availability=collections.Counter();groups={}
 for r in rows:
  if 'program_identity_observed'not in r:availability['legacy_unavailable']+=1;continue
  if not r['program_identity_observed']:availability['unavailable']+=1;continue
  if not(r['vp_content_valid']and r['fp_structure_valid']):availability['partial'if r['vp_content_valid']or r['fp_structure_valid']else 'neither_valid']+=1;continue
  availability['both_valid']+=1
  key=(r['target_select'],tuple(r['color_offsets']),tuple(r['color_locations']),r['shader_control'],r['vp_content_hash'],r['vp_instructions'],r['fp_structural_hash'],r['fp_bytes'])
  groups.setdefault(key,[]).append(r)
 out=[]
 for k,rr in groups.items():
  out.append({'target_select':k[0],'color_offsets':list(k[1]),'color_locations':list(k[2]),'shader_control':k[3],'vp_content_hash':k[4],'vp_instructions':k[5],'fp_structural_hash':k[6],'fp_bytes':k[7],'records':len(rr),'record_ids':[r['record']for r in rr],'outcomes':dict(collections.Counter(r['outcome']for r in rr)),'total_vertices':sum(r['vertices']for r in rr),'max_vertices':max(r['vertices']for r in rr),'vertex_counts':[{'vertices':v,'records':n}for v,n in sorted(collections.Counter(r['vertices']for r in rr).items())],'total_requested_indices':sum(r['requested_indices']for r in rr),'raw_fp_program_counts':[{'fp_program':v,'records':n}for v,n in sorted(collections.Counter(r['fp_program']for r in rr).items())],'raw_vp_start_counts':[{'vp_start':v,'records':n}for v,n in sorted(collections.Counter(r['vp_start']for r in rr).items())]})
 return {'identity_availability':dict(availability),'observed_program_groups':out}
def unique(pairs):
 d={}
 for k,v in pairs:
  if k in d:raise ValueError('Duplicate JSON key '+k)
  d[k]=v
 return d
def uint(n,bits=32):return type(n)is int and 0<=n<2**bits
def inspect_text(text,environment=None):
 errors=[];rows=[];schemas=set()
 def err(s):
  if len(errors)<32:errors.append(s)
 if len(text)>MAX_BYTES:return {'errors':['Text cap exceeded'],'qualified_complete_frames':[],'frame_candidates':[]}
 if text and not text.endswith('\n'):err('Incomplete final JSONL line')
 lines=text.splitlines()
 if len(lines)>MAX_ROWS:err('Observer record cap exceeded');lines=lines[:MAX_ROWS]
 for i,line in enumerate(lines,1):
  try:
   if len(line.encode('utf8'))>=4096:raise ValueError('Observer line cap exceeded')
   r=json.loads(line,object_pairs_hook=unique)
   if not isinstance(r,dict)or set(r)not in(FIELDS,FIELDS|PROGRAM_FIELDS):raise ValueError('Unexpected/missing observer schema')
   schema='observed_program_v2'if PROGRAM_FIELDS<=set(r)else 'legacy_v1'
   schemas.add(schema)
   if len(schemas)!=1:raise ValueError('Mixed observer row schemas')
   for k in FIELDS-HEX-set(ARRAYS)-{'outcome','attributes'}:
    if not uint(r[k],64 if k in ('array_vertices','requested_indices')else 32):raise ValueError('Invalid integer '+k)
   if r['outcome']not in OUTCOMES:raise ValueError('Unknown outcome')
   if schema=='observed_program_v2':validate_programs(r)
   if r['index_u32']not in(0,1)or r['fetch_ok']not in(0,1):raise ValueError('Invalid boolean integer')
   for k in HEX:
    if not isinstance(r[k],str)or not re.fullmatch('[0-9A-F]{8}',r[k]):raise ValueError('Invalid hex '+k)
   for k,n in ARRAYS.items():
    if not isinstance(r[k],list)or len(r[k])!=n or not all(uint(v)for v in r[k]):raise ValueError('Invalid array '+k)
   at=r['attributes']
   if not isinstance(at,list)or len(at)!=16 or not all(isinstance(a,list)and len(a)==2 and all(isinstance(v,str)and re.fullmatch('[0-9A-F]{8}',v)for v in a)for a in at):raise ValueError('Invalid attribute schema')
   if r['record']!=i:raise ValueError('Noncontiguous observer record sequence')
   if rows and r['frame']<rows[-1]['frame']:raise ValueError('Regressing/wrapped frame unsupported')
   rows.append(r)
  except (ValueError,TypeError)as e:err('Line%d: %s'%(i,e))
 if not rows:err('No observer records; unavailable, not zero draws')
 frames={}
 for r in rows:frames.setdefault(r['frame'],[]).append(r)
 ids=list(frames);candidates=ids[1:-1]if len(ids)>=3 else []
 profile_errors=[]
 if environment is None:profile_errors.append('Actual launch environment not supplied; all-target coverage unverified')
 elif not isinstance(environment,dict):profile_errors.append('Environment must be a flat actual-launch mapping')
 else:
  if environment.get('RSX_ENGINE_DRAW_OBSERVER_TARGET'):profile_errors.append('Raw color-A target filter was enabled')
  if environment.get('RSX_ENGINE_DRAW_OBSERVER_MAX')!='4096':profile_errors.append('Actual observer maximum differs from approved4096 profile')
  if not environment.get('RSX_ENGINE_DRAW_OBSERVER')or not environment.get('RSX_ENGINE_DRAW_OBSERVER_TRIGGER'):profile_errors.append('Actual output/trigger binding missing')
 metrics=[]
 for frame in candidates:
  rr=frames[frame];metrics.append({'frame':frame,'first_record':rr[0]['record'],'last_record':rr[-1]['record'],'records':len(rr),'outcomes':dict(collections.Counter(r['outcome']for r in rr)),'primitive_outcomes':[{'primitive':k[0],'outcome':k[1],'records':v}for k,v in collections.Counter((r['primitive'],r['outcome'])for r in rr).items()],'raw_program_control_outcomes':[{'fp_program':k[0],'shader_control':k[1],'vp_start':k[2],'outcome':k[3],'records':v}for k,v in collections.Counter((r['fp_program'],r['shader_control'],r['vp_start'],r['outcome'])for r in rr).items()]})
 for metric in metrics:metric.update(program_metrics(frames[metric['frame']]))
 return {'errors':errors,'profile_errors':profile_errors,'row_schema':next(iter(schemas))if len(schemas)==1 else 'invalid_or_mixed','records':len(rows),'record_cap_reached':len(rows)==MAX_ROWS,'observed_frame_ids':ids,'frame_candidates':candidates if not errors else [],'qualified_complete_frames':metrics if not errors and not profile_errors else [],'frame_counts':[{'frame':f,'records':len(rr),'outcomes':dict(collections.Counter(r['outcome']for r in rr)),'boundary_partial':f in ids[:1]+ids[-1:]}for f,rr in frames.items()],'source_scope':'Every observed sink_end_core call including early returns; does not cover upstream guest commands never dispatched to sink_end','qualifications':['Legacy v1 rows are explicitly accepted as identity-unavailable; no content identity is inferred from raw addresses. Mixed or partial extended schemas are rejected.','Observed groups require both VP and FP validity and group within one qualified frame by content hashes, lengths, control and all declared color target fields. Partial/unavailable rows are counted separately, never assigned a content group.','64-bit hashes can collide. FP structural hashes exclude inline constant values; VP constants are outside the content hash. Hash equality does not prove immutable identity, a coherent snapshot, complete pipeline-key equality or GPU binding/execution. Concurrent guest writes and residency changes remain possible.','First and last observed frames are always treated as partial, even at the4096record cap. Bracketed interiorframes require actualunfiltered launchconfiguration and completeJSONL.','Frame numbers count successful enginepresent calls, notGPUcompletion/scanout. Skipped numbers caninclude frameswithoutobservedsink_endcalls.','pipeline includes both eng_pipeline_get failure and sink_bind_textures ENG_INVALID in current source; notpure shaderfailure.','Raw FPaddress/control andVPstart are groupingfields, notimmutableprogramidentity or terrain/objectclassification.','Outcome records do notcarry checkedfailurelatch; correlate actualcheckedREFmode and failurelog beforeattribution.','Root mustseparately verify exactApp/SDK/source, freshtrigger timingactualHUD, exclusiveoutput, andretainedordinarysound/FPS. EnvironmentJSON aloneisnotlaunch provenance.']}
def inspect_file(path,environment=None):
 p=pathlib.Path(path)
 if p.is_symlink():raise ValueError('Symlink input')
 fd=os.open(p,os.O_RDONLY|getattr(os,'O_NOFOLLOW',0))
 with os.fdopen(fd,'rb')as f:
  a=os.fstat(f.fileno())
  if not stat.S_ISREG(a.st_mode)or a.st_size>MAX_BYTES:raise ValueError('Input mustbe regular fixedJSONLwithin16MiB')
  raw=f.read(MAX_BYTES+1);b=os.fstat(f.fileno())
 if len(raw)!=a.st_size or(a.st_dev,a.st_ino,a.st_size,a.st_mtime_ns)!=(b.st_dev,b.st_ino,b.st_size,b.st_mtime_ns):raise ValueError('Input changed duringread')
 r=inspect_text(raw.decode('utf8'),environment)
 if environment and environment.get('RSX_ENGINE_DRAW_OBSERVER')and pathlib.Path(environment['RSX_ENGINE_DRAW_OBSERVER']).resolve()!=p.resolve():r['profile_errors'].append('Actual output path doesnotbindinput');r['qualified_complete_frames']=[]
 r['input']={'path':str(p.resolve()),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()};return r
def main():
 ap=argparse.ArgumentParser();ap.add_argument('input');ap.add_argument('--environment',type=pathlib.Path);ap.add_argument('--output',type=pathlib.Path,required=True);a=ap.parse_args();env=json.loads(a.environment.read_text())if a.environment else None;r=inspect_file(a.input,env)
 with a.output.open('x')as f:json.dump(r,f,indent=2);f.write('\n')
 print(json.dumps({'errors':r['errors'],'profile_errors':r['profile_errors'],'records':r['records'],'qualified_frames':[f['frame']for f in r['qualified_complete_frames']]}));return 2 if r['errors']else 0
if __name__=='__main__':raise SystemExit(main())
