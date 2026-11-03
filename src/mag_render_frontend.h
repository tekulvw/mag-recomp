#ifndef MAG_RENDER_FRONTEND_H
#define MAG_RENDER_FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Contracts/lifetime skeleton only: no rendering or live removal is certified. */
#define MAG_RENDER_ABI_VERSION 1u
#define MAG_RENDER_OK 0u
#define MAG_RENDER_INVALID_VERSION 1u
#define MAG_RENDER_INVALID_SIZE 2u
#define MAG_RENDER_INVALID_HANDLE 3u
#define MAG_RENDER_WRONG_THREAD 4u
#define MAG_RENDER_STALE_GENERATION 5u
#define MAG_RENDER_INVALID_LENGTH 6u
#define MAG_RENDER_INTERNAL_ERROR 7u
#define MAG_RENDER_INVALID_CONFIG 8u

typedef uint64_t MagRenderHandle;

typedef struct MagRenderConfig {
    uint32_t abi_version;
    uint32_t struct_size; /* exactly sizeof(MagRenderConfig); zero reserved */
    uint64_t reserved;
} MagRenderConfig;

typedef struct MagRenderCreateResult {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t status;
    uint32_t reserved;
    MagRenderHandle handle; /* zero on failure; never a pointer */
    uint64_t callback_generation;
} MagRenderCreateResult;

uint32_t mag_render_abi_version(void);
MagRenderCreateResult mag_render_create(MagRenderConfig config);
/* Create, callbacks and destroy are thread-affine. Double destroy is an error.
 * Tokens are never reused, so old callbacks cannot address a later context.
 * Unwinding does not cross this ABI. No caller memory is retained. */
uint32_t mag_render_destroy(MagRenderHandle handle);
uint32_t mag_render_validate_callback(MagRenderHandle handle, uint64_t generation);
/* Arithmetic validation only; this does not accept or access a buffer. */
uint32_t mag_render_validate_span(uint64_t element_count, uint64_t element_size,
                                  uint64_t byte_length);

#ifdef __cplusplus
}
#endif
#endif
