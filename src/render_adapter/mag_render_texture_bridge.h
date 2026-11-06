#pragma once
/* Located guest reads for the cohort bridge: RSX local or main memory offsets
 * resolved page by page through the runtime. A synchronous borrowed read window;
 * neither the context nor its callbacks outlive the capture call that owns them. */
#include <cstdint>
using MagRenderTextureGuestCopy = int32_t (*)(void*, uint32_t, uint32_t, uint8_t*);
// Optional read-only view with the copy's checks; null when not supported.
using MagRenderTextureGuestView = const uint8_t* (*)(void*, uint32_t, uint32_t);
struct MagRenderTextureCopyContext {
    MagRenderTextureGuestCopy copy;
    void* opaque;
    MagRenderTextureGuestView view{};
};
// location 0 is LOCAL, 1 is MAIN. Every page is resolved and checked; an
// unmapped MAIN page never falls back to a guessed local or guest address.
extern "C" int32_t mag_render_texture_copy_located(void*, uint32_t location, uint32_t offset, uint32_t bytes,
                                                   uint8_t* out) noexcept;
// Read-only twin of the located copy: the same page resolution and checks, and a
// pointer only when the whole span is one contiguous guest range. Null means
// "copy instead". Valid only during the capture call that owns the context.
extern "C" const uint8_t* mag_render_texture_view_located(void*, uint32_t location, uint32_t offset,
                                                          uint32_t bytes) noexcept;
