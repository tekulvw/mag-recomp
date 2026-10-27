#pragma once
#include <cstdint>

// MAG v1.0 launcher init unconditionally re-enables tickets after Lua settings.
// Local-service mode restores the configured false value, without changing NP
// status or a connection state. Callers also verify the full original ELF hash.
inline int mag_restore_local_ticket(bool enabled, uint32_t load_instruction,
                                    uint32_t store_instruction, uint8_t configured,
                                    uint8_t& effective) {
    if (!enabled) return 0;
    if (load_instruction != 0x38000001 || store_instruction != 0x98092280 || configured != 0)
        return -1;
    if (effective == 0) return 0;
    effective = 0;
    return 1;
}

// Supply only the explicit local Player/PlayerRetail profiles, using the original setter.
// Never replace a nonempty identity or bypass a locked/ticket-enabled config.
inline int mag_local_account_policy(bool enabled, uint32_t getter_instruction,
                                    uint8_t tickets, uint8_t locked,
                                    uint32_t length, uint32_t capacity,
                                    bool matches_selected) {
    if (!enabled) return 0;
    if (getter_instruction != 0x3863004c || tickets || locked || capacity != 15)
        return -1;
    if ((length == 6 || length == 12) && matches_selected) return 0;
    return length == 0 ? 1 : -1;
}
