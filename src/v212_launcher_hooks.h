#pragma once
#include "retail_network_policy.h"

// 2.12-specific identity guard: the effective flag moved from the v1 layout.
// Restore only a false configuration explicitly selected by the supervisor.
inline int mag_v212_restore_local_ticket(bool enabled, uint32_t load_instruction,
                                         uint32_t store_instruction, uint8_t configured,
                                         uint8_t& effective) {
    if (!enabled) return 0;
    if (load_instruction != 0x38000001 || store_instruction != 0x98092289 || configured != 0)
        return -1;
    if (!effective) return 0;
    effective=0;
    return 1;
}

extern "C" void mag_v212_launcher_register(void);

// Call only for the exact EBOOT identity in retail-v2.12-launcher.json.
extern "C" void mag_v212_launcher_register();
