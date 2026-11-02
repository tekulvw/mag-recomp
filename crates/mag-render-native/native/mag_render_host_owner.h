#ifndef MAG_RENDER_HOST_OWNER_H
#define MAG_RENDER_HOST_OWNER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_RENDER_HOST_OWNER_ABI 1u
enum MagRenderHostBackend { MAG_RENDER_HOST_METAL=1, MAG_RENDER_HOST_VULKAN=2, MAG_RENDER_HOST_D3D12=3 };
enum MagRenderHostStatus { MAG_RENDER_HOST_OK=0, MAG_RENDER_HOST_INVALID=-1,
    MAG_RENDER_HOST_UNSUPPORTED=-2, MAG_RENDER_HOST_STALE=-3, MAG_RENDER_HOST_BUSY=-4,
    MAG_RENDER_HOST_ALLOCATION_FAILED=-5, MAG_RENDER_HOST_DEVICE_FAILED=-6 };
typedef void (*MagRenderProc)(void);
/* Provider-neutral, borrowed owner: SDK provider now, Rust provider later.
 * Getter metadata is unretained. Callback userdata must remain process-lived;
 * retain must succeed before ANY native device/queue dereference. Generation
 * mismatch or closing returns STALE. Retain pins both provider state and native
 * owner lifetime; teardown/reinit must refuse while a consumer remains pinned.
 *
 * try_begin is nonblocking and exclusive. It rejects uncommitted original work,
 * concurrent encoding/presentation, stale lifetime and actual owner failure.
 * Success holds all queue/execution serialization through encoding and native
 * commit+signal; end releases it on the same thread. Callback lock order:
 * consumer lease -> owner gate -> existing native/legacy locks. Callbacks never
 * acquire a consumer lease or wait for GPU work. Native code must not acquire
 * those same provider queue locks again while this lease is held.
 *
 * release follows genuine drain only; errors never imply terminal access.
 * Immutable metadata; serialize calls and preserve constructor thread affinity.
 * Device ownership does not itself specify guest/UI/final-present ordering:
 * frame coordinator and target leases provide that separate explicit contract.
 */
typedef struct MagRenderHostOwner {
    uint32_t abi_version,struct_size,backend,reserved;
    uint64_t lifetime;
    void *device,*queue,*userdata;
    uint32_t queue_family,reserved1;
    void *instance,*physical_device; /* Vulkan only; otherwise NULL. */
    MagRenderProc (*get_instance_proc)(void *,const char *);
    MagRenderProc (*get_device_proc)(void *,const char *);
    int (*retain)(void *,uint64_t);
    void (*release)(void *,uint64_t);
    int (*try_begin)(void *,uint64_t);
    void (*end)(void *,uint64_t);
} MagRenderHostOwner;
/* Optional enabled-device facts are captured with the same owner generation.
 * Known physical support alone is not an enabled feature. Old ABI1 owners
 * carry no such facts and keep conservative feature admission. */
#define MAG_RENDER_HOST_FEATURE_BC UINT64_C(1)
#define MAG_RENDER_HOST_FEATURE_ANISOTROPY UINT64_C(2)
#define MAG_RENDER_HOST_FEATURE_DEPTH_BOUNDS UINT64_C(4)
#define MAG_RENDER_HOST_FEATURE_ALL UINT64_C(7)
typedef struct MagRenderHostOwnerV2 {
    uint32_t abi_version,struct_size;
    MagRenderHostOwner base;
    uint64_t known_features,enabled_features;
} MagRenderHostOwnerV2;

#ifdef __cplusplus
}
#endif
#endif
