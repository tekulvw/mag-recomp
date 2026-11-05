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
    '00013C54': {'sha256': 'd88de44f6b44e8115eeed4b594e56059545e20b44c1f6f1d47c97e154689aa95', 'file': 'ppu_recomp_000.cpp'},
    '000DEB4C': {'sha256': '762987efc6ff7c7ba3de5b8df7b42d3e060909cebd08e99d546aeab694d82ec8', 'file': 'ppu_recomp_000.cpp'},
    '00A04688': {'sha256': '15f48c6e4208dead857df4ecac0140cb67e3c611ddc3cb232a70178602876a72', 'file': 'ppu_recomp_015.cpp'},
    '00A04778': {'sha256': 'ffc05e00cec24d2873ceff36abb64558d9a6189bdf5540c5dc7233ca95d2c041', 'file': 'ppu_recomp_004.cpp'},
    '00A04F90': {'sha256': 'dbda923058791b3a179147dd791c8f552fa2462944737df97983ad013bc31ed0', 'file': 'ppu_recomp_004.cpp'},
    '00A05040': {'sha256': '570c13a63dff26c365546009d5cc303b8ccf1390aeeb1c3a012b6df4356c6787', 'file': 'ppu_recomp_004.cpp'},
    '00A050F0': {'sha256': '66d4ce55887a8513e855ba48f18246c6899cb12decc54c74823f4518d3d542a1', 'file': 'ppu_recomp_004.cpp'},
    '00A07080': {'sha256': '416f0e9532f6286391b293057bff40be710d55e8345dfed76119dcf8448e007c', 'file': 'ppu_recomp_004.cpp'},
    '00CFDF44': {'sha256': 'd85fab698f7471aeadef4d6d2a186b5d627bdbd71e688a0be1142f0a4af8fcb5', 'file': 'ppu_recomp_013.cpp'},
    '00D0C12C': {'sha256': 'e30a48ea5557790658fd81b17cf1bbc9d5b2c3fcc1ebcdb90614907ba88bbaa8', 'file': 'ppu_recomp_005.cpp'},
    '00D0C944': {'sha256': '32cd2d508a6e43c78945f2902a79f7d327547ea49bcd741a72f40987f2fed2e7', 'file': 'ppu_recomp_005.cpp'},
    '00D10EB8': {'sha256': 'cd0d34d8c0e97c75b1a593e5de556768ecbb9c80cf251da3cdc39718c7be5d5f', 'file': 'ppu_recomp_005.cpp'},
    '00D116B8': {'sha256': '245ea04ed3e5aecfd270249bfa4297e09e1325cb2ad1845a5457deca377c21c1', 'file': 'ppu_recomp_005.cpp'},
    '00D11810': {'sha256': '0773966bc6ab23c2dfa23115e68876aa11d9737499f29295b480a871ad4aee1d', 'file': 'ppu_recomp_005.cpp'},
    '00D16CEC': {'sha256': '14de6479e886f4fecf5e42ebde1ab3b8698e7dc6225447067ea7e289c330d7db', 'file': 'ppu_recomp_005.cpp'},
    '00D1A430': {'sha256': 'f13e911608c738c9cf47d90c12ccf12919919e95194ac2d4dc13d879c1b2735c', 'file': 'ppu_recomp_005.cpp'},
    '00DFBCAC': {'sha256': '479c1ea369e3afea197f6944545683a329b666164e9a5228402802a8b55c6d67', 'file': 'ppu_recomp_005.cpp'},
    '00DFCCA4': {'sha256': '3300fe83aaa0b664c27831e50d00e8becf42f8a418cceb528e4202c5b137e6a5', 'file': 'ppu_recomp_005.cpp'},
    '00DFD2DC': {'sha256': 'b95d8f627b1929b7496c1244160aba99ed09c29c33133faf891ea86fce319e0c', 'file': 'ppu_recomp_005.cpp'},
    '00DFD51C': {'sha256': '6689f10fb845d64e3cd6227c37788ef772845a27b33a3ad50ab83ec88a76a5c9', 'file': 'ppu_recomp_005.cpp'},
}
# The update function also carries update-loop hooks; both stages pin it identically.
SHARED = {'00013C54'}
# Callees the scoped calls reach; verified, never changed.
DEPENDENCIES = {
    '00CFE7B0': {'sha256': '463389e1d79077cae76ab60f3d01c61e9b686a939799f2ce516418e449f1182a', 'file': 'ppu_recomp_005.cpp'},
    '00CFD9F0': {'sha256': 'a1459c850f23a3978f62ff37c59e3ce2fc4ee3a921bd66df7b522a748d2ba4bb', 'file': 'ppu_recomp_005.cpp'},
    '00CFE834': {'sha256': '4b1f2e83708d0ef5f844ee15f9d753000b4351c5c4f6b37be574b604f764dff8', 'file': 'ppu_recomp_005.cpp'},
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
        branch = f'        if (((ctx->cr >> 0) & 2)) goto loc_{label};\n'
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
