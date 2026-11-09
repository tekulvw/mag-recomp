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
    '00E813E8': {'sha256': '2bc9a58140bacfaf2bf7808d381969ba62f953b05fef2f56920e8e80f6df5564', 'file': 'ppu_recomp_006.cpp'},
}
# Callees whose behaviour the scopes rely on; verified, never changed.
DEPENDENCIES = {
    '00D145C8': {'sha256': '6e4f99076a8a1fcae1d5c87d9a213a12d05addd83b533c592ff72bb4035235d3', 'file': 'ppu_recomp_010.cpp'},
    '01524AC0': {'sha256': '7d3ca1c50964fa99677963b110c0ac88f410a801e7bfb3c4ea33c1d0f17241f4', 'file': 'ppu_recomp_008.cpp'},
    '00D13F34': {'sha256': '456d795ec35c0327e54eb12a8cd7b1a839bc77de577e7072cf0e624c7a2ddd49', 'file': 'ppu_recomp_005.cpp'},
    '01524AA0': {'sha256': 'b1348f9f095b16a3c10aab66c7337ed755cc56845bbf2df09c2c25d2605507d4', 'file': 'ppu_recomp_008.cpp'},
}


def patches(name):
    entry = common.signature(name)
    return [(entry, entry + common.guarded(GUARD, f'        {SCOPES[name]} mag_render_config_scope(ctx);'))]


def strip_body(body, name):
    return common.remove_patches(body, patches(name))


def instrument_body(body, name):
    return common.apply_patches(body, patches(name), f'{NAME} {name}')
