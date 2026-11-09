"""UI handoff stage: the original UI packet writers at the frame's pre-UI boundary.

Each pinned writer gets one scope at entry. The word writer also reports just
before and just after its final header store, and the scissor writer reports
after its last parameter store, so the bridge sees the packet the original
actually wrote. Original packets, calls, drains and returns are unchanged.
Guard: MAG_RENDER_ADAPTER_UI_HANDOFF.
"""
from tools.render_hooks import common

NAME = 'ui-handoff'
GUARD = 'defined(MAG_RENDER_ADAPTER_UI_HANDOFF) && MAG_RENDER_ADAPTER_UI_HANDOFF'
INCLUDE = '#if ' + GUARD + '\n#include "mag_render_ui_handoff.h"\n#endif\n'
SCOPES = {'0024EDF0': 'MagRenderUiBeginScope',
          '00982504': 'MagRenderUiWordScope',
          '00D084E8': 'MagRenderTargetSourceScope',
          '009825B0': 'MagRenderSurfacePacketScope',
          '0098368C': 'MagRenderViewportPacketScope',
          '009835B4': 'MagRenderScissorPacketScope'}
# Unique stores that close a packet: (data store, header store) of the word
# writer and the final parameter store of the scissor writer.
WORD_DATA_STORE = '        vm_write32(ctx->gpr[11] + 0x4, ctx->gpr[9]);'
WORD_HEADER_STORE = '        vm_write32(ctx->gpr[11] + 0x0, ctx->gpr[10]);'
SCISSOR_FINAL_STORE = '        vm_write32(ctx->gpr[8] + 0x4, ctx->gpr[10]);'
PINS = {
    '0024EDF0': {'sha256': '8253176182df482f1b4391e8695985698cabcdf7aefbb9b66b4d744d63cfcf7d', 'file': 'ppu_recomp_000.cpp'},
    '00982504': {'sha256': '334009a552da6bf41831affa837f0524373f2bdbb449f2ca44a1e36751b3117e', 'file': 'ppu_recomp_004.cpp'},
    '00D084E8': {'sha256': 'a08b771941315b322c2900303de5a92d188c40ef5e6007bdb5e72c08fd16574b', 'file': 'ppu_recomp_005.cpp'},
    '009825B0': {'sha256': '1470c352f0e9469df747ef545a0436ccedf29776272a30261a8874265386e118', 'file': 'ppu_recomp_004.cpp'},
    '0098368C': {'sha256': '9a3b6c3cc04ee6a17ce34448a9ec53492439f7c93872be8fc284917dfc6854f3', 'file': 'ppu_recomp_004.cpp'},
    '009835B4': {'sha256': '4defd5d2d7d69cfc26190a4f228c329c94b4998e67d27fde365da6f5be96fa6a', 'file': 'ppu_recomp_004.cpp'},
}
# Callees the writers reach before their final stores; verified, never changed.
DEPENDENCIES = {
    '00E4E9C0': {'sha256': '2b62665798556d09942fab0d3610c8cbd8b0073c45e2f6386ad829ab20aad4d1', 'file': 'ppu_recomp_005.cpp'},
    '00E59B80': {'sha256': 'c320c2dfab8234d8c4d4784e39c8d40485b7ece3b1f2b3ef0d6c1ad3d5ca5d63', 'file': 'ppu_recomp_005.cpp'},
    '00E5A6C0': {'sha256': 'b180274575e2801b90710691bc05f58818b53f87fc1c564c8001f69b4f266828', 'file': 'ppu_recomp_005.cpp'},
    '00D7EBE0': {'sha256': '45ffb1f9951909a245098c5df192832f2b7c067de56b5182666108c65c65099a', 'file': 'ppu_recomp_005.cpp'},
    '00D7EBE8': {'sha256': '4e40753f2af7a51c26f9578efa109b5e2e084e253fd665c0183c35a529df1790', 'file': 'ppu_recomp_005.cpp'},
    '00E59B90': {'sha256': '906968436cbd35feb24168a0f4ebe95aea87efb157a8237ab11178efc65fe48d', 'file': 'ppu_recomp_005.cpp'},
}


def patches(name):
    entry = common.signature(name)
    result = [(entry, entry + common.guarded(GUARD, f'        {SCOPES[name]} mag_render_ui_scope(ctx);'))]
    if name == '00982504':
        result += [(WORD_DATA_STORE, common.guarded(GUARD, '        mag_render_ui_scope.before_header(ctx);') + WORD_DATA_STORE),
                   (WORD_HEADER_STORE, WORD_HEADER_STORE + common.guarded(GUARD, '        mag_render_ui_scope.after_header(ctx);'))]
    if name == '009835B4':
        result.append((SCISSOR_FINAL_STORE, SCISSOR_FINAL_STORE + common.guarded(GUARD, '        mag_render_ui_scope.after_packet(ctx);')))
    return result


def strip_body(body, name):
    return common.remove_patches(body, patches(name))


def instrument_body(body, name):
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
