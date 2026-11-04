"""Configuration lifetime stage: the Shadow configuration manager's init and shutdown.

Each of the two pinned functions gets one scope object at entry. The init scope
observes the manager's state and parameters on entry and again when the
function returns; the shutdown scope reports the end of that configuration
before the original shutdown runs. Original calls, drains, writes and returns
are unchanged. Guard: MAG_RENDER_ADAPTER_CONFIGURATION.
"""
from tools.render_hooks import common

NAME = 'configuration'
GUARD = 'defined(MAG_RENDER_ADAPTER_CONFIGURATION) && MAG_RENDER_ADAPTER_CONFIGURATION'
INCLUDE = '#if ' + GUARD + '\n#include "mag_render_frontend_bridge.h"\n#endif\n'
SCOPES = {'00E8155C': 'MagRenderShadowConfigInitScope',
          '00E813E8': 'MagRenderShadowConfigShutdownScope'}
PINS = {
    '00E8155C': {'sha256': '38623a9ad0bdcba96abe3a9b657b22b51a78c58cdcdc5d2a68f39510ae2e98be', 'file': 'ppu_recomp_006.cpp'},
    '00E813E8': {'sha256': '7942bb7ee6a1e3b1e249ce8c5fb7fdbb1515c6c4f2eda31c464318fbad7dbfa2', 'file': 'ppu_recomp_006.cpp'},
}
# Callees whose behaviour the scopes rely on; verified, never changed.
DEPENDENCIES = {
    '00D145C8': {'sha256': '867f5c02535811ef2f6f3ca667493bbcdb1e9f6b683417319c3b4578fff37b75', 'file': 'ppu_recomp_010.cpp'},
    '01524AC0': {'sha256': '7d3ca1c50964fa99677963b110c0ac88f410a801e7bfb3c4ea33c1d0f17241f4', 'file': 'ppu_recomp_008.cpp'},
    '00D13F34': {'sha256': '7bc29cf3697500c82c257f1ba13b6b67ea368b7803a8bd7f5a9a6bb037713afa', 'file': 'ppu_recomp_005.cpp'},
    '01524AA0': {'sha256': 'b1348f9f095b16a3c10aab66c7337ed755cc56845bbf2df09c2c25d2605507d4', 'file': 'ppu_recomp_008.cpp'},
}


def patches(name):
    entry = common.signature(name)
    return [(entry, entry + common.guarded(GUARD, f'        {SCOPES[name]} mag_render_config_scope(ctx);'))]


def strip_body(body, name):
    return common.remove_patches(body, patches(name))


def instrument_body(body, name):
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
