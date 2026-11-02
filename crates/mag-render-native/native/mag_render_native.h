/* Extracted from ps3recomp libs/video/rsx_frontend.h at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#ifndef MAG_RENDER_NATIVE_H
#define MAG_RENDER_NATIVE_H

#include <stdint.h>
#include "mag_render_host_owner.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in semantic compute interface. Borrowed device lifetime ends at backend
 * shutdown. Serialize calls; descriptors/host bytes are consumed before return.
 * No guest packets, window/present work, or implicit original-work fallback. */
#define MAG_NATIVE_ABI_VERSION 1u
#define MAG_NATIVE_MAX_BINDINGS 16u
#define MAG_NATIVE_NO_SIZE_INDEX UINT32_MAX
typedef struct mag_native_device mag_native_device;
/* New context owns native resources but borrows exactly the supplied owner. */
int mag_native_create(const MagRenderHostOwner *,mag_native_device **);
/* BUSY preserves context/owner; no implicit GPU wait or fabricated retirement. */
int mag_native_close(mag_native_device *);
enum mag_native_status {
    MAG_NATIVE_OK = 0,
    MAG_NATIVE_INDETERMINATE = 1, /* ticket retained; poll for real terminal */
    MAG_NATIVE_INVALID = -1,
    MAG_NATIVE_UNSUPPORTED = -2,
    MAG_NATIVE_STALE = -3,
    MAG_NATIVE_BUSY = -4,
    MAG_NATIVE_ALLOCATION_FAILED = -5,
    MAG_NATIVE_DEVICE_FAILED = -6
};
enum mag_native_access { MAG_NATIVE_READ = 1, MAG_NATIVE_READ_WRITE = 2 };
enum mag_native_binding_kind { MAG_NATIVE_STORAGE = 0, MAG_NATIVE_UNIFORM = 1 };
enum mag_native_shader_language {
    MAG_NATIVE_MSL = 1, MAG_NATIVE_HLSL = 2, MAG_NATIVE_SPIRV = 3
};
typedef struct mag_native_buffer { uint64_t device_generation, id, version; } mag_native_buffer;
typedef struct mag_native_pipeline { uint64_t device_generation, id; } mag_native_pipeline;
typedef struct mag_native_ticket { uint64_t device_generation, submission_id; } mag_native_ticket;

typedef struct mag_native_caps_desc {
    uint32_t struct_size, abi_version;
    uint64_t device_generation;
    uint32_t compute_supported, draws_supported, max_pending, max_bindings;
    uint32_t runtime_sizes_slot, reserved;
} mag_native_caps_desc;

typedef struct mag_native_buffer_desc {
    uint32_t struct_size, abi_version;
    uint64_t byte_length;
    const void *initial_bytes; /* NULL means zero initialize */
} mag_native_buffer_desc;

/* CPU publication never overwrites a prior allocation/version. update publishes
 * complete CPU bytes into a new native buffer without reading/mutating the prior
 * in-flight version. Explicit READ_WRITE GPU bindings can mutate that native
 * version; its post-dispatch bytes must not be mistaken for the CPU seed.
 * Each version remains addressable until explicitly released. Submission retains
 * its own references through GPU terminal completion even after public release. */
int mag_native_buffer_create(mag_native_device *, const mag_native_buffer_desc *, mag_native_buffer *);
int mag_native_buffer_update(mag_native_device *, mag_native_buffer previous,
    const mag_native_buffer_desc *, mag_native_buffer *);
int mag_native_buffer_release(mag_native_device *, mag_native_buffer);
/* Diagnostic/readback only; returns BUSY until all uses are terminal and polled. */
int mag_native_buffer_read(mag_native_device *, mag_native_buffer, uint64_t offset,
    void *destination, uint64_t length);

typedef struct mag_native_binding_layout {
    uint32_t slot, access;
    uint64_t minimum_bytes;
    uint32_t runtime_array_stride; /* zero for fixed layouts */
    uint32_t runtime_size_index; /* dense metadata indices or NO_SIZE_INDEX */
    uint32_t alignment_bytes, binding_kind; /* uniform requires READ */
} mag_native_binding_layout;
/* Initial native compute accepts runtime arrays starting at byte zero only:
 * minimum_bytes must equal runtime_array_stride. Struct headers before runtime
 * arrays return UNSUPPORTED; their translation metadata is not native support. */

typedef struct mag_native_pipeline_desc {
    uint32_t struct_size, abi_version;
    const void *source_bytes;
    uint64_t source_length;
    const char *entry_point;
    uint32_t entry_point_length, binding_count;
    const mag_native_binding_layout *bindings;
    uint32_t threads_per_group[3], shader_language;
} mag_native_pipeline_desc;
int mag_native_pipeline_create(mag_native_device *, const mag_native_pipeline_desc *, mag_native_pipeline *);
int mag_native_pipeline_release(mag_native_device *, mag_native_pipeline);

typedef struct mag_native_binding {
    uint32_t slot, reserved;
    mag_native_buffer buffer;
    uint64_t offset, byte_length;
} mag_native_binding;
typedef struct mag_native_frame {
    uint32_t struct_size, abi_version;
    mag_native_pipeline pipeline;
    const mag_native_binding *bindings;
    uint32_t binding_count, groups[3];
} mag_native_frame;

/* Negative returns guarantee no commit. OK/INDETERMINATE provide a qualified
 * ticket; only a genuine terminal event authorizes resource reclamation.
 * Existing-backend uncommitted work makes submit BUSY: callers must explicitly
 * submit that work first. Shared queue commit order then preserves dependencies;
 * this API never flushes/replays original work or creates a second queue. */
int mag_native_submit(mag_native_device *, const mag_native_frame *, mag_native_ticket *);
typedef struct mag_native_event {
    uint32_t struct_size, abi_version;
    mag_native_ticket ticket;
    uint32_t succeeded, no_further_gpu_access;
    uint32_t native_status, reserved;
} mag_native_event;
/* count=0 means no event, never completion. Each terminal event is emitted once.
 * API errors do not certify terminal state and do not release pending leases. */
int mag_native_poll(mag_native_device *, mag_native_event *, uint32_t capacity, uint32_t *count);
int mag_native_caps(mag_native_device *, mag_native_caps_desc *);

#ifdef __cplusplus
}
#endif
#endif
