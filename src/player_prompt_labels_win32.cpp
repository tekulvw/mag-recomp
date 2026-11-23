/* Windows host key names for prompt labels (player_prompt_labels.h): the key's set-1 scan code,
 * named by GetKeyNameTextW, which follows the active keyboard layout. */
#include "player_prompt_labels.h"
#include <windows.h>

namespace {
// Set-1 scan codes of the layout-dependent keys, by HID usage. Zero: not one of them.
uint16_t scan_code(uint32_t usage) {
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (usage >= 4 && usage <= 29) return letters[usage - 4];
    if (usage >= 30 && usage <= 39) return uint16_t(0x02 + (usage - 30));  // 1..9 then 0
    switch (usage) {
    case 45: return 0x0C; case 46: return 0x0D; case 47: return 0x1A; case 48: return 0x1B;
    case 49: return 0x2B; case 50: return 0x2B; case 51: return 0x27; case 52: return 0x28;
    case 53: return 0x29; case 54: return 0x33; case 55: return 0x34; case 56: return 0x35;
    case 100: return 0x56;
    default: return 0;
    }
}
}  // namespace

extern "C" size_t mag_prompt_host_key_name(uint32_t usage, char* out, size_t capacity) {
    const uint16_t scan = scan_code(usage);
    if (!scan || !out || capacity < 2) return 0;
    wchar_t name[32]{};
    const int length = GetKeyNameTextW(LONG(scan) << 16, name, 32);
    if (length <= 0) return 0;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, length, out, int(capacity), nullptr, nullptr);
    return bytes > 0 ? size_t(bytes) : 0;
}
