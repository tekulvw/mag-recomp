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
    '00D12B70': {'sha256': '1b6c5e788155d158c3f3b82c23344e67846706135b374412a71ed02bc478e9b5', 'file': 'ppu_recomp_005.cpp'},
}
# The helper's caller and the getters it reads through; verified, never changed.
DEPENDENCIES = {
    '00D1573C': {'sha256': 'c0d81999a3823a1a420bc7b403eb92a23917411495eb8b01821967d21d798ccd', 'file': 'ppu_recomp_005.cpp'},
    '00E599D0': {'sha256': '18b748637f2dd0bf3ebed8723c6a360c589e09051386be69b204554a6f6c9934', 'file': 'ppu_recomp_005.cpp'},
    '00E6E608': {'sha256': '448c970d24d4b0f22b27c521bda1174077eec0c00e5b3aab26ff739f91bf1191', 'file': 'ppu_recomp_006.cpp'},
}


def patches(name):
    entry = common.signature(name)
    return [(entry, entry + common.guarded(GUARD, '        MagRenderShadowCornerScope mag_render_shadow_corner_scope(ctx);'))]


def strip_body(body, name):
    return common.remove_patches(body, patches(name))


def instrument_body(body, name):
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
