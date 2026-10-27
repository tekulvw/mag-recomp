"""Add an independently selectable exact-span audio lift to private geometry output."""
import argparse,hashlib,importlib.util,json,pathlib,re,subprocess,sys
from verified_spu_jobs import digest,sdk_identity,fresh_output,copy_baseline,executable_sections,emit_registry
PREFIX='mag_audio_span_perf_'
GEOMETRY_SHA='4c7fc874b50d7f2105561fdcbf2011eb33105a19a151a7804c137729b333c74d'
PROFILE_SHA='b846690d488c6cc0e0e5189cf777b41137c488091369805f04c882575cb4aa82'
def registry_source(segments):
 s=emit_registry([(104,PREFIX,segments)])
 s=s.replace('#include <stdlib.h>','#include <stdlib.h>\n#include <string.h>')
 needle='__attribute__((constructor)) static void mag_register_verified_overlays(void) {'
 assert s.count(needle)==1
 return s.replace(needle,'__attribute__((constructor)) static void mag_register_audio_span_overlay(void) {\n  const char* enabled=getenv("MAG_COMPILED_AUDIO");\n  if (!enabled || strcmp(enabled,"1")) return;')
def guard_census(source,segments):
 funcs=list(re.finditer(r'^void '+PREFIX+r'spu_func_([0-9A-F]+)\(spu_context\*',source,re.M))
 if not funcs or len({m[1] for m in funcs})!=len(funcs):raise ValueError('empty or duplicate function census')
 total=len(re.findall(r'spu_verified_code_enter_span\(',source))
 if total!=len(funcs) or 'spu_verified_code_enter(ctx' in source:raise ValueError('incomplete span gates')
 for i,f in enumerate(funcs):
  body=source[f.start():funcs[i+1].start() if i+1<len(funcs) else len(source)]
  guards=re.findall(r'spu_verified_code_enter_span\(ctx, 104, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\)',body)
  if len(guards)!=1:raise ValueError('missing or duplicate body gate')
  a,b=(int(v,16) for v in guards[0])
  if a!=int(f[1],16) or b<=a or (a|b)&3 or not any(s<=a<b<=s+len(data) for s,data in segments):raise ValueError('invalid body coverage')
 if re.search(r'unsupported|UNSUPPORTED|\.word',source):raise ValueError('unsupported instructions')
 return len(funcs)
def source_inventory(root):
 return {str(p.relative_to(root)) for p in root.rglob('*') if p.suffix in ('.c','.h','.cpp')}
def verify_sources(root,manifest):
 if source_inventory(root)!=set(manifest):raise ValueError('source inventory differs from manifest')
 for rel,h in manifest.items():
  if digest(root/rel)!=h:raise ValueError('source hash drift')
