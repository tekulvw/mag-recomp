"""Shadow view stage: the helper that derives one Shadow view's corner vectors.

One scope at the helper's entry reads the helper's guest inputs and asks the
adapter for its own result; when the helper returns, the scope compares that
result with what the original wrote. It never writes guest memory, so the
original result stays authoritative. Guard: MAG_RENDER_ADAPTER_SHADOW_VIEW,
which also requires the configuration stage.
"""
from tools.render_hooks import common

NAME = 'shadow-view'
GUARD = ('defined(MAG_RENDER_ADAPTER_CONFIGURATION) && MAG_RENDER_ADAPTER_CONFIGURATION'
         ' && defined(MAG_RENDER_ADAPTER_SHADOW_VIEW) && MAG_RENDER_ADAPTER_SHADOW_VIEW')
INCLUDE = '#if ' + GUARD + '\n#include "mag_render_shadow_view_hook.h"\n#endif\n'
PINS = {
    '00D12B70': {'sha256': '27a1ab7d4b491523c34809f1b3d8b69513ea8b597bcdeb6b5e5cf6c272a463d2', 'file': 'ppu_recomp_005.cpp'},
}
# The helper's caller and the getters it reads through; verified, never changed.
DEPENDENCIES = {
    '00D1573C': {'sha256': '730559929f56d78e11ef2a6e309bb312bbbd02551cc651ecb3aa3907e3058ab7', 'file': 'ppu_recomp_005.cpp'},
    '00E599D0': {'sha256': '18b748637f2dd0bf3ebed8723c6a360c589e09051386be69b204554a6f6c9934', 'file': 'ppu_recomp_005.cpp'},
    '00E6E608': {'sha256': '0a5953427e7bf6f995d5eb394f1822892ec6809b57abc5a9b1f52f9e67c50956', 'file': 'ppu_recomp_006.cpp'},
}


def patches(name):
    entry = common.signature(name)
    return [(entry, entry + common.guarded(GUARD, '        MagRenderShadowCornerScope mag_render_shadow_corner_scope(ctx);'))]


def strip_body(body, name):
    return common.remove_patches(body, patches(name))


def instrument_body(body, name):
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
