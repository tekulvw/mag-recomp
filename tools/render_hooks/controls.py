"""Controls stage: the seams of the opt-in game controls adapter (docs/native-controls.md).

Three kinds of hook, each one guarded line set:

- entry/exit scopes around whole functions (the update tick, the input poll, a
  handler update, handler lifecycle functions, the actor pre-update, two
  angle-rate helpers and the assist cleanup);
- early-result entries on the two assist queries: when the adapter answers, the
  function returns at once with the result register the adapter set;
- points before or after one statement. The statement is named by what it is
  (a call by its return address and callee, a load, store, multiply or compare by
  its registers and offset, or a label) and rendered here in the pinned lifter's
  output form; the recognition point continues at a named label when the adapter
  has published the control's result.

Without the guard every hooked function is the original program. Guard:
MAG_GAME_CONTROLS_HOOKS.
"""
from tools.render_hooks import common

NAME = 'controls'
GUARD = 'defined(MAG_GAME_CONTROLS_HOOKS) && MAG_GAME_CONTROLS_HOOKS'
INCLUDE = '#if ' + GUARD + '\n#include "mag_game_controls_hooks.h"\n#endif\n'


def call(lr, callee):
    """A call whose return address is `lr`, with its drain."""
    return f'        ctx->lr = 0x{lr}; func_{callee}(ctx); DRAIN_TRAMPOLINE(ctx);'


def load_single(target, base, offset):
    """fpr[target] = single-precision float at gpr[base] + offset."""
    return (f'        {{ uint32_t tmp = vm_read32(ctx->gpr[{base}] + 0x{offset:X}); float ftmp; '
            f'memcpy(&ftmp, &tmp, 4); ctx->fpr[{target}] = ftmp; }}')


def load_double_word(target, base, offset):
    """gpr[target] = 64-bit word at gpr[base] + offset."""
    return f'        ctx->gpr[{target}] = vm_read64(ctx->gpr[{base}] + 0x{offset:X});'


def store_word(base, offset, source):
    """32-bit word at gpr[base] + offset = gpr[source]."""
    return f'        vm_write32(ctx->gpr[{base}] + 0x{offset:X}, ctx->gpr[{source}]);'


def multiply_single(target, a, b):
    """fpr[target] = single-precision fpr[a] * fpr[b]."""
    return f'        ctx->fpr[{target}] = ppu_fp_single(ppu_fmul(ctx->fpr[{a}], ctx->fpr[{b}]));'


def compare_float(a, b, field=0):
    """Unordered compare of fpr[a] with fpr[b] into condition field `field`."""
    shift = 4 * field
    return (f'        {{ double a = ctx->fpr[{a}]; double b = ctx->fpr[{b}]; uint32_t cr_val = '
            f'(a != a || b != b) ? 1 : (a < b) ? 8 : (a > b) ? 4 : 2; '
            f'ctx->cr = (ctx->cr & ~(0xFu << {shift})) | (cr_val << {shift}); }}')


def label(address):
    return f'loc_{address}:'


