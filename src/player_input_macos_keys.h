#pragma once
/* macOS virtual key codes (the kVK_ constants of Carbon's Events.h, which name key positions on
 * the ANSI layout) by USB HID keyboard usage, for reading a key's physical state with
 * CGEventSourceKeyState. Portable so the table is checked on any host. -1: no such key here, or a
 * key capture never reports (the GUI keys). */
#include <cstdint>

constexpr int mag_macos_key_code(uint32_t usage) {
    constexpr uint8_t letters[26] = {0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                     0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06};
    constexpr uint8_t digits[10] = {0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D};  // 1..9, 0
    constexpr uint8_t functions[12] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};
    constexpr uint8_t keypad[10] = {0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C, 0x52};  // 1..9, 0
    constexpr uint8_t high_functions[8] = {0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A};       // F13..F20
    if (usage >= 4 && usage <= 29) return letters[usage - 4];
    if (usage >= 30 && usage <= 39) return digits[usage - 30];
    if (usage >= 58 && usage <= 69) return functions[usage - 58];
    if (usage >= 89 && usage <= 98) return keypad[usage - 89];
    if (usage >= 104 && usage <= 111) return high_functions[usage - 104];
    switch (usage) {
    case 40: return 0x24;   // Return
    case 41: return 0x35;   // Escape
    case 42: return 0x33;   // Backspace (Delete)
    case 43: return 0x30;   // Tab
    case 44: return 0x31;   // Space
    case 45: return 0x1B; case 46: return 0x18; case 47: return 0x21; case 48: return 0x1E;  // - = [ ]
    case 49: return 0x2A; case 50: return 0x2A; case 51: return 0x29; case 52: return 0x27;  // \ # ; '
    case 53: return 0x32; case 54: return 0x2B; case 55: return 0x2F; case 56: return 0x2C;  // ` , . /
    case 57: return 0x39;   // Caps Lock
    case 73: return 0x72;   // Insert (Help)
    case 74: return 0x73;   // Home
    case 75: return 0x74;   // Page Up
    case 76: return 0x75;   // Delete (forward)
    case 77: return 0x77;   // End
    case 78: return 0x79;   // Page Down
    case 79: return 0x7C;   // Right
    case 80: return 0x7B;   // Left
    case 81: return 0x7D;   // Down
    case 82: return 0x7E;   // Up
    case 83: return 0x47;   // Keypad Clear (Num Lock)
    case 84: return 0x4B;   // Keypad /
    case 85: return 0x43;   // Keypad *
    case 86: return 0x4E;   // Keypad -
    case 87: return 0x45;   // Keypad +
    case 88: return 0x4C;   // Keypad Enter
    case 99: return 0x41;   // Keypad .
    case 100: return 0x0A;  // ISO key left of Z
    case 103: return 0x51;  // Keypad =
    case 224: return 0x3B;  // Left Control
    case 225: return 0x38;  // Left Shift
    case 226: return 0x3A;  // Left Option
    case 228: return 0x3E;  // Right Control
    case 229: return 0x3C;  // Right Shift
    case 230: return 0x3D;  // Right Option
    default: return -1;
    }
}
