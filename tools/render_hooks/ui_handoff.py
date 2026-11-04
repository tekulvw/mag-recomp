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
    '0024EDF0': {'sha256': '8652d47778f2fb02d509c89e5a3b5be7037e0bc66f44f8a415ea9bc8cbccdd16', 'file': 'ppu_recomp_000.cpp'},
    '00982504': {'sha256': '0f734d21003d1e4b860bf88e9e600d52d26ba63cb0fef00462e04caed5bfc12a', 'file': 'ppu_recomp_004.cpp'},
    '00D084E8': {'sha256': 'd8cb244639d660d9b85338ff90c382585632f3969f1f5a460694a538092c5d54', 'file': 'ppu_recomp_005.cpp'},
    '009825B0': {'sha256': '5c744f3d671481b9f4eee29f151718191753c48f2255896cd2adc936e9de6af3', 'file': 'ppu_recomp_004.cpp'},
    '0098368C': {'sha256': '2c7638717d647c50722bce0a1af83eba1b3949c0f5966c777f84e6dfe1a312d1', 'file': 'ppu_recomp_004.cpp'},
    '009835B4': {'sha256': '3066238d4ce2e7e3cc376349a502bbf3efac60a83e559bf3a7805cda5aa46c95', 'file': 'ppu_recomp_004.cpp'},
}
# Callees the writers reach before their final stores; verified, never changed.
DEPENDENCIES = {
    '00E4E9C0': {'sha256': '2b62665798556d09942fab0d3610c8cbd8b0073c45e2f6386ad829ab20aad4d1', 'file': 'ppu_recomp_005.cpp'},
    '00E59B80': {'sha256': 'c320c2dfab8234d8c4d4784e39c8d40485b7ece3b1f2b3ef0d6c1ad3d5ca5d63', 'file': 'ppu_recomp_005.cpp'},
    '00E5A6C0': {'sha256': 'b180274575e2801b90710691bc05f58818b53f87fc1c564c8001f69b4f266828', 'file': 'ppu_recomp_005.cpp'},
    '00D7EBE0': {'sha256': '9e8a4ba007135b357da1446fc44b36cec7e6980d01a4c499ad396941802f8ae5', 'file': 'ppu_recomp_005.cpp'},
    '00D7EBE8': {'sha256': 'e28e8ab53d29312f84af242ce5c1b707cf50fb82f2baa6fb3ef22a1e361f36d3', 'file': 'ppu_recomp_005.cpp'},
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