# Function -> (entry event, exit event) of a whole-function scope.
SCOPES = {
    '00013C54': ('OuterEnter', 'OuterExit'),
    '003A5E2C': ('PollEnter', 'PollExit'),
    '003A2C84': ('HandlerEnter', 'HandlerExit'),
    '003A0E30': ('HandlerRemove', 'HandlerRemoveExit'),
    '003A3368': ('HandlerDestroy', 'HandlerDestroyed'),
    '003A36E4': ('HandlerDestroy', 'HandlerDestroyed'),
    '003A1F9C': ('HandlerConstruct', 'HandlerConstructed'),
    '003A23C0': ('HandlerAdd', 'HandlerAdded'),
    '003A2130': ('HandlerRefresh', 'HandlerRefreshed'),
    '006415D0': ('ActorEnter', 'ActorExit'),
    '0061E524': ('YawRateEnter', 'YawRateExit'),
    '005DA0F0': ('SprintPitchRateEnter', 'SprintPitchRateExit'),
    '004F1EEC': ('AssistCleanupEnter', 'AssistCleanupExit'),
}
# Function -> event whose true answer returns from the function at once.
EARLY = {'004E42BC': 'AssistEligibility', '004F07D0': 'AssistAdhesion'}
# Function -> [(event, placement, statement, continuation label or None)].
POINTS = {
    '003A5E2C': [('SampleSeal', 'before', load_single(30, 2, 0x2D18), None)],
    '003A2C84': [
        ('ControlLoop', 'after', label('003A2D30'), None),
        ('ControlRecognize', 'before', call('003A2DA0', '0039FBE4'), '003A2E58'),
        ('ControlCallback', 'before', store_word(29, 0x18, 0), None),
    ],
    '006415D0': [('Prepare', 'after', call('00641790', '006406E0'), None)],
    '00611D80': [
        ('BasicYaw', 'before', load_single(31, 3, 0x188), None),
        ('BasicPitch', 'before', compare_float(31, 30), None),
    ],
    '00676D2C': [
        ('SprintPitch', 'before', call('00676DCC', '0061199C'), None),
        ('SprintYaw', 'before', call('00676DEC', '005E47D0'), None),
    ],
    '0061E524': [('YawDamage', 'before', load_double_word(0, 1, 0xB0), None)],
    '005DA0F0': [('SprintPitchDamage', 'before', multiply_single(31, 31, 1), None)],
    '004E1DA8': [('AssistFrictionYaw', 'before', multiply_single(1, 0, 1), None)],
    '004E1E10': [('AssistFrictionPitch', 'before', multiply_single(1, 0, 1), None)],
}
PINS = {
    '00013C54': {'sha256': 'd88de44f6b44e8115eeed4b594e56059545e20b44c1f6f1d47c97e154689aa95', 'file': 'ppu_recomp_000.cpp'},
    '003A0E30': {'sha256': 'ee3199b3bc3d4d59015c51ebf2c29b81d6728797dd90de1e3bf398d067557978', 'file': 'ppu_recomp_001.cpp'},
    '003A1F9C': {'sha256': 'b1c46fb8b152702b8a5e621b5f531cfd27bbf8acc2ea5c3a9a1870cd4d1a7654', 'file': 'ppu_recomp_001.cpp'},
    '003A2130': {'sha256': '0433affd16e408656f28a7519a4db75e5cebdfc6f2d508ce43671561821b4430', 'file': 'ppu_recomp_001.cpp'},
    '003A23C0': {'sha256': '4780eba9cebc1d9f3b6e462ecebc05a67abdd6bb3acca189435364435e098d3e', 'file': 'ppu_recomp_001.cpp'},
    '003A2C84': {'sha256': '9f732d7ed5ed0ad02517c4a70ef752087b69d19f9c5b5423b748a71bbe8dfc3f', 'file': 'ppu_recomp_001.cpp'},
    '003A3368': {'sha256': '1a52aa1ded5609556a1fb0acf12009ae4987458ab847041b66f72edfb684dc8c', 'file': 'ppu_recomp_001.cpp'},
    '003A36E4': {'sha256': '20c1f682fc66a7db94b52bde56593fc7e8e0a7de89f42deca976d8afe0fc3a97', 'file': 'ppu_recomp_001.cpp'},
    '003A5E2C': {'sha256': '286283a0586a6fc73753cc7a1637b70f086c4fd943545a63f9947828cdddb151', 'file': 'ppu_recomp_001.cpp'},
    '004E1DA8': {'sha256': '31f20012b2586171cce9e01db3cf7621a561cb79eb6601566f7b4611856a2486', 'file': 'ppu_recomp_002.cpp'},
    '004E1E10': {'sha256': '101becd8f81fecb65ccb4a1bbb8bec500aafb5691432106655b993ffcdf7ad47', 'file': 'ppu_recomp_002.cpp'},
    '004E42BC': {'sha256': 'ae5c5a50224f4b57f948d639bfbad5f9e0ed4fcdc6db47430d72d5ae6750a169', 'file': 'ppu_recomp_002.cpp'},
    '004F07D0': {'sha256': '778ac35df5cc74b56f978f09efad10b0c1f186382088dc9fabb0150d98d86630', 'file': 'ppu_recomp_002.cpp'},
    '004F1EEC': {'sha256': '5a447285229955b1bce51b34a9d44cf97f6b391a56bc37e8876de52f6f0703f8', 'file': 'ppu_recomp_002.cpp'},
    '005DA0F0': {'sha256': '734a4e3d2a52c20bc3098a87b61d47c453f8c3f7e1cfc258fb458c29fd0e31b4', 'file': 'ppu_recomp_002.cpp'},
    '00611D80': {'sha256': 'c04c1ac754201c8a6d3003488e3b1de635b3dfcec4616b7278ed521cc8b33298', 'file': 'ppu_recomp_002.cpp'},
    '0061E524': {'sha256': '4bb6c37807ab4ad2fa94d15c10de4ccaf4456ad1df78a992e57de51be85100de', 'file': 'ppu_recomp_002.cpp'},
    '006415D0': {'sha256': '6d726222bb6a4b85118467d26f2832ca1940d2dfb75726f5220931bdbc3ee6a6', 'file': 'ppu_recomp_002.cpp'},
    '00676D2C': {'sha256': 'b1b1b5a2046016231259b1ccd1f7aab708aee384e717fc2a1c9ea4b3f481dd84', 'file': 'ppu_recomp_002.cpp'},
}
# The update tick also carries update-loop and cohort hooks; all three pin it identically.
SHARED = {'00013C54'}
# UI functions the adapter's eligibility rules depend on; verified, never changed.
DEPENDENCIES = {
    '0023F108': {'sha256': 'c06078a61eb2ec168ad442de19a85bc27464bbf4026b40dfced38c80e8ce7b2f', 'file': 'ppu_recomp_000.cpp'},
    '0026F91C': {'sha256': '16ead7d076c23d3286928a039820caaa633084867f1799f5d76126d99c3b6cdf', 'file': 'ppu_recomp_001.cpp'},
    '002726B8': {'sha256': '88b684eea89b4396bdf2da62219f2e311622e42891bd3e162759e86a45986d7a', 'file': 'ppu_recomp_001.cpp'},
}