def covered_functions(bounds,segments,code_end):
 selected=[(s,min(e,code_end)) for s,e in bounds if s<code_end]
 kept=[];excluded=[]
 for s,e in selected:
  if s<e and not (s|e)&3 and any(a<=s<e<=a+len(data) for a,data in segments):kept.append((s,e))
  else:excluded.append((s,e))
 return kept,excluded
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('profile','sdk','geometry','audio-elf','output'):p.add_argument('--'+n,type=pathlib.Path,required=True)
 p.add_argument('--sdk-revision',required=True);a=p.parse_args()
 if digest(a.profile)!=PROFILE_SHA:raise ValueError('unreviewed profile')
 profile=json.loads(a.profile.read_text());audio=profile['audio_A']
 if digest(a.audio_elf)!=audio['ELF_sha256']:raise ValueError('audio identity')
 segs=executable_sections(a.audio_elf.read_bytes())
 if [(s,len(b)) for s,b in segs]!=[(int(s),int(n)) for s,n in audio['segments']]:raise ValueError('audio code map')
 pin=sdk_identity(a.sdk,a.sdk_revision);base_sha=digest(a.geometry/'provenance.json');base=json.loads((a.geometry/'provenance.json').read_text())
 if base_sha!=GEOMETRY_SHA:raise ValueError('unreviewed geometry preparation')
 if base.get('status')!='complete' or base.get('verified_overlay_extension') or base.get('audio_performance_extension') or base.get('geometry_performance_extension',{}).get('validation')!='span':raise ValueError('expected geometry-only span baseline')
 verify_sources(a.geometry,base['outputs'])
 out=fresh_output(a.output);copy_baseline(a.geometry,out)
 pending=dict(base,status='preparing');(out/'provenance.json').write_text(json.dumps(pending,indent=2)+'\n')
 dest=out/'mag_verified_104'
 if dest.exists():raise ValueError('audio image already exists')
 spec=importlib.util.spec_from_file_location('pinned_audio_function_detector',a.sdk/'tools/find_spu_functions.py');detector=importlib.util.module_from_spec(spec);spec.loader.exec_module(detector)
 blob=a.audio_elf.read_bytes();bounds,(offset,base_addr,size)=detector.detect_functions(blob)
 kept,excluded=covered_functions(bounds,segs,int(audio['code_end'],0))
 if excluded!=[(0x4000,0x4014)] or len(kept)!=audio['expected_function_count']-1:raise ValueError('unexpected uncovered function census')
 raw=out/'inputs/audio-span-text.bin';raw.write_bytes(blob[offset:offset+size])
 functions=out/'inputs/audio-span-functions.json';functions.write_text(json.dumps([{'start':hex(s),'end':hex(e)} for s,e in kept],indent=2)+'\n')
 cmd=[sys.executable,str(a.sdk/'tools/spu_lifter.py'),str(raw),'--base',hex(base_addr),'--functions',str(functions),'--symbol-prefix',PREFIX,'--verified-code-image','104','--verified-code-spans','--output',str(dest)]
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=90);(out/'audio-lift.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise ValueError('audio lift failed')
 for f in (dest/'spu_recomp.c',dest/'spu_recomp.h'):f.write_text(f.read_text().replace('../../runtime/spu/',''))
 count=guard_census((dest/'spu_recomp.c').read_text(),segs)
 if count!=len(kept):raise ValueError('audio function census')
 registry=out/'jobmods_register.c';original=registry.read_bytes();registry.write_bytes(original+b'\n'+registry_source(segs).encode())
 if source_inventory(out)!=set(base['outputs'])|{'mag_verified_104/spu_recomp.c','mag_verified_104/spu_recomp.h'}:raise ValueError('unexpected prepared sources')
 for rel,h in base['outputs'].items():
  if rel!='jobmods_register.c' and digest(out/rel)!=h:raise ValueError('baseline output changed')
 if not registry.read_bytes().startswith(original):raise ValueError('geometry registration changed')
 if pin!=sdk_identity(a.sdk,a.sdk_revision) or digest(a.geometry/'provenance.json')!=base_sha or digest(a.audio_elf)!=audio['ELF_sha256'] or digest(a.profile)!=PROFILE_SHA:raise ValueError('input drift')
 verify_sources(a.geometry,base['outputs'])
 base['audio_performance_extension']={'SDK_pin':pin,'profile_sha256':PROFILE_SHA,'input_sha256':audio['ELF_sha256'],'baseline_provenance_sha256':base_sha,'image':104,'validation':'span','function_count':count,'excluded_uncovered_bounds':[[hex(s),hex(e)] for s,e in excluded],'exclusion_reason':'GUID entry extends beyond verified section into padding; remains interpreted','detector_sha256':digest(a.sdk/'tools/find_spu_functions.py'),'raw_input_sha256':digest(raw),'bounds_input_sha256':digest(functions),'runtime_toggle':'MAG_COMPILED_AUDIO=1; disabled otherwise','command':cmd,'scope':'Exact complete body spans; full-image indirect admission; existing readonly-running-body contract retained'}
 base['outputs']={str(f.relative_to(out)):digest(f) for f in sorted(out.rglob('*')) if f.suffix in ('.c','.h','.cpp')}
 (out/'provenance.json').write_text(json.dumps(base,indent=2)+'\n')
 print(json.dumps({'output':str(out),'functions':count,'SDK':pin,'provenance_sha256':digest(out/'provenance.json')},indent=2))
if __name__=='__main__':main()
