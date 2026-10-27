#!/usr/bin/env python3
"""Append one replacement PSARC block; preserve unrelated bytes and verify members.
Only supports a one-block existing/replacement member, PSARC 1.3 zlib, 30-byte TOC.
Input must be the decrypted PSAR file, output a distinct new disposable file.
"""
import argparse, hashlib, json, pathlib, struct, zlib

def digest(b): return hashlib.sha256(b).hexdigest()
def parse(d):
 if len(d)<32: raise ValueError('Truncated header')
 magic,ver,comp,toc,entrysz,count,bs,flags=struct.unpack('>4sI4sIIIII',d[:32])
 if (magic,ver,comp,entrysz)!=(b'PSAR',0x10003,b'zlib',30): raise ValueError('Unsupported archive header')
 if bs<2 or bs&(bs-1): raise ValueError('Unsupported block size')
 width=((bs-1).bit_length()+7)//8; start=32+entrysz*count
 if start>toc or toc>len(d) or (toc-start)%width:raise ValueError('Invalid block table')
 sizes=[int.from_bytes(d[x:x+width],'big') or bs for x in range(start,toc,width)]
 entries=[]
 for i in range(count):
  e=d[32+entrysz*i:32+entrysz*(i+1)]
  idx=int.from_bytes(e[16:20],'big'); length=int.from_bytes(e[20:25],'big'); off=int.from_bytes(e[25:30],'big')
  blocks=(length+bs-1)//bs
  if idx+blocks>len(sizes) or off<toc or off+sum(sizes[idx:idx+blocks])>len(d):raise ValueError('Invalid member bounds')
  entries.append((idx,length,off,blocks))
 def extract(i):
  idx,length,off,blocks=entries[i]; chunks=[]
  for bi in range(blocks):
   n=sizes[idx+bi]; chunk=d[off:off+n]; expected=min(bs,length-bi*bs)
   if n==expected: out=chunk
   else: out=zlib.decompress(chunk)
   if len(out)!=expected:raise ValueError('Invalid decoded block length')
   chunks.append(out);off+=n
  return b''.join(chunks)
 names=extract(0).decode('utf-8').splitlines()
 if len(names)!=count-1:raise ValueError('Unexpected name count')
 return entries,names,extract,start,width,bs

def replace(src,member,payload):
 entries,names,extract,start,width,bs=parse(src)
 if names.count(member)!=1:raise ValueError('Member not uniquely present')
 target=names.index(member)+1;idx,length,off,blocks=entries[target]
 if blocks!=1 or not 0<len(payload)<=bs:raise ValueError('Both versions must be one block')
 for i,(other_idx,_,_,other_blocks) in enumerate(entries):
  if i!=target and other_idx<=idx<other_idx+other_blocks:raise ValueError('Target shares a block table entry')
 compressed=zlib.compress(payload,9); stored=compressed if len(compressed)<len(payload) else payload
 if len(stored)>=(1<<(width*8)):raise ValueError('Stored block too large')
 out=bytearray(src)
 out[32+target*30+20:32+target*30+25]=len(payload).to_bytes(5,'big')
 out[32+target*30+25:32+target*30+30]=len(src).to_bytes(5,'big')
 out[start+idx*width:start+(idx+1)*width]=len(stored).to_bytes(width,'big')
 out.extend(stored); out=bytes(out)
 after_entries,after_names,after_extract,*_=parse(out)
 if after_names!=names:raise ValueError('Names changed')
 for i in range(len(entries)):
  expected=payload if i==target else extract(i)
  if after_extract(i)!=expected:raise ValueError(f'Member {i} differs')
 return out,{'member':member,'entry':target,'members_verified':len(entries),'unrelated_members_unchanged':len(entries)-1,'before_member_sha256':digest(extract(target)),'after_member_sha256':digest(payload),'source_sha256':digest(src),'output_sha256':digest(out),'source_bytes':len(src),'output_bytes':len(out),'old_payload_offset':off,'new_payload_offset':len(src),'appended_compressed_bytes':len(stored),'format':'PSARC 1.3 zlib; unencrypted private runtime copy'}

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=pathlib.Path);p.add_argument('member');p.add_argument('replacement',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args()
 if a.source.resolve()==a.output.resolve() or a.output.exists():p.error('Output must be a distinct new file')
 out,report=replace(a.source.read_bytes(),a.member,a.replacement.read_bytes())
 with a.output.open('xb') as f:f.write(out)
 print(json.dumps(report,indent=2))