def _guarded(text):
    return common.guarded(GUARD, text)


def _hook(event):
    return f'mag_game_controls_hook(MagControlsHook::{event}, ctx)'


def patches(name):
    entry = common.signature(name)
    result = []
    if name in SCOPES:
        enter, leave = SCOPES[name]
        result.append((entry, entry + _guarded(
            f'        MagGameControlsHookScope mag_game_controls_scope(MagControlsHook::{enter}, MagControlsHook::{leave}, ctx);')))
    if name in EARLY:
        result.append((entry, entry + _guarded(f'        if ({_hook(EARLY[name])}) return;')))
    for event, placement, statement, continuation in POINTS.get(name, []):
        line = (f'        if ({_hook(event)}) goto loc_{continuation};' if continuation
                else f'        (void){_hook(event)};')
        result.append((statement, _guarded(line) + statement if placement == 'before' else statement + _guarded(line)))
    return result


def strip_body(body, name):
    return common.remove_patches(body, patches(name))


def required(name):
    """Lines that must appear exactly once in `name` without being changed: continuation labels."""
    return [label(continuation) for _, _, _, continuation in POINTS.get(name, []) if continuation]


def instrument_body(body, name):
    for line in required(name):
        if body.count(f'\n{line}\n') != 1:
            raise common.HookError(f'{NAME} {name}: continuation {line} is not unique')
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
