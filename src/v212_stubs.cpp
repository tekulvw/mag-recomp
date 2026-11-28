// The 2.12 branch starts from generic runtime integration and original modules.
// No v1 title addresses, observers or hooks are installed in this target.
#include "ppu_recomp.h"
#ifdef MAG_V212_LAUNCHER
#include "v212_launcher_hooks.h"
#include "retail_handoff.h"
#endif
#ifdef MAG_V212_ENGINE
#include "v212_engine_hooks.h"
#endif
#ifdef MAG_QUEUE_FIRMWARE
extern "C" void mag_load_queue_firmware(void);
#endif
#ifdef MAG_V212_SPU_PREPARED
extern "C" void mag_v212_spu_register_all(void);
#endif
#ifdef MAG_V212_PACE_PREPARED
extern "C" void mag_v212_pace_register(void);
#endif

extern "C" void ps3_load_prx_modules(void) {
#ifdef MAG_QUEUE_FIRMWARE
    mag_load_queue_firmware();
#endif
#ifdef MAG_V212_SPU_PREPARED
    mag_v212_spu_register_all();
#endif
#ifdef MAG_V212_PACE_PREPARED
    mag_v212_pace_register();
#endif
#ifdef MAG_V212_LAUNCHER
    mag_v212_launcher_register();
    mag_register_handoff();
#endif
#ifdef MAG_V212_ENGINE
    mag_v212_engine_register();
#endif
}
extern "C" void mag_queue_snapshot(void) {}
extern "C" void mag_job_marker_snapshot(void) {}
