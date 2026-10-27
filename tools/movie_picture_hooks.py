"""Retained exact-image hooks for immutable retail movie texture references."""
import json
import re
from pathlib import Path
from tools.prepare_inputs import sha256_file, verify_hash

RETAIL_ELF='0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c'

def instrument_source(source: str) -> str:
    for name in ('009841FC','00E14D48'):
        start='void func_'+name+'(ppu_context* ctx) {'
        if start not in source: continue
        match=re.search(re.escape(start)+r'.*?^}',source,re.M|re.S)
        if not match: raise ValueError('Unknown generated movie function shape: '+name)
        body=match.group()
        if name=='009841FC':
            marker='        vm_write32(ctx->gpr[11] + 0x4, ctx->gpr[10]);'
            if 'mag_movie_picture_command(' in body: continue
            if body.count(marker)!=1: raise ValueError('Unknown retail texture copy shape')
            body=body.replace(marker,marker+'\n        mag_movie_picture_command((uint32_t)(ctx->gpr[11]+4),(uint32_t)ctx->gpr[31],(uint32_t)ctx->gpr[10]);',1)
        else:
            if 'MagMoviePictureScope mag_picture_scope' in body: continue
            body=body.replace(start,start+'\n        MagMoviePictureScope mag_picture_scope((uint32_t)ctx->gpr[3]);',1)
        source=source[:match.start()]+body+source[match.end():]
    if 'mag_movie_picture_command(' in source or 'MagMoviePictureScope mag_picture_scope' in source:
        include='#include "mag_movie_picture.h"\n'
        if include not in source: source=include+source
    return source

def apply_hooks(directory: Path, elf_hash: str) -> dict:
    if elf_hash!=RETAIL_ELF: raise ValueError('Movie hooks are pinned to retail v1.0 mage_g')
    record=json.loads((directory/'provenance.json').read_text())
    if record['elf_sha256']!=elf_hash or record['status']!='complete': raise ValueError('Unverified lift')
    for name,digest in record['outputs'].items(): verify_hash(directory/name,digest)
    changed={}
    for path in directory.glob('ppu_recomp_*.cpp'):
        old=path.read_text();new=instrument_source(old)
        if old!=new: changed[path.name]=(old,new)
    if not changed and record.get('movie_picture_hooks'): return record
    if len(changed)!=2: raise ValueError('Expected both retail descriptor and FIFO functions')
    evidence=dict(tool_sha256=sha256_file(Path(__file__)),elf_sha256=elf_hash,
        original_outputs={n:record['outputs'][n] for n in changed},
        meaning='Preserve original instructions; bracket selected frame and capture immutable identity before PUT exposure')
    for name,(_,new) in changed.items(): (directory/name).write_text(new)
    for name in changed: record['outputs'][name]=sha256_file(directory/name)
    record['movie_picture_hooks']=evidence
    (directory/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
    return record
