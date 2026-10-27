"""Read-only assessor for b92fda1 audio chronology metadata (no payload).

Associations are physical observations, not logical PCM/job ownership. Source
logs are read at a fixed prefix cutoff; output is stdout or a fresh JSON file.
No runtime, process, debugger, input, SDK, or publishing operations.
"""
from pathlib import Path
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
import math
import os
import re
import stat

UINT64=(1<<64)-1
HEX={'ea','lsa','cmd','ctx','pc','mask'}
RECORD_KEYS=set('seq kind us begin_us copy_us port epoch index ea size lsa cmd ctx spu pc mask nz words flags rc arg0 arg1 value'.split())
SUMMARY_KEYS=set('reason count dropped limit start_us duration_us queue_min queue_zero max_submit_interval_us inflight_at_flush coverage overlap_is_ambiguous'.split())
NAMES={1:'port',2:'consume_pre',3:'clear_begin',4:'clear_end',5:'cursor_post',6:'DMA_outcome',7:'SDL_submission',8:'notify',9:'API'}


def fields(text, required, summary=False):
    parts=text.split(); pairs={}
    for part in parts:
        if '=' not in part:raise ValueError('Non key=value token')
        key,value=part.split('=',1)
        if key in pairs:raise ValueError('Duplicate field '+key)
        pairs[key]=value
    if set(pairs)!=required:raise ValueError('Unrecognized or missing fields: '+str(sorted(set(pairs)^required)))
    out={}
    for key,value in pairs.items():
        if summary and key in ('reason','coverage'):
            out[key]=value;continue
        base=16 if key in HEX and not summary else 10
        pattern=r'[0-9a-fA-F]+' if base==16 else (r'-?[0-9]+' if key=='rc' else r'[0-9]+')
        if not re.fullmatch(pattern,value):raise ValueError('Malformed integer '+key)
        number=int(value,base)
        if key=='rc':
            if not -(1<<31)<=number<(1<<31):raise ValueError('Result outside signed32')
        elif not 0<=number<=UINT64:raise ValueError('Value outside uint64')
        out[key]=number
    return out


def scan(path):
    records=[]; summaries=[]; errors=[]; digest=hashlib.sha256(); lines=0; partial=False; error_count=0
    def error(line,message):
        nonlocal error_count
        error_count+=1
        if len(errors)<32:errors.append({'line':line,'error':message})
    with path.open('rb') as stream:
        meta=os.fstat(stream.fileno())
        if not stat.S_ISREG(meta.st_mode):raise ValueError('Expected a regular log file')
        remaining=meta.st_size
        while remaining:
            raw=stream.readline(min(remaining,1<<20))
            if not raw:break
            remaining-=len(raw);digest.update(raw);lines+=1
            if raw.startswith(b'[audio-chronology'):
                if not raw.endswith(b'\n'):
                    partial=True;error(lines,'Partial chronology line at cutoff');continue
                try:
                    line=raw.decode('ascii').strip()
                    if line.startswith('[audio-chronology-summary] '):
                        if len(summaries)>=8:raise ValueError('Too many capture summaries')
                        summaries.append(fields(line[len('[audio-chronology-summary] '):],SUMMARY_KEYS,True))
                    elif line.startswith('[audio-chronology] '):
                        if len(records)>=32768:raise ValueError('Record count exceeds source cap')
                        records.append(fields(line[len('[audio-chronology] '):],RECORD_KEYS))
                    else:raise ValueError('Unknown chronology prefix')
                    if len(records)>32768 or len(summaries)>8:raise ValueError('Chronology input exceeds source bounds')
                except (ValueError,UnicodeError) as exc:error(lines,str(exc))
        if remaining:error(lines,'Log shortened while reading prefix')
    return summaries,records,dict(path=str(path),prefix_bytes=meta.st_size-remaining,prefix_sha256=digest.hexdigest(),lines=lines,partial_chronology_line=partial,parse_error_count=error_count),errors


def distribution(values):
    values=sorted(values)
    if not values:return dict(count=0)
    q=lambda p:values[max(0,math.ceil(len(values)*p)-1)]
    return dict(count=len(values),min=values[0],max=values[-1],mean=sum(values)/len(values),p50=q(.5),p95=q(.95),p99=q(.99))


