"""One pinned original policy BISL plus genuine drain/continuation, no guest changes."""
import hashlib,re,json,os
from pathlib import Path
from tools.prepare_inputs import sha256_file,verify_hash
POLICY_SHA='6a3af90518af72ed790b36b2081ab2040237d378bf3644d4461356b44bcfc58a'
# The pinned policy lift's payload function: lifted with the runtime pin's lifter and
# --promote-regs (profiles/retail-v1.0-engine-spu.json, policy.lifter/lifter_args).
FUNCTION_SHA='7bf7133fc56fc72e59f2552a8426f781fc0ab0fbe6dc275a654afe9cd36ccdfe'
# The payload call. A register-promoted lift stores its live registers to ctx->gpr between
# the host-depth increment and the call; the trace begins after them, so it sees them.
CALL='spu_indirect_branch(ctx); spu_drain_call(ctx, 0x3260);'
CALL_SITE=re.compile(r'ctx->host_depth\+\+; (?:ctx->gpr\[\d+\] = SPU_PR_U\(rg\d+\); )*'+re.escape(CALL))
BEFORE='MagGraphicsPayloadTrace mag_payload_trace; mag_graphics_payload_begin(&mag_payload_trace,ctx); '
AFTER=' mag_graphics_payload_end(&mag_payload_trace,ctx);'
def instrument_source(source,pin=None):
 m=re.search(r'void mag_wws_policy_spu_func_00003190\(spu_context\* ctx\) \{.*?^}',source,re.M|re.S)
 if not m:raise ValueError('Missing original policy payload function')
 body=m[0];original=body.replace(BEFORE,'',1).replace(AFTER,'',1)
 if hashlib.sha256(original.encode()).hexdigest()!=(FUNCTION_SHA if pin is None else pin):raise ValueError('Original policy function mismatch')
 if body==original:
  if original.count(CALL)!=1 or len(CALL_SITE.findall(original))!=1:raise ValueError('Original call/continuation mismatch')
  body=original.replace(CALL,BEFORE+CALL+AFTER,1);source=source[:m.start()]+body+source[m.end():]
 include='#include "mag_graphics_payload_trace.h"\n'
 return source if include in source else include+source
def apply_hooks(directory):
 record=json.loads((directory/'provenance.json').read_text())
 if record.get('policy_sha256')!=POLICY_SHA or record.get('status')!='complete':raise ValueError('Policy identity mismatch')
 for name,digest in record['outputs'].items():verify_hash(directory/name,digest)
 path=directory/'mag_wws_policy/spu_recomp.c';old=path.read_text();new=instrument_source(old)
 if new==old and record.get('graphics_payload_hooks'):return record
 record['graphics_payload_hooks']={'policy_sha256':POLICY_SHA,'original_function_sha256':FUNCTION_SHA,'tool_sha256':sha256_file(Path(__file__)),'original_source_sha256':sha256_file(path),'call_pc':'0x325C','continuation':'0x3260','scope':'Observe original requested call, actual original drain return; preserve all original guest instructions and runtime calls'}
 t=path.with_suffix('.payload-tmp');t.write_text(new);os.replace(t,path)
 key=next((k for k in record['outputs'] if k.replace('\\','/')=='mag_wws_policy/spu_recomp.c'),'mag_wws_policy/spu_recomp.c')
 record['outputs'][key]=sha256_file(path)
 t=directory/'provenance.payload-tmp.json';t.write_text(json.dumps(record,indent=2)+'\n');os.replace(t,directory/'provenance.json');return record
