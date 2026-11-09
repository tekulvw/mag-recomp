"""Cohort stage: ownership and publication of the retained visibility cohorts.

Three kinds of hook, all observers:

- entry scopes on the job allocator, submitter, barrier, resets and producer,
  so the bridge knows which retained job a frame's work belongs to;
- scopes around selected original calls (identified by their return address),
  each keeping the complete original call and its drain inside the scope;
- on the two job-return functions, the returning job's address is read just
  before the original stores it back to the pool, and the pool is reported
  again on the original's success branch.

The sort selection and fog parameters get one call each at a fixed seam.
Every original call, drain, write and return is kept. Guard:
MAG_RENDER_ADAPTER_COHORT.
"""
from tools.render_hooks import common

NAME = 'cohort'
GUARD = 'defined(MAG_RENDER_ADAPTER_COHORT) && MAG_RENDER_ADAPTER_COHORT'
INCLUDE = '#if ' + GUARD + '\n#include "mag_render_cohort_bridge.h"\n#endif\n'
# Caller -> (return address of the original call, callee, role).
CALLS = {
    '00013C54': [('00014200', '00E493E0', 'Regular')],
    '00D11810': [('00D11834', '00D0C944', 'Regular')],
    '000DEB4C': [('000DED2C', '00E4C5B0', 'Minimap')],
    '00D116B8': [('00D11714', '00D10EB8', 'Main')],
    '00D0C944': [('00D0D0B8', '00CFD9F0', 'DepthSort')],
    '00D0C12C': [('00D0C3A8', '00CFE7B0', 'DepthRoot')],
    '00D10EB8': [('00D11268', '00CFD9F0', 'Sort'),
                 ('00D11358', '00DFD2DC', 'Join'),
                 ('00D11368', '00DFD2DC', 'Join'),
                 ('00D11438', '00CFE7B0', 'Root')],
}
ENTRIES = {'00DFCCA4': 'Allocate', '00DFBCAC': 'Submit',
           '00A07080': 'Barrier', '00A04688': 'ResetAll',
           '00D16CEC': 'Producer',
           **dict.fromkeys(('00A04778', '00A04F90', '00A05040', '00A050F0'), 'Reset')}
# Job-return function -> (register holding the returning job, success label).
RETURNS = {'00DFD2DC': (8, '00DFD410'), '00DFD51C': (30, '00DFD59C')}
SORT_SELECTED = '00CFDF44'
FOG_PARAMETERS = '00D1A430'
# The fog parameters are complete once this original call and its drain return.
FOG_SEAM = '        ctx->lr = 0x00D1A824; func_00D22878(ctx); DRAIN_TRAMPOLINE(ctx);'
# Bound on the distance between a call's return-address store and the call.
CALL_WINDOW = 1024
PINS = {
    '00013C54': {'sha256': 'ae1db95ed4ba73fb73ebeb35886f0b47e5c0e9c3acf1c156f103fc767fa2e704', 'file': 'ppu_recomp_000.cpp'},
    '000DEB4C': {'sha256': '4696701ba5305dffe55b107369d3aa3bea7f381803c64f4472ebf805b8d2cb11', 'file': 'ppu_recomp_000.cpp'},
    '00A04688': {'sha256': '15f48c6e4208dead857df4ecac0140cb67e3c611ddc3cb232a70178602876a72', 'file': 'ppu_recomp_015.cpp'},
    '00A04778': {'sha256': 'db6ad03d1c6a270d69ec7480da9d2794b3bd2f532827f2270160339724316113', 'file': 'ppu_recomp_004.cpp'},
    '00A04F90': {'sha256': '8db3f8140ba27ef8a6a74e8fa7f93caa6a68c3e59a03fc5ea00900afe70b70c4', 'file': 'ppu_recomp_004.cpp'},
    '00A05040': {'sha256': '0c3591886247d6e4f586f79f3b4d0354d1455aca4381e3b13845a5ae15b83353', 'file': 'ppu_recomp_004.cpp'},
    '00A050F0': {'sha256': 'dc30dc486979fa7fac78cd4118a367d7c774551f6eada0a29408d411ac1f1e7b', 'file': 'ppu_recomp_004.cpp'},
    '00A07080': {'sha256': '416f0e9532f6286391b293057bff40be710d55e8345dfed76119dcf8448e007c', 'file': 'ppu_recomp_004.cpp'},
    '00CFDF44': {'sha256': 'd85fab698f7471aeadef4d6d2a186b5d627bdbd71e688a0be1142f0a4af8fcb5', 'file': 'ppu_recomp_013.cpp'},
    '00D0C12C': {'sha256': '7539b1201116d93c866734981408361d856e5c56200882f87d775bccb99a8f7a', 'file': 'ppu_recomp_005.cpp'},
    '00D0C944': {'sha256': 'cd0ed80d92545a48f97f40a9d827daea44448a6b3c3a00e94b1ba629e61e9e0e', 'file': 'ppu_recomp_005.cpp'},
    '00D10EB8': {'sha256': '5f76e3551933b5882fa49e5d321a32194892c6630aa653f2c8a6d792e26c69ce', 'file': 'ppu_recomp_005.cpp'},
    '00D116B8': {'sha256': '635b55f7542b5feb7e5d11446d6cc940372e4a30236f2d6ff49c165b8f771d40', 'file': 'ppu_recomp_005.cpp'},
    '00D11810': {'sha256': '875e6b567fddc2cfbad598acfb2ea3e61afe09a03eeb0833d544dd7476dd7706', 'file': 'ppu_recomp_005.cpp'},
    '00D16CEC': {'sha256': '743089057d48a0a8c8c37a3f04e2a8b73a6d2a6d407f75ebcd637173723d6d6c', 'file': 'ppu_recomp_005.cpp'},
    '00D1A430': {'sha256': '97bca16b629bc049442175f8f461cbd42b98209ee8bd4d47480f45ed60f47d02', 'file': 'ppu_recomp_005.cpp'},
    '00DFBCAC': {'sha256': '479c1ea369e3afea197f6944545683a329b666164e9a5228402802a8b55c6d67', 'file': 'ppu_recomp_005.cpp'},
    '00DFCCA4': {'sha256': '5313696e062c6dcb7af6ef7d20a981e034238524f548288fd84ed3cc3490b3e5', 'file': 'ppu_recomp_005.cpp'},
    '00DFD2DC': {'sha256': '68a6300fbd99595e0b5298fa9b94e00ad5cf817cee2d87a20af22abcc3acd18e', 'file': 'ppu_recomp_005.cpp'},
    '00DFD51C': {'sha256': '0d8f877406b6463430f7c4bbe25a31714cf0dd6e136a9f42e6e5e5b483391f7b', 'file': 'ppu_recomp_005.cpp'},
}
# The update function also carries update-loop and controls hooks; all three pin it identically.
SHARED = {'00013C54'}
# Callees the scoped calls reach; verified, never changed.
DEPENDENCIES = {
    '00CFE7B0': {'sha256': 'b5fe71ca91c1cb6da578ed10f9014b4bdc187a2873df7e2b6fd746e24ed0e853', 'file': 'ppu_recomp_005.cpp'},
    '00CFD9F0': {'sha256': '4f7adcc9f0a1c8bc9de1dae1db48b95e38364436f4618d9db6c494df319a3802', 'file': 'ppu_recomp_005.cpp'},
    '00CFE834': {'sha256': '49d89530e946cdfa02c2803c894fe51598130d07804ac613816e5d1bb3d1b600', 'file': 'ppu_recomp_005.cpp'},
}