def assess(summaries,records,read_receipt=None,parse_errors=None):
    errors=list(parse_errors or []); warnings=[]
    if len(summaries)!=1:
        errors.append({'error':'Exactly one summary required; missing summary or multiple captures are not silently accepted','summary_count':len(summaries)})
    summary=summaries[0] if len(summaries)==1 else None
    seqs=[r['seq'] for r in records]
    if summary:
        if not 0<summary['limit']<=32768 or summary['count']>summary['limit']:
            errors.append({'error':'Invalid cap/count'})
        if len(records)!=summary['count'] or seqs!=list(range(1,summary['count']+1)):
            errors.append({'error':'Truncated, duplicated or noncontiguous printed buffer','expected':summary['count'],'observed':len(records)})
        if summary['coverage']!='SPU-DMA-only' or summary['overlap_is_ambiguous']!=1:
            errors.append({'error':'Unsupported observer coverage or ambiguity policy'})
        if not 0<summary['duration_us']<=30000000:
            errors.append({'error':'Invalid configured duration'})
        if summary['reason'] not in ('window','shutdown'):
            errors.append({'error':'Unknown flush reason'})
        if summary['dropped']:warnings.append('Eligible events were dropped after the global cap; association inference disabled.')
        if summary['inflight_at_flush']:warnings.append('Writer-slot brackets were pending at flush; association inference disabled.')
    if any(r['kind'] not in NAMES for r in records):errors.append({'error':'Unknown event kind'})
    u32_fields={'seq','kind','port','ea','size','lsa','cmd','spu','pc','mask','nz','words','flags'}
    for r in records:
        if any(r[key]>(1<<32)-1 for key in u32_fields):errors.append({'error':'Record field exceeds source uint32','seq':r['seq']})
        allowed_flags=7 if r['kind']==6 else 1 if r['kind']in(1,2,3,4,5)else 0
        if r['flags']&~allowed_flags:errors.append({'error':'Unsupported event flags','seq':r['seq']})
        if r['kind']==9 and r['cmd']not in(0,1):errors.append({'error':'Unsupported API kind','seq':r['seq']})
    api_source_counts=Counter(r['cmd']for r in records if r['kind']==9)
    if any(n>16 for n in api_source_counts.values()):errors.append({'error':'Per-API source quota exceeded'})
    times=[r['us'] for r in records]
    if times!=sorted(times):errors.append({'error':'Metadata event timestamps regressed'})
    if summary and any(not summary['start_us']<=t<summary['start_us']+summary['duration_us'] for t in times):errors.append({'error':'Event outside configured admission window'})
    counts=Counter(NAMES.get(r['kind'],'unknown') for r in records)
    ports={}; current_port_epoch={}; port_rows=[]; consumes=[]; dmas=[]; backend=[]; notifications=[]; api=[]; cycles=defaultdict(list)
    for r in records:
        if r['kind']==1:
            if r['port']>=8:errors.append({'error':'Invalid port registry index','seq':r['seq']});continue
            if r['flags']:
                channels,blocks=r['lsa'],r['cmd']
                if channels not in (2,8) or blocks not in (8,16,32) or r['size']!=channels*256*4 or r['ea']+r['size']*blocks>1<<32:
                    errors.append({'error':'Invalid allocated-port dimensions','seq':r['seq']});continue
                row=dict(port=r['port'],epoch=r['epoch'],EA=f"0x{r['ea']:08X}",base=r['ea'],channels=channels,blocks=blocks,block_bytes=r['size'],first_seen_us=r['us'])
                key=(r['port'],r['epoch'])
                if key in ports and any(ports[key][k]!=row[k] for k in ('base','channels','blocks','block_bytes')):errors.append({'error':'Port metadata changed without an epoch change','seq':r['seq']})
                ports[key]=row
            current_port_epoch[r['port']]=r['epoch']
            port_rows.append(dict(seq=r['seq'],port=r['port'],epoch=r['epoch'],active=bool(r['flags']),us=r['us'],ea=r['ea'],block_bytes=r['size'],blocks=r['cmd'],channels=r['lsa']))
        elif 2<=r['kind']<=5:
            p=ports.get((r['port'],r['epoch']))
            if not p:errors.append({'error':'Consumer lacks matching allocation metadata','seq':r['seq']});continue
            if r['epoch']!=current_port_epoch.get(r['port']):errors.append({'error':'Consumer allocation epoch differs from current registry','seq':r['seq']})
            slot=r['index']%p['blocks'];expected=p['base']+slot*p['block_bytes']
            if r['ea']!=expected or r['size']!=p['block_bytes'] or r['mask']!=1<<slot:
                errors.append({'error':'Consumer frame/slot mismatch','seq':r['seq']});continue
            expected_cursor=((r['index']+1)%p['blocks'])if r['kind']==5 else slot
            if r['arg0']!=expected_cursor:errors.append({'error':'Guest cursor stage is not the expected modulo index','seq':r['seq']})
            if r['kind']==2:
                if r['words']!=r['size']//4 or r['nz']>r['words']:errors.append({'error':'Invalid consumption magnitude count','seq':r['seq']})
                consumes.append(r)
            cycles[(r['port'],r['epoch'],r['index'],r['ea'])].append(r)
        elif r['kind']==6:
            p=ports.get((r['port'],r['epoch']))
            if not p:errors.append({'error':'DMA lacks its original port allocation metadata','seq':r['seq']});continue
            if r['nz']>r['words'] or r['words']*4>r['arg1'] or r['arg0']+r['arg1']>r['size']:
                errors.append({'error':'DMA sampled-word/range bounds mismatch','seq':r['seq']})
            if bool(r['flags']&2)!=(r['value']!=r['epoch']):errors.append({'error':'DMA epoch value/mismatch flag inconsistent','seq':r['seq']})
            if r['value']!=current_port_epoch.get(r['port']):errors.append({'error':'DMA current epoch differs from record-time registry','seq':r['seq']})
            if summary:
                end=summary['start_us']+summary['duration_us']
                if not summary['start_us']<=r['begin_us']<end or (r['copy_us'] and not summary['start_us']<=r['copy_us']<end):errors.append({'error':'DMA bracket began or copied outside capture admission','seq':r['seq']})
            if not r['begin_us']<=r['us'] or (r['flags']&4 and not r['begin_us']<=r['copy_us']<=r['us']):errors.append({'error':'DMA bracket timestamp invalid','seq':r['seq']})
            lo=max(r['ea'],p['base']);hi=min(r['ea']+r['size'],p['base']+p['blocks']*p['block_bytes'])
            if lo>=hi or r['arg0']!=lo-r['ea'] or r['arg1']!=hi-lo:
                errors.append({'error':'DMA recorded overlap differs from registered ring','seq':r['seq']});continue
            first=(lo-p['base'])//p['block_bytes'];last=(hi-1-p['base'])//p['block_bytes']
            expected_mask=((1<<(last-first+1))-1)<<first
            if r['mask']!=expected_mask:errors.append({'error':'DMA slot mask mismatch','seq':r['seq']})
            dmas.append(r)
        elif r['kind']==7:backend.append(r)
        elif r['kind']==8:notifications.append(r)
        elif r['kind']==9:
            name='block_tag' if r['cmd']==0 else 'timestamp' if r['cmd']==1 else 'unknown'
            row=dict(epoch_qualification='Record-time registry snapshot; not necessarily the epoch of the API computation before its observation callback.',seq=r['seq'],api=name,port=r['port'],allocation_epoch=r['epoch'],time_us=r['us'],requested=r['arg0'],captured_counter=None if r['arg1']==UINT64 else r['arg1'],returned_value=None if r['value']==UINT64 else r['value'],result=r['rc'])
            p=ports.get((r['port'],r['epoch']))
            if p and r['cmd']==0 and r['rc']==0 and r['arg1']!=UINT64:
                model=r['arg1']+r['arg0']-r['arg1']%p['blocks']
                row.update(reference_arithmetic_only=model,actual_minus_reference=r['value']-model,reference_dimensions_qualification='Uses record-time allocation dimensions, not a certificate of dimensions during the earlier API computation.')
            if r['cmd']==1 and r['rc']==0 and r['value']!=UINT64:row['returned_minus_host_observation_us']=r['value']-r['us']
            api.append(row)
    complete_cycles={}; partial_cycles=[]
    for key,items in cycles.items():
        kinds=[x['kind'] for x in items]
        if kinds==[2,3,4,5]:complete_cycles[key]=items
        else:partial_cycles.append(dict(port=key[0],epoch=key[1],consumer_index=key[2],ea=key[3],stages=kinds,seqs=[x['seq']for x in items]))
    if partial_cycles:warnings.append('Consumption stages cross a capture boundary or are incomplete; those cycles are excluded from associations.')
    # Source flags are copy/consume-specific. Conservatively detect producer-vs-
    # producer overlap too, using request-through-outcome brackets. Equal-us
    # endpoints are ambiguous at the clock resolution, not forcibly ordered.
    producer_overlap=set();comparison_exhausted=False;comparison_count=0;comparison_limit=250000
    successful=[d for d in dmas if d['rc']==0 and d['flags']&4]
    by_port=defaultdict(list)
    for d in successful:by_port[(d['port'],d['epoch'])].append(d)
    for group in by_port.values():
        group.sort(key=lambda x:x['begin_us']);active=[]
        for d in group:
            active=[a for a in active if a['us']>=d['begin_us']]
            for a in active:
                comparison_count+=1
                if comparison_count>comparison_limit:
                    comparison_exhausted=True;break
                if max(a['ea']+a['arg0'],d['ea']+d['arg0'])<min(a['ea']+a['arg0']+a['arg1'],d['ea']+d['arg0']+d['arg1']):producer_overlap.update((a['seq'],d['seq']))
            active.append(d)
            if comparison_exhausted:break
        if comparison_exhausted:break
    if comparison_exhausted:
        warnings.append('Producer overlap comparison budget exhausted; associations disabled.')
    dma_slots=defaultdict(list)
    for d in dmas:
        for slot in range(32):
            if d['mask']&(1<<slot):dma_slots[(d['port'],d['epoch'],slot)].append(d)
    for group in dma_slots.values():group.sort(key=lambda d:(d['us'],d['seq']))
    slot_times={key:[d['us']for d in group]for key,group in dma_slots.items()}
    association_enabled=bool(summary and not errors and not summary['dropped'] and not summary['inflight_at_flush'] and not comparison_exhausted)
    associations=[]; exclusions=Counter(); clear_times=defaultdict(list)
    # A recorded physical clear remains a cutoff even if its cycle lacks a
    # cursor/other stage at the capture boundary. Never resurrect preclear PCM.
    for items in cycles.values():
        for r in items:
            if r['kind']==4:clear_times[(r['port'],r['epoch'],r['ea'])].append((r['us'],r['seq']))
    for stamps in clear_times.values():stamps.sort()
    for c in consumes:
        key=(c['port'],c['epoch'],c['index'],c['ea']);slot_key=(c['port'],c['epoch'],c['ea'])
        cycle=complete_cycles.get(key)
        stamps=clear_times.get(slot_key,[])
        before=bisect.bisect_left(stamps,(c['us'],c['seq']))
        prior=stamps[before-1][0]if before else summary['start_us']if summary else 0
        if not association_enabled:exclusions['capture_incomplete_or_budget_or_inflight']+=1
        elif not cycle:exclusions['incomplete_consume_cycle']+=1
        elif any(r['flags'] for r in cycle):exclusions['consume_overlaps_writer']+=1
        else:
            slot=c['index']%ports[(c['port'],c['epoch'])]['blocks']
            bucket=(c['port'],c['epoch'],slot)
            group=dma_slots.get(bucket,[]);stamps=slot_times.get(bucket,[])
            overlapping=group[bisect.bisect_left(stamps,prior):bisect.bisect_right(stamps,c['us'])]
            ambiguous=[d for d in overlapping if d['flags']&3 or d['seq']in producer_overlap or d['us']>=c['us']]
            if ambiguous:exclusions['ambiguous_writer_or_clock_tie']+=1
            else:
                eligible=[d for d in overlapping if d['rc']==0 and d['flags']&4 and prior<d['copy_us']<c['us'] and d['us']<c['us']]
                if not eligible:exclusions['no_qualified_observed_SPU_copy_not_no_producer']+=1
                else:
                    latest=max(eligible,key=lambda d:(d['copy_us'],d['seq']))
                    lo=latest['ea']+latest['arg0'];hi=lo+latest['arg1']
                    whole=lo<=c['ea'] and c['ea']+c['size']<=hi
                    spans=sorted((max(d['ea']+d['arg0'],c['ea']),min(d['ea']+d['arg0']+d['arg1'],c['ea']+c['size']))for d in eligible)
                    union_bytes=0;union_end=c['ea']
                    for low,high in spans:
                        if high>max(low,union_end):union_bytes+=high-max(low,union_end)
                        union_end=max(union_end,high)
                    associations.append(dict(observed_candidate_copies=len(eligible),observed_span_union_bytes=union_bytes,observed_span_union_covers_slot=union_bytes==c['size'],consume_seq=c['seq'],dma_seq=latest['seq'],port=c['port'],allocation_epoch=c['epoch'],consumer_index=c['index'],slot_ea=c['ea'],copy_to_consume_us=c['us']-latest['copy_us'],whole_slot_covered=whole,multi_slot_DMA=latest['mask']&(latest['mask']-1)!=0,copied_words=latest['words'],copy_magnitude_nz=latest['nz'],consumed_magnitude_nz=c['nz'],copy_magnitude_is_slot_specific=latest['mask']==c['mask'],prior_clear_or_window_start_us=prior,qualification='Latest recorded unambiguous SPU copy intersecting this slot; intervening CPU/HLE stores and logical producer intent are not excluded.'))
    consume_groups=defaultdict(list)
    for c in consumes:consume_groups[(c['port'],c['epoch'])].append(c)
    consumer_summary=[]
    for (port,epoch),items in consume_groups.items():
        nz=sum(x['nz']>0 for x in items)
        consumer_summary.append(dict(port=port,allocation_epoch=epoch,observed_pre_consumption_records=len(items),records_with_nonzero_magnitude=nz,observed_nonzero_fraction=nz/len(items),overlap_flagged_records=sum(bool(x['flags'])for x in items),complete_cycles=sum((x['port'],x['epoch'],x['index'],x['ea'])in complete_cycles for x in items),qualification='Captured raw-magnitude observations, not an underrun or intentional-silence classification.'))
    interval=[r['value']for r in backend if r['value']]
    complete=not errors and summary is not None
    return dict(status='complete_metadata_buffer' if complete else 'incomplete_or_invalid',capture_integrity=dict(complete_printed_buffer=complete,association_enabled=association_enabled,errors=errors,warnings=warnings,partial_consumption_cycles=partial_cycles),source_format='b92fda1/5de5ed4 unchanged metadata schema',read_receipt=read_receipt,summary=summary,event_counts=dict(counts),port_epochs=port_rows,DMA=dict(attempts=len(dmas),successful_copied_PUTs=len(successful),failed_or_no_copy=len(dmas)-len(successful),copy_consume_ambiguous_flags=sum(bool(d['flags']&1)for d in dmas),epoch_mismatch_flags=sum(bool(d['flags']&2)for d in dmas),producer_overlap_or_clock_tie_seqs=sorted(producer_overlap),overlap_comparison_count=comparison_count,overlap_comparison_budget_exhausted=comparison_exhausted,partial_word_samples=sum(d['words']*4<d['arg1']for d in successful),whole_record_zero_sample_counts=sum(d['words']>0 and d['nz']==0 for d in successful)),consumers=consumer_summary,copy_to_consume=dict(association_count=len(associations),interval_us=distribution([a['copy_to_consume_us']for a in associations]),whole_slot_associations=sum(a['whole_slot_covered']for a in associations),partial_slot_associations=sum(not a['whole_slot_covered']for a in associations),observed_span_union_whole_slots=sum(a['observed_span_union_covers_slot']for a in associations),exclusions=dict(exclusions),examples=associations[:256],examples_truncated=len(associations)>256),backend=dict(observed_submissions=len(backend),observed_zero_queue_before=sum(r['arg0']==0 for r in backend),queue_before_frames=distribution([r['arg0']for r in backend]),queue_after_frames=distribution([r['arg1']for r in backend]),submit_interval_us=distribution(interval),errors=sum(r['rc']<0 for r in backend),qualification='Sampled queue depths/intervals, not an asynchronous device-underrun certificate.'),notifications=dict(observed=len(notifications),successful_pushes=sum(r['rc']==0 for r in notifications),failed_pushes=sum(r['rc']==-1 for r in notifications),unresolved_queues=sum(r['rc']==-2 for r in notifications),other_results=dict(Counter(str(r['rc'])for r in notifications if r['rc']not in(0,-1,-2)))),APIs=dict(counts=dict(Counter(r['api']for r in api)),records=api,qualification='First16 admitted calls per API. API epochs are record-time registry snapshots, not operation-time ownership certificates. Reference arithmetic comparison does not certify firmware behavior or a MAG defect; timestamp/host clock domains remain qualified.'),qualifications=['Allocation epoch is not logical producer/job generation.','No observed SPU PUT does not imply no CPU/HLE/atomic producer.','Nonzero magnitude bits do not establish finite or audible PCM.','Physical copy-to-consume association is not packet ownership, designed silence or proof of starvation.','Dropped/inflight/ambiguous records cannot establish absence or exact first loss.','Diagnostic sampling overhead and final stderr flush remain relevant to native timing.'])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--log',required=True,type=Path);p.add_argument('--output',type=Path);args=p.parse_args()
    summaries,records,receipt,errors=scan(args.log)
    result=assess(summaries,records,receipt,errors)
    result['assessor_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    text=json.dumps(result,indent=2)+'\n'
    if args.output:
        if args.output.resolve()==args.log.resolve():raise ValueError('Cannot replace input log')
        with args.output.open('x')as stream:stream.write(text)
    else:print(text,end='')
    raise SystemExit(0 if result['capture_integrity']['complete_printed_buffer'] else 2)


if __name__=='__main__':main()
