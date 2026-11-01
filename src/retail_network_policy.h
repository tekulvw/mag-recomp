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

// The original inline string has capacity 15. This subset also satisfies the
// community server's explicit local-development identity policy.
inline bool mag_local_account_name_valid(const char* name) {
    if (!name) return false;
    unsigned length = 0;
    for (; name[length]; ++length) {
        const unsigned char c = (unsigned char)name[length];
        if (length >= 15 || !((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return length >= 3;
}

// Supply only an explicit, validated local account, using the original setter.
// Never replace a nonempty identity or bypass a locked/ticket-enabled config.
inline int mag_local_account_policy(bool enabled, uint32_t getter_instruction,
                                    uint8_t tickets, uint8_t locked,
                                    uint32_t length, uint32_t capacity,
                                    bool matches_selected) {
    if (!enabled) return 0;
    if (getter_instruction != 0x3863004c || tickets || locked || capacity != 15)
        return -1;
    if (length >= 3 && length <= 15 && matches_selected) return 0;
    return length == 0 ? 1 : -1;
}