def _guarded(text):
    return common.guarded(GUARD, text)


def patches(name):
    result = []
    entry = common.signature(name)
    if name in ENTRIES:
        result.append((entry, entry + _guarded(
            f'        MagRenderCohortScope mag_render_cohort_scope(ctx,MagRenderCohortRole::{ENTRIES[name]});')))
    if name == SORT_SELECTED:
        result.append((entry, entry + _guarded('        mag_render_cohort_sort_selected(ctx);')))
    if name == FOG_PARAMETERS:
        result.append((FOG_SEAM, FOG_SEAM + _guarded('        mag_render_cohort_fog_parameters(ctx);')))
    if name in RETURNS:
        register, label = RETURNS[name]
        result.append((entry, entry + _guarded('        uint32_t mag_render_returning_job=0;')))
        store = f'        vm_write32(ctx->gpr[{register}] + 0x288, ctx->gpr[0]);'
        result.append((store, _guarded(
            f'        mag_render_returning_job=static_cast<uint32_t>(ctx->gpr[{register}]);\n'
            '        mag_render_cohort_pool_before(mag_render_returning_job);') + store))
        # The success branch: CR7 EQ set (CR bit 30), branching to the pinned label.
        branch = f'        if (((ctx->cr >> 1) & 1)) goto loc_{label};\n'
        result.append((branch, branch + _guarded('        mag_render_cohort_pool_returned(mag_render_returning_job);')))
    return result


def _call(target):
    return f'func_{target}(ctx); DRAIN_TRAMPOLINE(ctx);'


def _wrapped(lr, target, role):
    return (_guarded(f'        {{ MagRenderCohortScope mag_render_cohort_{lr}(ctx,MagRenderCohortRole::{role});')
            + _call(target) + _guarded('        }'))


def strip_body(body, name):
    body = common.remove_patches(body, patches(name))
    for lr, target, role in CALLS.get(name, []):
        body = body.replace(_wrapped(lr, target, role), _call(target))
    return body


def instrument_body(body, name):
    for lr, target, role in CALLS.get(name, []):
        anchor = f'ctx->lr = 0x{lr};'
        if body.count(anchor) != 1:
            raise common.HookError(f'{NAME} {name}: call returning to {lr} is not unique')
        start = body.index(anchor)
        at = body.find(_call(target), start)
        if at < 0 or at - start > CALL_WINDOW:
            raise common.HookError(f'{NAME} {name}: call returning to {lr} has no complete call and drain')
        body = body[:at] + _wrapped(lr, target, role) + body[at + len(_call(target)):]
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
