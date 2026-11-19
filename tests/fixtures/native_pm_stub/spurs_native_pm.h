/* Stand-in for the runtime's spurs_native_pm.h (ps3recomp runtime/spu), used by the shim fixture
 * only when the SDK checkout predates native SPURS policy modules: the payload-hook setter the
 * shim calls, declared as the runtime declares it. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;
typedef int (*spurs_native_pm_payload_fn)(struct spu_context*, uint32_t entry);
void spurs_native_pm_set_payload_hook(spurs_native_pm_payload_fn);
#ifdef __cplusplus
}
#endif
