"""Prepare a private, runtime-selectable geometry lift for matched native trials."""
import argparse,hashlib,json,pathlib,re,subprocess,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from verified_spu_jobs import digest,sdk_identity,fresh_output,copy_baseline,executable_sections,emit_registry
PREFIX='mag_edgegeom_perf_'
def registry_source(segments):
 s=emit_registry([(105,PREFIX,segments)])
 s=s.replace('#include <stdlib.h>','#include <stdlib.h>\n#include <string.h>')
 needle='__attribute__((constructor)) static void mag_register_verified_overlays(void) {'
 assert s.count(needle)==1
 return s.replace(needle,needle+'\n  const char* enabled=getenv("MAG_COMPILED_GEOMETRY");\n  if (!enabled || strcmp(enabled,"1")) return;')
def guard_census(source,spans):
 functions=re.findall(r'^void '+PREFIX+r'spu_func_([0-9A-F]+)\(spu_context\*',source,re.M)
 if spans:
  guards=re.findall(r'spu_verified_code_enter_span\(ctx, 105, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\)',source)
  if len(guards)!=len(functions) or [int(a,16) for a,b in guards]!=[int(x,16) for x in functions] or any(int(b,16)<=int(a,16) or (int(a,16)|int(b,16))&3 for a,b in guards):raise ValueError('incomplete or invalid generated guard spans')
 else:
  if len(re.findall(r'spu_verified_code_enter\(ctx, 105,',source))!=len(functions):raise ValueError('incomplete full-image gates')
 if re.search(r'unsupported|UNSUPPORTED|\.word',source):raise ValueError('unsupported instructions')
 return len(functions)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ['profile','sdk','baseline','edge-raw','edge-wrapper','output']:p.add_argument('--'+name,type=pathlib.Path,required=True)
 p.add_argument('--sdk-revision',required=True);p.add_argument('--validation',choices=['full','span'],default='span');a=p.parse_args()
 if digest(a.profile)!='b846690d488c6cc0e0e5189cf777b41137c488091369805f04c882575cb4aa82':raise ValueError('unreviewed profile')
 profile=json.loads(a.profile.read_text());edge=profile['edge_partial'];raw=a.edge_raw.read_bytes();wrapper=a.edge_wrapper.read_bytes()
 if hashlib.sha256(raw).hexdigest()!=edge['raw_sha256'] or len(raw)!=edge['initialized_bytes']:raise ValueError('raw geometry identity')
 start,end=int(edge['selected_start'],0),int(edge['selected_end'],0)
 segments=[(start,raw[start-0x4000:end-0x4000])]
 if executable_sections(wrapper)!=segments:raise ValueError('wrapper executable coverage')
 pin=sdk_identity(a.sdk,a.sdk_revision);base_digest=digest(a.baseline/'provenance.json');base=json.loads((a.baseline/'provenance.json').read_text())
 if base.get('status')!='complete' or base.get('verified_overlay_extension'):raise ValueError('baseline must contain original four images only')
 for rel,h in base['outputs'].items():
  if digest(a.baseline/rel)!=h:raise ValueError('baseline drift')
 output=fresh_output(a.output);copy_baseline(a.baseline,output)
 pending=dict(base,status='preparing');(output/'provenance.json').write_text(json.dumps(pending,indent=2)+'\n')
 dest=output/'mag_verified_105'
 command=[sys.executable,str(a.sdk/'tools/spu_lifter.py'),'--auto-functions',str(a.edge_wrapper),'--code-end',hex(end),'--extra-funcs',','.join(edge['extra_entries']),'--symbol-prefix',PREFIX,'--verified-code-image','105','--output',str(dest)]
 if a.validation=='span':command+=['--verified-code-spans']
 result=subprocess.run(command,capture_output=True,text=True,timeout=90);(output/'geometry-lift.log').write_text(result.stdout+result.stderr)
 if result.returncode:raise ValueError('geometry lift failed')
 for file in [dest/'spu_recomp.c',dest/'spu_recomp.h']:file.write_text(file.read_text().replace('../../runtime/spu/',''))
 count=guard_census((dest/'spu_recomp.c').read_text(),a.validation=='span')
 if count!=edge['expected_function_count']:raise ValueError('geometry function census changed')
 (output/'jobmods_register.c').write_text(registry_source(segments))
 if pin!=sdk_identity(a.sdk,a.sdk_revision) or digest(a.baseline/'provenance.json')!=base_digest:raise ValueError('input drift')
 for rel,h in base['outputs'].items():
  if digest(output/rel)!=h:raise ValueError('original four images changed')
 base['geometry_performance_extension']={'profile_sha256':digest(a.profile),'SDK_pin':pin,'image':105,'validation':a.validation,'function_count':count,'runtime_toggle':'MAG_COMPILED_GEOMETRY=1; disabled otherwise','raw_sha256':digest(a.edge_raw),'wrapper_sha256':digest(a.edge_wrapper),'command':command,'scope':'complete function-span checks at direct entries; full-image indirect admission; no running-body mutation support'}
 base['outputs']={str(f.relative_to(output)):digest(f) for f in sorted(output.rglob('*')) if f.suffix in ('.c','.h','.cpp')}
 (output/'provenance.json').write_text(json.dumps(base,indent=2)+'\n')
 print(json.dumps({'output':str(output),'functions':count,'SDK':pin,'provenance_sha256':digest(output/'provenance.json')},indent=2))
if __name__=='__main__':main()
