#!/usr/bin/env python3
"""Private MAG preparation; expected guest bytes and generated lifts stay outside Git."""
import argparse, hashlib, json, os, pathlib, re, shutil, struct, subprocess, sys

def digest(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def sdk_identity(sdk, revision):
    head=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    dirty=subprocess.check_output(['git','-C',str(sdk),'status','--porcelain','--untracked-files=no'],text=True)
    if head!=revision or dirty: raise ValueError('SDK revision or tracked cleanliness changed')
    return {'revision':head,'lifter_sha256':digest(sdk/'tools/spu_lifter.py'),
            'header_sha256':digest(sdk/'runtime/spu/spu_verified_overlay.h')}

def fresh_output(path):
    path=pathlib.Path(path)
    if path.exists() or path.is_symlink(): raise ValueError('output must be fresh')
    if any(part.casefold()=='.git' for part in path.parts): raise ValueError('output cannot be Git metadata')
    path=path.resolve()
    if any(part.casefold()=='.git' for part in path.parts): raise ValueError('output cannot be Git metadata')
    ancestor=path.parent
    while not ancestor.exists(): ancestor=ancestor.parent
    probe=subprocess.run(['git','-C',str(ancestor),'rev-parse','--show-toplevel'],capture_output=True,text=True)
    if probe.returncode==0:
        ignored=subprocess.run(['git','-C',probe.stdout.strip(),'check-ignore','-q','--no-index',str(path)+os.sep],capture_output=True)
        if ignored.returncode!=0: raise ValueError('original outputs must be outside Git or explicitly ignored')
    else:
        metadata=subprocess.run(['git','-C',str(ancestor),'rev-parse','--is-inside-git-dir'],capture_output=True,text=True)
        if metadata.returncode==0 and metadata.stdout.strip()=='true': raise ValueError('output cannot be Git metadata')
    path.mkdir(mode=0o700,parents=True,exist_ok=False)
    return path

def copy_baseline(source, output):
    # Copy children, not the root: copytree would replace its owner-only mode
    # with the baseline directory's permissions during its final copystat.
    for child in source.iterdir():
        if child.is_dir(): shutil.copytree(child,output/child.name)
        else: shutil.copy2(child,output/child.name)

def executable_sections(blob):
    if len(blob)<52 or blob[:7]!=b'\x7fELF\x01\x02\x01' or struct.unpack_from('>H',blob,18)[0]!=23:
        raise ValueError('expected big-endian SPU ELF32')
    off=struct.unpack_from('>I',blob,32)[0]; size,n,si=struct.unpack_from('>HHH',blob,46)
    if size!=40 or not 0<n<=256 or si>=n or off+n*size>len(blob): raise ValueError('section table bounds')
    ss=[struct.unpack_from('>IIIIIIIIII',blob,off+i*size) for i in range(n)]
    out=[]
    for s in ss:
        if not s[2]&4: continue
        _,typ,flags,lsa,fo,count,*_=s
        if typ!=1 or flags&1 or not count or (lsa|count)&3 or lsa+count>0x40000 or fo+count>len(blob):
            raise ValueError('invalid or writable executable section')
        if any(lsa<a+len(b) and a<lsa+count for a,b in out): raise ValueError('overlapping executable sections')
        out.append((lsa,blob[fo:fo+count]))
    if not out: raise ValueError('no executable sections')
    return out

def emit_registry(images):
    lines=['/* PRIVATE generated expected guest bytes; do not commit. */','#include "spu_verified_overlay.h"','#include <stdio.h>','#include <stdlib.h>','extern void spu_begin_image(int);']
    for ident,prefix,segs in images:
        lines.append(f'extern void {prefix}spu_recomp_register(void);')
        for j,(lsa,data) in enumerate(segs):
            lines.append(f'static const uint8_t image_{ident}_{j}[] = {{')
            lines += ['  '+','.join(f'0x{x:02x}' for x in data[k:k+24])+',' for k in range(0,len(data),24)]
            lines.append('};')
        lines.append(f'static const spu_verified_code_segment segments_{ident}[] = {{')
        lines += [f'  {{0x{a:X}u,{len(b)}u,image_{ident}_{j}}},' for j,(a,b) in enumerate(segs)]
        lines.append('};')
    lines.append('__attribute__((constructor)) static void mag_register_verified_overlays(void) {')
    for ident,prefix,segs in images:
        lines.append(f'  if (!spu_verified_overlay_register(101,{ident},segments_{ident},{len(segs)})) {{ fputs("MAG verified overlay registration failed\\n",stderr); abort(); }}')
        lines.append(f'  spu_begin_image({ident}); {prefix}spu_recomp_register();')
    lines += ['  spu_begin_image(0);','}']
    return '\n'.join(lines)+'\n'

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--profile',type=pathlib.Path,required=True);ap.add_argument('--sdk',type=pathlib.Path,required=True);ap.add_argument('--sdk-revision',required=True);ap.add_argument('--baseline',type=pathlib.Path,required=True);ap.add_argument('--output',type=pathlib.Path,required=True);ap.add_argument('--audio-elf',type=pathlib.Path,required=True);ap.add_argument('--edge-raw',type=pathlib.Path);ap.add_argument('--edge-wrapper',type=pathlib.Path)
    a=ap.parse_args()
    if digest(a.profile)!='b846690d488c6cc0e0e5189cf777b41137c488091369805f04c882575cb4aa82': raise ValueError('profile identity requires review')
    profile=json.loads(a.profile.read_text())
    if profile.get('schema')!=1 or profile.get('policy_image_id')!=101 or profile.get('audio_A',{}).get('image_id')!=104 or profile.get('edge_partial',{}).get('image_id')!=105: raise ValueError('unsupported profile contract')
    sdk_pin=sdk_identity(a.sdk,a.sdk_revision)
    if a.output.exists(): raise ValueError('fresh output required')
    audio=a.audio_elf.read_bytes()
    if hashlib.sha256(audio).hexdigest()!=profile['audio_A']['ELF_sha256']: raise ValueError('audio identity')
    segments=executable_sections(audio)
    if [(x,len(y)) for x,y in segments]!=[(0x4000,16),(0x4080,64056)]: raise ValueError('audio code map')
    baseline_provenance_sha=digest(a.baseline/'provenance.json')
    baseline=json.loads((a.baseline/'provenance.json').read_text())
    for rel,h in baseline['outputs'].items():
        if digest(a.baseline/rel)!=h: raise ValueError('baseline output hash '+rel)
    if bool(a.edge_raw)!=bool(a.edge_wrapper): raise ValueError('edge requires raw and wrapper')
    if a.edge_raw:
        if digest(a.edge_raw)!='0ba87cc07fc961ca98231735183699f1b71ecec792dae70095c1b57f430fefe8': raise ValueError('edge identity')
        wrapper=a.edge_wrapper.read_bytes()
        if executable_sections(wrapper)!=[(0x4030,a.edge_raw.read_bytes()[0x30:0x11B50])] or struct.unpack_from('>I',wrapper,24)[0]!=0x15898: raise ValueError('edge wrapper code/entry')
    lifter=a.sdk/'tools/spu_lifter.py'
    if '--verified-code-image' not in lifter.read_text(): raise ValueError('verified lifter required')
    a.output=fresh_output(a.output)
    copy_baseline(a.baseline,a.output)
    pending=dict(baseline);pending['status']='preparing'
    (a.output/'provenance.json').write_text(json.dumps(pending,indent=2)+'\n')
    configs=[(104,'mag_audio_a_verified_',a.audio_elf,0x13AB8,[],segments)]
    if a.edge_raw:
        configs.append((105,'mag_edgegeom_verified_',a.edge_wrapper,0x15B50,[0x15944,0x15898,0x7A40,0xB368],[(0x4030,a.edge_raw.read_bytes()[0x30:0x11B50])]))
    images=[];records=[]
    for ident,prefix,elf,end,extra,segs in configs:
        d=a.output/f'mag_verified_{ident}'
        cmd=[sys.executable,str(lifter),'--auto-functions',str(elf),'--code-end',hex(end),'--symbol-prefix',prefix,'--verified-code-image',str(ident),'--output',str(d)]
        if extra: cmd+=['--extra-funcs',','.join(hex(x) for x in extra)]
        if sdk_identity(a.sdk,a.sdk_revision)!=sdk_pin: raise ValueError('SDK pin drift before lift')
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
        if sdk_identity(a.sdk,a.sdk_revision)!=sdk_pin: raise ValueError('SDK pin drift after lift')
        (a.output/f'lift-{ident}.log').write_text(r.stdout+r.stderr)
        if r.returncode: raise ValueError('lifter failed')
        for f in [d/'spu_recomp.c',d/'spu_recomp.h']: f.write_text(f.read_text().replace('../../runtime/spu/',''))
        c=(d/'spu_recomp.c').read_text(); count=len(re.findall(r'^void '+prefix+r'spu_func_',c,re.M));guard=len(re.findall(r'if \(!spu_verified_code_enter\(ctx, '+str(ident)+',',c))
        if count!=profile['audio_A' if ident==104 else 'edge_partial']['expected_function_count']: raise ValueError('unexpected function census')
        if count!=guard or re.search(r'unsupported|UNSUPPORTED|\.word',c): raise ValueError('unguarded or unsupported lift')
        records.append({'image_id':ident,'policy_image_id':101,'prefix':prefix,'function_count':count,'entry_guards':guard,'input_sha256':digest(elf),'command':cmd,'SDK_pin':sdk_pin,'segments':[{'lsa':x,'size':len(y),'sha256':hashlib.sha256(y).hexdigest()} for x,y in segs]})
        images.append((ident,prefix,segs))
    (a.output/'jobmods_register.c').write_text(emit_registry(images))
    for rel,h in baseline['outputs'].items():
        if digest(a.output/rel)!=h: raise ValueError('baseline source changed')
    baseline['verified_overlay_extension']={'profile_sha256':digest(a.profile),'profile':profile,'lifter_sha256':digest(lifter),'SDK_pin':sdk_pin,'images':records,'baseline_provenance_sha256':baseline_provenance_sha,'scope':'opt-in exact-byte guarded transfer; no mid-function SMC guarantee; unknown PCs remain interpreter'}
    baseline['outputs']={str(f.relative_to(a.output)):digest(f) for f in sorted(a.output.rglob('*')) if f.suffix in ('.c','.h')}
    if sdk_identity(a.sdk,a.sdk_revision)!=sdk_pin: raise ValueError('SDK pin drift before completion')
    if digest(a.baseline/'provenance.json')!=baseline_provenance_sha or digest(a.profile)!='b846690d488c6cc0e0e5189cf777b41137c488091369805f04c882575cb4aa82': raise ValueError('profile drift before completion')
    (a.output/'provenance.json').write_text(json.dumps(baseline,indent=2)+'\n')
    print(json.dumps({'output':str(a.output),'images':records,'provenance_sha256':digest(a.output/'provenance.json')},indent=2))
if __name__=='__main__': main()
