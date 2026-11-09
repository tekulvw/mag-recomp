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
    '00013C54': {'sha256': 'ae1db95ed4ba73fb73ebeb35886f0b47e5c0e9c3acf1c156f103fc767fa2e704', 'file': 'ppu_recomp_000.cpp'},
    '003A0E30': {'sha256': 'cf7751610d12ab96e9a3abb5dbedf608ccc318529c153857102bc5d7b54e6287', 'file': 'ppu_recomp_001.cpp'},
    '003A1F9C': {'sha256': 'e825b1535a7357214fefeca4b16fc1145cd441687ea8648bb830f35cd5ee11d8', 'file': 'ppu_recomp_001.cpp'},
    '003A2130': {'sha256': '3e575132fef3ebcb089923def06d892850beb54478560196f9cad80a3c3a4b5f', 'file': 'ppu_recomp_001.cpp'},
    '003A23C0': {'sha256': '10eaeedbdb7281c02ba06b48c657bc9f658a2a664275c4307c9ddaaf5ceaf7e7', 'file': 'ppu_recomp_001.cpp'},
    '003A2C84': {'sha256': 'ed68681285bb3e968b442fe3299ad9b8abb023117266dc7873795a568df99a93', 'file': 'ppu_recomp_001.cpp'},
    '003A3368': {'sha256': '7e13c09fea9cd6c1926e37deb131d9b9820e079206661b23f335ad4f9eca71f5', 'file': 'ppu_recomp_001.cpp'},
    '003A36E4': {'sha256': '22b75f5e0798f29565b1d24b995a14637bfec632cc6a613349d7ecb36447dd61', 'file': 'ppu_recomp_001.cpp'},
    '003A5E2C': {'sha256': '14eb6fd4c4dc05965e65271e5f636c7f2afef09d739a6efc1fc6bc900277cba2', 'file': 'ppu_recomp_001.cpp'},
    '004E1DA8': {'sha256': 'c2d0c7015e9b8f9f118a4a28e5fded9fc305d0425e320190d05908f708fa0a61', 'file': 'ppu_recomp_002.cpp'},
    '004E1E10': {'sha256': '18564ea71fd9b716b100fc275f201469979ac2a4d924e4dbff8ac06f21890a92', 'file': 'ppu_recomp_002.cpp'},
    '004E42BC': {'sha256': '5329e71f95669ce3afb1e6e00e47644010626353d29242fd678d636c7d945a57', 'file': 'ppu_recomp_002.cpp'},
    '004F07D0': {'sha256': 'ae9c682f28cb93a0fce795f385a337eca80e9ec36e6f3dcfee45059885e90f09', 'file': 'ppu_recomp_002.cpp'},
    '004F1EEC': {'sha256': '1c69de6e83c54bf32df33f68fed334d1e238d04974f1780aa945b70e3178c81f', 'file': 'ppu_recomp_002.cpp'},
    '005DA0F0': {'sha256': '9025cff085347076021b06cb2ad566eeedd5ef3af9a43d5113667d05c147a149', 'file': 'ppu_recomp_002.cpp'},
    '00611D80': {'sha256': 'affb0531c07d6cd1387797783bcac314bbd6997bceb51c44c5b2a6a5274699e8', 'file': 'ppu_recomp_002.cpp'},
    '0061E524': {'sha256': '12d8d6770223b7fd28d819adc9b572c7e1f1a9e58bd9794b50b29dcd89b4d1bd', 'file': 'ppu_recomp_002.cpp'},
    '006415D0': {'sha256': 'e449e9228adf18fe3f268792cd61659151ee4f65e9adc70ea7368016e6995abc', 'file': 'ppu_recomp_002.cpp'},
    '00676D2C': {'sha256': '8e73a555f96b45dd861c7d6ecc3cb9b05fe448cfc119f1b00fb7e2190cf21002', 'file': 'ppu_recomp_002.cpp'},
}
# The update tick also carries update-loop and cohort hooks; all three pin it identically.
SHARED = {'00013C54'}
# UI functions the adapter's eligibility rules depend on; verified, never changed.
DEPENDENCIES = {
    '0023F108': {'sha256': 'a737b005e4b46fd489f8277c5881e171d6c5290dde7dac91524bc5c8bb771292', 'file': 'ppu_recomp_000.cpp'},
    '0026F91C': {'sha256': '8b614e07251147930efccd71037f2bce63e12013a0cbdd48ac8400f0326f4bed', 'file': 'ppu_recomp_001.cpp'},
    '002726B8': {'sha256': '5ba983c6b0e8faa9bf6d989ac4b4fa09fe3b062c844f37cfe2d2f273d61fa4f5', 'file': 'ppu_recomp_001.cpp'},
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
