#!/usr/bin/env python3
"""Audit/lift/compile every retail SPU ELF in an explicitly pinned inventory.

Original images, generated C and objects belong in an excluded private output
folder. This tool never links, emits startup registration, or enables runtime
routing. A compiled object is preparation evidence only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import time

MAX_INPUT=1024*1024*1024

def sha(data):return hashlib.sha256(data).hexdigest()
def number(value):
    if isinstance(value,bool)or not isinstance(value,(int,str)):raise ValueError('Address/count must be integer or integer string')
    return int(value,0)if isinstance(value,str)else int(value)
def fnv(data):
    value=1469598103934665603
    for byte in data:value=((value^byte)*1099511628211)&0xffffffffffffffff
    return value

def read_regular(path,cap=MAX_INPUT):
    path=Path(path)
    if path.is_symlink():raise ValueError('Symlink input: '+str(path))
    try:fd=os.open(path,os.O_RDONLY|getattr(os,'O_NOFOLLOW',0))
    except OSError as error:raise ValueError('Cannot open input: '+str(path))from error
    with os.fdopen(fd,'rb')as stream:
        before=os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode)or before.st_size>cap:raise ValueError('Not a bounded regular input')
        data=stream.read(cap+1);after=os.fstat(stream.fileno())
    keys=lambda s:(s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns)
    if keys(before)!=keys(after)or len(data)!=before.st_size:raise ValueError('Input changed during read')
    return data

def fresh_output(path):
    path=Path(path)
    if path.exists()or path.is_symlink():raise ValueError('Output must be fresh: '+str(path))
    path=path.resolve()
    if '.git'in path.parts:raise ValueError('Output cannot be Git metadata')
    ancestor=path.absolute().parent
    while not ancestor.exists():ancestor=ancestor.parent
    probe=subprocess.run(['git','-C',str(ancestor),'rev-parse','--show-toplevel'],capture_output=True,text=True)
    if probe.returncode==0:
        ignored=subprocess.run(['git','-C',probe.stdout.strip(),'check-ignore','-q','--no-index',str(path)+os.sep],capture_output=True)
        if ignored.returncode!=0:raise ValueError('Original outputs must be outside Git or explicitly ignored')
    path.mkdir(mode=0o700,parents=True,exist_ok=False)
    return path

def elf_metadata(data):
    if len(data)<52 or data[:7]!=b'\x7fELF\x01\x02\x01':raise ValueError('Expected ELF32 big-endian version1')
    h=struct.unpack_from('>16sHHIIIIIHHHHHH',data)
    _,kind,machine,version,entry,phoff,shoff,flags,ehsize,phes,phnum,shes,shnum,strings=h
    if kind!=2 or machine!=23 or version!=1 or ehsize!=52 or not phnum or phes<32 or phoff+phes*phnum>len(data):raise ValueError('Invalid SPU program headers')
    if shnum and(shes<40 or shoff+shes*shnum>len(data)):raise ValueError('Invalid SPU section headers')
    end=shoff+shnum*shes;loads=[];execute=[];sections=[]
    for k in range(phnum):
        ptype,off,va,pa,fs,ms,pflags,align=struct.unpack_from('>IIIIIIII',data,phoff+phes*k)
        if off+fs>len(data):raise ValueError('Segment backing out of bounds')
        end=max(end,off+fs)
        if ptype==1:
            if fs>ms or va+ms>0x40000:raise ValueError('Segment LS bounds invalid')
            loads.append((va,va+ms))
            if pflags&1:execute.append((va,va+fs))
    for k in range(shnum):
        _,stype,sflags,addr,off,size,_,_,_,_=struct.unpack_from('>IIIIIIIIII',data,shoff+shes*k)
        if stype!=8:
            if off+size>len(data):raise ValueError('Section backing out of bounds')
            end=max(end,off+size)
        if sflags&4 and stype==1:
            if addr+size>0x40000 or size%4:raise ValueError('Executable section LS/alignment bounds invalid')
            if not any(lo<=addr and addr+size<=hi for lo,hi in execute):raise ValueError('Executable section outside executable load')
            sections.append((addr,addr+size))
    if end!=len(data):raise ValueError('ELF extent differs from exact inventory size')
    if not execute or entry%4 or not any(lo<=entry<hi for lo,hi in execute):raise ValueError('Entry is outside executable LS bytes')
    if sections and not any(lo<=entry<hi for lo,hi in sections):raise ValueError('Entry outside executable section')
    return dict(entry=entry,executable_end=max(hi for lo,hi in(sections or execute)),load_segments=loads,executable_ranges=sections or execute)

def plan(inventory,carriers):
    """Verify ALL requested retail occurrences before producing original files."""
    images=[x for x in inventory.get('images',[])if x.get('scope')=='retail']
    retail_sources=[x for x in inventory.get('sources',[])if x.get('scope')=='retail']
    sources={x['name']:x for x in retail_sources}
    if len(sources)!=len(retail_sources):raise ValueError('Duplicate source names')
    if not images:raise ValueError('No requested retail SPU ELF images')
    needed={x['carrier']for x in images}
    for name in needed:
        source=sources.get(name);data=carriers.get(name)
        if not source or data is None:raise ValueError('Missing requested carrier '+name)
        if len(data)!=number(source['bytes'])or sha(data)!=source['sha256']:raise ValueError('Carrier identity mismatch: '+name)
    grouped={}
    for image in images:
        off=number(image['file_offset']);size=number(image['size']);carrier=carriers[image['carrier']]
        if off<0 or size<=0 or off+size>len(carrier):raise ValueError('Image slice bounds invalid')
        data=carrier[off:off+size];identity=sha(data)
        if identity!=image['sha256']:raise ValueError('Embedded image SHA mismatch')
        meta=elf_metadata(data)
        if meta['entry']!=number(image['entry'])or meta['executable_end']!=number(image['executable_end']):raise ValueError('Entry/code extent differs from pinned inventory')
        if fnv(data)!=number(image['runtime_fingerprint']):raise ValueError('Runtime fingerprint mismatch')
        previous=grouped.get(identity)
        if previous and(previous['metadata']!=meta or previous['data']!=data):raise ValueError('Duplicate metadata conflict')
        if not previous:
            previous=dict(sha256=identity,data=data,metadata=meta,image_metadata=image,occurrences=[])
            grouped[identity]=previous
        previous['occurrences'].append(image)
    expected=inventory.get('summary',{})
    if 'retail_occurrences'in expected and number(expected['retail_occurrences'])!=len(images):raise ValueError('Occurrence census mismatch')
    if 'retail_unique_ELFs'in expected and number(expected['retail_unique_ELFs'])!=len(grouped):raise ValueError('Unique-image census mismatch')
    return [grouped[key]for key in sorted(grouped)]

def classify(lift_exit,compile_exit,unsupported,entry_present):
    if lift_exit!=0 or compile_exit!=0 or not entry_present:return 'failed'
    return 'unsupported'if unsupported else 'compiled'

def run_command(command,timeout):
    start=time.monotonic()
    try:
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=(os.name!='nt'))
        try:output,_=process.communicate(timeout=timeout);expired=False
        except subprocess.TimeoutExpired:
            if os.name!='nt':os.killpg(process.pid,signal.SIGKILL)
            else:process.kill()
            output,_=process.communicate();expired=True
        return dict(command=command,exit=process.returncode,timeout=expired,elapsed_seconds=time.monotonic()-start,output=output.decode('utf-8',errors='replace'))
    except OSError as error:return dict(command=command,exit=None,timeout=False,elapsed_seconds=time.monotonic()-start,output=str(error))

def sdk_guard(sdk,revision):
    head=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    if head!=revision:raise ValueError('SDK revision changed/unexpected')
    dirty=subprocess.check_output(['git','-C',str(sdk),'status','--porcelain','--untracked-files=no'],text=True)
    if dirty:raise ValueError('Tracked SDK sources are dirty')

def atomic_report(output,report):
    temporary=output/'report.json.tmp';temporary.write_text(json.dumps(report,indent=2)+'\n');temporary.replace(output/'report.json')

def build_image(item,sdk,revision,output,compiler,lift_timeout,compile_timeout):
    identity=item['sha256'];name='retail_'+identity;prefix='mag_batch_'+identity+'_';folder=output/name;folder.mkdir()
    record={k:v for k,v in item.items()if k!='data'}
    record.update(prefix=prefix,status='failed',runtime_activation='not_enabled',link_status='not_linked',runtime_routing='unverified',registration_classification='inventory-source-cutoff-only',stages={})
    sdk_guard(sdk,revision)
    image=folder/'input.elf';image.write_bytes(item['data']);generated=folder/'generated'
    command=[sys.executable,str(sdk/'tools/spu_lifter.py'),'--auto-functions',str(image),'--code-end',hex(item['metadata']['executable_end']),'--symbol-prefix',prefix,'--output',str(generated)]
    lift=run_command(command,lift_timeout);(folder/'lift.log').write_text(lift.pop('output'));record['stages']['lift']=lift
    if lift['exit']!=0:record['failure_reason']='lift_timeout'if lift['timeout']else'lift_failed';return record
    source=generated/'spu_recomp.c';header=generated/'spu_recomp.h'
    try:
        for path in(source,header):
            original=read_regular(path,64*1024*1024).decode('utf-8')
            path.write_text(re.sub(r'#include "(?:\.\./)+runtime/spu/([^"/]+)"',r'#include "\1"',original))
        code=source.read_text();entry=f"{prefix}spu_func_{item['metadata']['entry']:08X}"
        present=bool(re.search(r'\bvoid\s+'+re.escape(entry)+r'\s*\([^;]*\)\s*\{',code))
        record['entry_symbol']=entry;record['entry_present']=present
        record['generated_function_definitions']=len(re.findall(r'\bvoid\s+'+re.escape(prefix)+r'spu_func_[0-9A-Fa-f]+\s*\([^;]*\)\s*\{',code))
        record['unsupported_marker_count']=len(re.findall(r'TODO spu:|\bspu_unsupported\s*\(',code))
        record['unsupported_call_sites']=len(re.findall(r'\bspu_unsupported\s*\(',code))
        record['unsupported_count_scope']='text markers, may include TODO+call for same instruction; not unique opcode count'
        record['generated_files']={str(p.relative_to(output)):sha(read_regular(p))for p in(source,header)}
        # No constructor, registry emission, link or original-program execution.
        if '__attribute__((constructor))'in code:raise ValueError('Unexpected generated startup constructor')
        obj=folder/'spu_recomp.o'
        command=[compiler,'-O1','-std=gnu11','-ffp-contract=off','-I'+str(sdk/'include'),'-I'+str(sdk/'runtime/spu'),'-c',str(source),'-o',str(obj)]
        compile=run_command(command,compile_timeout);(folder/'compile.log').write_text(compile.pop('output'));record['stages']['compile']=compile
        valid_object=compile['exit']==0 and obj.is_file()and not obj.is_symlink()and obj.stat().st_size>0
        record['status']=classify(lift['exit'],compile['exit']if valid_object else None,record['unsupported_marker_count'],present)
        if valid_object:record.update(object=str(obj.relative_to(output)),object_bytes=obj.stat().st_size,object_sha256=sha(read_regular(obj)))
        if record['status']=='failed':record['failure_reason']='compile_timeout'if compile['timeout']else'compile_or_entry_or_object_failed'
    except(ValueError,OSError,UnicodeError)as error:record['failure_reason']='generated_artifact_validation: '+str(error)
    sdk_guard(sdk,revision)
    return record

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inventory',required=True,type=Path);ap.add_argument('--inventory-sha256',required=True)
    ap.add_argument('--carrier',action='append',default=[],metavar='NAME=PATH');ap.add_argument('--sdk',required=True,type=Path);ap.add_argument('--sdk-revision',required=True)
    ap.add_argument('--output',required=True,type=Path);ap.add_argument('--compiler',default='cc');ap.add_argument('--lift-timeout',type=float,default=60);ap.add_argument('--compile-timeout',type=float,default=90)
    args=ap.parse_args();started=time.monotonic()
    try:
        if not(0<args.lift_timeout<=600 and 0<args.compile_timeout<=600):raise ValueError('Timeout bounds invalid')
        raw=read_regular(args.inventory,16*1024*1024)
        if sha(raw)!=args.inventory_sha256:raise ValueError('Inventory hash mismatch')
        inventory=json.loads(raw);paths={}
        for value in args.carrier:
            name,sep,path=value.partition('=')
            if not sep or not name or name in paths:raise ValueError('Invalid/duplicate carrier argument')
            paths[name]=Path(path)
        carriers={name:read_regular(path)for name,path in paths.items()};items=plan(inventory,carriers)
        sdk=args.sdk.resolve();sdk_guard(sdk,args.sdk_revision)
        compiler=shutil.which(args.compiler)
        if not compiler:raise ValueError('Compiler not found')
        compiler_version=run_command([compiler,'--version'],5)
        if compiler_version['exit']!=0:raise ValueError('Compiler identity unavailable')
        tool_paths=['tools/spu_lifter.py','tools/find_spu_functions.py','tools/spu_disasm.py','runtime/spu/spu_helpers.h','runtime/spu/spu_context.h']
        tool_hashes={name:sha(read_regular(sdk/name))for name in tool_paths}
        output=fresh_output(args.output)
        report=dict(schema_version=1,status='incomplete',scope='all-inventory-retail-ELFs',inventory_sha256=sha(raw),inventory_MAG_revision=inventory.get('MAG_revision'),inventory_lifter_revision=inventory.get('lifter_revision'),sdk_revision=args.sdk_revision,sdk=str(sdk),tool_sha256=tool_hashes,driver_sha256=sha(read_regular(Path(__file__))),compiler=compiler_version,carriers={name:dict(path=str(path.resolve()),sha256=sha(carriers[name]),bytes=len(carriers[name]))for name,path in paths.items()},requested_occurrences=sum(len(x['occurrences'])for x in items),requested_unique=len(items),images=[],excluded_nonretail_occurrences=sum(x.get('scope')!='retail'for x in inventory['images']),inventory_registrations_outside_selected_ELFs=[r for r in inventory.get('registrations',[])if number(r['fingerprint'])not in {number(x['image_metadata']['runtime_fingerprint'])for x in items}],limits=['No raw non-ELF policies or unlisted images discovered by this inventory-driven batch','Registration metadata is pinned historical source-cutoff evidence, not current linker or runtime routing proof','Compiled success does not establish execution, scheduling, DMA, synchronization, rendering, audio or speed'])
        atomic_report(output,report)
        for item in items:
            image_started=time.monotonic()
            try:record=build_image(item,sdk,args.sdk_revision,output,compiler,args.lift_timeout,args.compile_timeout)
            except Exception as error:
                record={k:v for k,v in item.items()if k!='data'};record.update(status='failed',failure_reason=type(error).__name__+': '+str(error),runtime_activation='not_enabled',link_status='not_linked',runtime_routing='unverified')
            record['elapsed_seconds']=time.monotonic()-image_started
            report['images'].append(record);atomic_report(output,report)
            print(json.dumps(dict(sha256=item['sha256'],status=record['status'],reason=record.get('failure_reason'))),flush=True)
        sdk_guard(sdk,args.sdk_revision)
        counts={status:sum(x['status']==status for x in report['images'])for status in('compiled','unsupported','failed')}
        report.update(status='complete'if counts['compiled']==len(items)else'completed_with_failures',counts=counts,elapsed_seconds=time.monotonic()-started)
        atomic_report(output,report)
        return 0 if report['status']=='complete'else 1
    except(ValueError,OSError,subprocess.SubprocessError,json.JSONDecodeError)as error:
        if 'report'in locals()and 'output'in locals():
            report.update(status='global_validation_failed',global_error=type(error).__name__+': '+str(error),elapsed_seconds=time.monotonic()-started)
            atomic_report(output,report)
        print('GLOBAL VALIDATION FAILED: '+str(error),file=sys.stderr);return 2
if __name__=='__main__':raise SystemExit(main())
