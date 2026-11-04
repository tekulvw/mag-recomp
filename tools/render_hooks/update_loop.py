"""Update-loop stage: which accepted engine update a thread's render work belongs to.

The engine's frame loop calls its update function from two call sites. A scope
around each complete call (and its drain) marks one update loop on that thread.
Inside the update function, once the original call that accepts the frame's
time step has returned, one call reports the accepted update. The bridge then
gives later hooks on that thread a nonzero update identity, and zero outside an
accepted update. Every original call, drain, write and return is kept. Guard:
MAG_RENDER_ADAPTER_UPDATE_LOOP.
"""
from tools.render_hooks import common

NAME = 'update-loop'
GUARD = 'defined(MAG_RENDER_ADAPTER_UPDATE_LOOP) && MAG_RENDER_ADAPTER_UPDATE_LOOP'
INCLUDE = '#if ' + GUARD + '\n#include "mag_render_update_loop.h"\n#endif\n'
UPDATE = '00013C54'
# Return addresses of the two calls to the update function in each loop copy.
LOOP_CALLS = ('00010DD8', '00010E14')
# The update is accepted once this original call and its drain return. The
# accepted time step is then in the register the hook reads.
ACCEPTED = '        ctx->lr = 0x00013EA4; func_00E49380(ctx); DRAIN_TRAMPOLINE(ctx);\n'
ACCEPTED_STEP = 'ctx->fpr[31]'
CALL_WINDOW = 1024
PINS = {
    '00010AE8': {'sha256': '8a5a4630026885a79bf3ceaa4bedec66bddd178b88666c7c6e2bb63501de0abc', 'file': 'ppu_recomp_000.cpp'},
    '00010AF8': {'sha256': '76650fcb75a8de78a5ac782210d6be01dfb6538aa519417a881cbe73e1e1e579', 'file': 'ppu_recomp_008.cpp'},
    '00013C54': {'sha256': 'd88de44f6b44e8115eeed4b594e56059545e20b44c1f6f1d47c97e154689aa95', 'file': 'ppu_recomp_000.cpp'},
}
# The update function also carries cohort hooks; both stages pin it identically.
SHARED = {UPDATE}
# The callee whose return accepts the update; verified, never changed.
DEPENDENCIES = {
    '00E49380': {'sha256': '404d3c8e95c3e942a49ccd0d1b2003b60b5638b4ae07b567b07867751030231c', 'file': 'ppu_recomp_005.cpp'},
}


def _guarded(text):
    return common.guarded(GUARD, text)


def _call():
    return f'func_{UPDATE}(ctx); DRAIN_TRAMPOLINE(ctx);'


def _wrapped(lr):
    return (_guarded(f'        {{ MagRenderUpdateLoopScope mag_render_update_loop_{lr}(ctx);')
            + _call() + _guarded('        }'))


def patches(name):
    if name == UPDATE:
        return [(ACCEPTED, ACCEPTED + _guarded(f'        mag_render_update_accepted(ctx,{ACCEPTED_STEP});'))]
    return []


def strip_body(body, name):
    body = common.remove_patches(body, patches(name))
    if name != UPDATE:
        for lr in LOOP_CALLS:
            body = body.replace(_wrapped(lr), _call())
    return body


def instrument_body(body, name):
    if name != UPDATE:
        for lr in LOOP_CALLS:
            anchor = f'ctx->lr = 0x{lr};'
            if body.count(anchor) != 1:
                raise common.HookError(f'{NAME} {name}: call returning to {lr} is not unique')
            start = body.index(anchor)
            at = body.find(_call(), start)
            if at < 0 or at - start > CALL_WINDOW:
                raise common.HookError(f'{NAME} {name}: call returning to {lr} has no complete call and drain')
            body = body[:at] + _wrapped(lr) + body[at + len(_call()):]
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
