"""Pinned observations after four original effective-threshold stores."""
import hashlib,json,re,os
from pathlib import Path
from tools.prepare_inputs import sha256_file,verify_hash
from tools.movie_picture_hooks import RETAIL_ELF
PINS={'00CFDB5C': {'sha256': '07ffb217be9312bb79e5cfb5ccefb691ccd330dde312991f2946c4c8265776a5', 'labels': ['00CFDC28', '00CFDD24', '00CFDE20'], 'file': 'ppu_recomp_013.cpp'}, '00CFDC5C': {'sha256': 'b092b529ac34c1a5fc67fb30c519721f42737c657406aec7fee5bcf23ea02642', 'labels': ['00CFDD24', '00CFDE20'], 'file': 'ppu_recomp_013.cpp'}, '00CFDD58': {'sha256': 'af25cc609dedafe3587abd316b6668ed6ff0991209bb09c54c18cedd198c0aac', 'labels': ['00CFDE20'], 'file': 'ppu_recomp_013.cpp'}, '00CFDA98': {'sha256': 'cb0e0005a666c33403dbf99633eaa96f9863d2b6dfe1ed469bd6c2a7ed54eea6', 'labels': ['00CFDB20', '00CFDC28', '00CFDD24', '00CFDE20'], 'file': 'ppu_recomp_010.cpp'}}
STORES={'00CFDB20':(0xCFDB24,8,0),'00CFDC28':(0xCFDC2C,10,1),'00CFDD24':(0xCFDD28,10,2),'00CFDE20':(0xCFDE24,8,3)}
def instrument_source(source,pins=None):
 pins=PINS if pins is None else pins
 for name,pin in pins.items():
  start='void func_'+name+'(ppu_context* ctx) {'
  if start not in source:continue
  match=re.search(re.escape(start)+r'.*?^}',source,re.M|re.S)
  if not match:raise ValueError('Missing pinned threshold function '+name)
  body=match[0];original=re.sub(r'\n        mag_graphics_occlusion_store\([^\n]*\);','',body)
  if hashlib.sha256(original.encode()).hexdigest()!=pin['sha256']:raise ValueError('Threshold source mismatch '+name)
  changed=original
  for label in pin['labels']:
   pc,base,partition=STORES[label]
   pattern=r'(loc_'+label+r':\n[^\n]*\n[^\n]*vm_write32[^\n]*;)' 
   found=list(re.finditer(pattern,changed))
   if len(found)!=1:raise ValueError('Threshold store coverage mismatch '+label)
   at=found[0].end();hook='\n        mag_graphics_occlusion_store(ctx,0x%X,(uint32_t)(ctx->gpr[%d]+0x18),(uint32_t)ctx->gpr[23],(uint32_t)ctx->gpr[0],%d);'%(pc,base,partition)
   changed=changed[:at]+hook+changed[at:]
  source=source[:match.start()]+changed+source[match.end():]
 if 'mag_graphics_occlusion_store(ctx,' in source and '#include "mag_graphics_trace.h"' not in source:source='#include "mag_graphics_trace.h"\n'+source
 return source

def apply_hooks(directory,elf_hash):
 if elf_hash!=RETAIL_ELF:raise ValueError('Threshold hooks require exact retail ELF')
 record=json.loads((directory/'provenance.json').read_text())
 if record['elf_sha256']!=elf_hash or record['status']!='complete':raise ValueError('Unverified threshold lift')
 for name,digest in record['outputs'].items():verify_hash(directory/name,digest)
 changed={};seen=set()
 for filename in sorted({p['file'] for p in PINS.values()}):
  path=directory/filename;old=path.read_text();new=instrument_source(old)
  for name in PINS:
   if 'void func_'+name+'(ppu_context* ctx) {' in old:seen.add(name)
  if old!=new:changed[filename]=(old,new)
 if seen!=set(PINS):raise ValueError('Missing threshold function copies')
 if not changed and record.get('occlusion_store_hooks'):return record
 evidence={'tool_sha256':sha256_file(Path(__file__)),'elf_sha256':elf_hash,'source_function_pins':PINS,'original_outputs':{n:record['outputs'][n] for n in changed},'meaning':'After original effective-threshold stores; original instructions retained; no query result behavior change'}
 for name,(_,new) in changed.items():
  temporary=directory/(name+'.occlusion-tmp');temporary.write_text(new);os.replace(temporary,directory/name)
 for name in changed:record['outputs'][name]=sha256_file(directory/name)
 record['occlusion_store_hooks']=evidence
 temporary=directory/'provenance.occlusion-tmp.json';temporary.write_text(json.dumps(record,indent=2)+'\n');os.replace(temporary,directory/'provenance.json');return record
