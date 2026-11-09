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

// A player's local account name: 3 to 15 letters, digits, '-' or '_', starting with a letter.
// 15 is the capacity of the retail short string the original setter fills in place.
inline bool mag_local_account_name_valid(const char* name) {
    if (!name) return false;
    unsigned length = 0;
    for (const char* p = name; *p; ++p, ++length) {
        char c = *p;
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool digit = c >= '0' && c <= '9';
        if (length == 0 ? !letter : !(letter || digit || c == '-' || c == '_')) return false;
        if (length >= 15) return false;
    }
    return length >= 3;
}

// Supply the selected local account name, using the original setter. Never replace a
// different nonempty identity or bypass a locked/ticket-enabled config.
inline int mag_local_account_policy(bool enabled, uint32_t getter_instruction,
                                    uint8_t tickets, uint8_t locked,
                                    uint32_t length, uint32_t capacity,
                                    uint32_t selected_length, bool matches_selected) {
    if (!enabled) return 0;
    if (getter_instruction != 0x3863004c || tickets || locked || capacity != 15)
        return -1;
    if (selected_length < 3 || selected_length > capacity) return -1;
    if (length == selected_length && matches_selected) return 0;
    return length == 0 ? 1 : -1;
}
