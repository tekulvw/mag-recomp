#pragma once
#include <cstddef>
#include <cstdint>
// Guest ranges the adapter may touch: the low image window and the user allocation pool that
// the pinned runtime places in [0x10000000, 0x20000000). Codec, graphics, SPU and reserved high
// windows are excluded. Works for both zero-based and offset guest mappings.
inline bool mag_game_controls_memory_range(bool ready, uint64_t vm_end, uint32_t address, size_t size) {
    const uint64_t end = vm_end && vm_end < 0x20000000ull ? vm_end : 0x20000000ull;
    return ready && size && address >= 0x10000u && address < end && size <= end - address;
}
