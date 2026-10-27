"""Prepare an exact private residual-renderer overlay; no guest bytes in Git."""
import argparse,hashlib,json,pathlib,re,subprocess,sys
from tools.verified_spu_jobs import digest,sdk_identity,fresh_output,copy_baseline,executable_sections,emit_registry
PREFIX='mag_renderer_perf_'
def main():
 p=argparse.ArgumentParser()
 for n in ('profile','sdk','baseline','original','snapshot','output'):p.add_argument('--'+n,type=pathlib.Path,required=True)
 p.add_argument('--sdk-revision',required=True);a=p.parse_args();profile=json.loads(a.profile.read_text())
 if profile.get('schema')!=1 or profile.get('image_id')!=107 or profile.get('policy_image_id')!=101:raise ValueError('Unsupported profile')
 if digest(a.original)!=profile['ELF_sha256'] or digest(a.snapshot)!=profile['snapshot_sha256']:raise ValueError('Original/snapshot identity mismatch')
 original=a.original.read_bytes();ls=a.snapshot.read_bytes();offset=profile['module_file_offset'];size=profile['initialized_bytes'];base=profile['LS_base'];raw=original[offset:offset+size]
 if len(raw)!=size or hashlib.sha256(raw).hexdigest()!=profile['raw_sha256'] or ls[base:base+size]!=raw:raise ValueError('Complete initialized module mismatch')
 start,end=profile['selected_start'],profile['selected_end'];code=raw[start-base:end-base]
 if hashlib.sha256(code).hexdigest()!=profile['selected_sha256']:raise ValueError('Selected executable mismatch')
 pin=sdk_identity(a.sdk,a.sdk_revision);provenance=digest(a.baseline/'provenance.json');record=json.loads((a.baseline/'provenance.json').read_text())
 if record.get('status')!='complete' or record.get('renderer_performance_extension'):raise ValueError('Invalid baseline')
 for name,h in record['outputs'].items():
  if digest(a.baseline/name)!=h:raise ValueError('Baseline source drift')
 output=fresh_output(a.output);copy_baseline(a.baseline,output)
 (output/'provenance.json').write_text(json.dumps(dict(record,status='preparing'),indent=2)+'\n')
 sys.path.insert(0,str(a.sdk/'tools'));from wrap_spu_elf import wrap
 wrapper=wrap(code,base=start,entry=profile['entry']);path=output/'renderer-text.elf';path.write_bytes(wrapper)
 if executable_sections(wrapper)!=[(start,code)]:raise ValueError('Wrapper coverage mismatch')
 dest=output/'mag_renderer_107';command=[sys.executable,str(a.sdk/'tools/spu_lifter.py'),'--auto-functions',str(path),'--code-end',hex(end),'--extra-funcs',','.join(hex(x) for x in profile['extra_entries']),'--symbol-prefix',PREFIX,'--verified-code-image','107','--verified-code-spans','--output',str(dest)]
 result=subprocess.run(command,capture_output=True,text=True,timeout=120);(output/'renderer-lift.log').write_text(result.stdout+result.stderr)
 if result.returncode:raise ValueError('Renderer lift failed')
 for f in (dest/'spu_recomp.c',dest/'spu_recomp.h'):f.write_text(f.read_text().replace('../../runtime/spu/',''))
 source=(dest/'spu_recomp.c').read_text();functions=re.findall(r'^void '+PREFIX+r'spu_func_([0-9A-F]+)\(spu_context\*',source,re.M);guards=re.findall(r'spu_verified_code_enter_span\(ctx, 107, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\)',source)
 if not functions or len(functions)!=len(guards) or [int(x,16) for x in functions]!=[int(x,16) for x,y in guards] or any(int(y,16)<=int(x,16) or int(x,16)<start or int(y,16)>end for x,y in guards):raise ValueError('Incomplete function-span admission')
 if re.search(r'TODO|unsupported|UNSUPPORTED|\.word',source):raise ValueError('Unsupported generated instructions')
 # Actual entry activation is observed only after the original exact span gate.
 m=re.search(r'(void mag_renderer_perf_spu_func_00017A30\(spu_context\* ctx\) \{\n    if \(!spu_verified_code_enter_span[^\n]+\n)',source)
 if not m:raise ValueError('Missing pinned native entry guard')
 source=source[:m.end()]+'    mag_graphics_renderer_enter(ctx);\n'+source[m.end():]
 source='#include "mag_graphics_payload_trace.h"\n'+source
 (dest/'spu_recomp.c').write_text(source)

 registry=emit_registry([(107,PREFIX,[(start,code)])]).replace('#include <stdlib.h>','#include <stdlib.h>\n#include <string.h>').replace('mag_register_verified_overlays','mag_register_renderer_overlay');needle='static void mag_register_renderer_overlay(void) {';registry=registry.replace(needle,needle+'\n  const char *on=getenv("MAG_COMPILED_RENDERER");\n  if(!on || strcmp(on,"1"))return;')
 with (output/'jobmods_register.c').open('a') as f:f.write('\n'+registry)
 for name,h in record['outputs'].items():
  if name!='jobmods_register.c' and digest(output/name)!=h:raise ValueError('Inherited generated source changed')
 if pin!=sdk_identity(a.sdk,a.sdk_revision) or provenance!=digest(a.baseline/'provenance.json'):raise ValueError('Input drift')
 record['renderer_performance_extension']={'profile_sha256':digest(a.profile),'preparer_sha256':digest(pathlib.Path(__file__)),'baseline_provenance_sha256':provenance,'SDK_pin':pin,'image':107,'function_count':len(functions),'validation':'full executable image at lookup, complete function spans at direct entry','runtime_toggle':'MAG_COMPILED_RENDERER=1; default off','raw_sha256':profile['raw_sha256'],'text_sha256':profile['selected_sha256'],'snapshot_sha256':profile['snapshot_sha256'],'command':command,'scope':profile['contract']}
 record['outputs']={str(f.relative_to(output)):digest(f) for f in sorted(output.rglob('*')) if f.suffix in ('.c','.h','.cpp')}
 (output/'provenance.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps({'output':str(output),'functions':len(functions),'provenance_sha256':digest(output/'provenance.json')},indent=2))
if __name__=='__main__':main()
