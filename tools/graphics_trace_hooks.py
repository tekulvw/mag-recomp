"""Exact retail function-body hooks, including generated direct callers."""
import hashlib,json,re,os
from pathlib import Path
from tools.prepare_inputs import sha256_file,verify_hash
from tools.movie_picture_hooks import RETAIL_ELF
SHAPES={'0023D7D0': '0f4f3c219cf7e77a3a033fcca453aa80d9f4c4be962e00a53ec011e0441d20e2', '00A047D8': 'd82fe1aa47697a98b46959f5e79115f60dc6ad4bb8cce7b28c1b642c19d454c8', '00A05438': '470b08be0a440e138ea57f9769a6066362e2f6403326181d0a26bde9691bd402', '01111740': '6e32e64f069b71f9104e8f13681793ce8cd658e72df1ff624ca22aeb854f0214', '00DFD2DC': '50f6e35b16015573a004879329894dd8f330b4c61eb2b37217583849364f630b', '00DFBCAC': 'd50ce683af503dc76daa3cfbcdffe8965172b608d96a6242176ee4e762299070'}
SHAPES.update({'00A04778': '3800bf3d3e9bc997ce0887fa9919270a3a358550fa70f530f92f9eb27ac0f3ef'})
def instrument_source(source,pins=None):
 pins=SHAPES if pins is None else pins
 for name,digest in pins.items():
  start="void func_"+name+"(ppu_context* ctx) {"
  if start not in source:continue
  m=re.search(re.escape(start)+r".*?^}",source,re.M|re.S)
  if not m:raise ValueError("Unknown graphics function shape "+name)
  body=m[0];hook="\n        MagGraphicsTraceScope mag_graphics_trace_scope(ctx,0x"+name+");"
  original=re.sub(r"\n        mag_graphics_trace_scope\.marker\(\d+,\(uint16_t\)ctx->gpr\[\d+\]\);","",body.replace(hook,"",1))
  if hashlib.sha256(original.encode()).hexdigest()!=digest:raise ValueError("Graphics function hash mismatch "+name)
  new=original.replace(start,start+hook,1)
  if name=='00A047D8':
   loads=list(re.finditer(r'(        ctx->gpr\[(\d+)\] = vm_read16\(ctx->gpr\[4\] \+ 0x[0-9A-F]+\);)',new))
   if len(loads)!=7:raise ValueError('Original predicate must contain exactly seven halfword loads')
   for index,load in reversed(list(enumerate(loads))):new=new[:load.end()]+f'\n        mag_graphics_trace_scope.marker({index},(uint16_t)ctx->gpr[{load[2]}]);'+new[load.end():]
  source=source[:m.start()]+new+source[m.end():]
 if "MagGraphicsTraceScope mag_graphics_trace_scope" in source and '#include "mag_graphics_trace.h"' not in source:source='#include "mag_graphics_trace.h"\n'+source
 return source
def apply_hooks(directory,elf_hash):
 if elf_hash!=RETAIL_ELF:raise ValueError("Graphics hooks require exact retail ELF")
 record=json.loads((directory/'provenance.json').read_text())
 if record['elf_sha256']!=elf_hash or record['status']!='complete':raise ValueError("Unverified lift")
 for name,digest in record['outputs'].items():verify_hash(directory/name,digest)
 changed={};seen=set()
 for path in directory.glob('ppu_recomp_*.cpp'):
  old=path.read_text();new=instrument_source(old)
  for name in SHAPES:
   if 'void func_'+name+'(ppu_context* ctx) {' in old:seen.add(name)
  if old!=new:changed[path.name]=(old,new)
 if seen!=set(SHAPES):raise ValueError("Missing required graphics hooks")
 if not changed and record.get('graphics_trace_hooks'):return record
 evidence={'tool_sha256':sha256_file(Path(__file__)),'elf_sha256':elf_hash,'source_function_sha256':SHAPES,'original_outputs':{n:record['outputs'][n] for n in changed},'meaning':'RAII scopes retain original instruction/direct-call order; pointer observations noncoherent'}
 for name,(_,new) in changed.items():
  temporary=directory/(name+'.graphics-trace-tmp');temporary.write_text(new);os.replace(temporary,directory/name)
 for name in changed:record['outputs'][name]=sha256_file(directory/name)
 record['graphics_trace_hooks']=evidence
 temporary=directory/'provenance.graphics-trace-tmp.json';temporary.write_text(json.dumps(record,indent=2)+'\n');os.replace(temporary,directory/'provenance.json');return record
