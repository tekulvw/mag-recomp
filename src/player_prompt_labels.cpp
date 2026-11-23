#include "player_prompt_labels.h"
#include <array>
#include <string_view>

namespace mag {
namespace {
// Keys whose printed character depends on the keyboard layout: letters, the digit row and the
// punctuation keys. Every other key keeps a fixed short label on every layout.
bool layout_key(uint32_t usage) {
    return (usage >= 4 && usage <= 39) || (usage >= 45 && usage <= 56) || usage == 100;
}
const char* fixed_label(uint32_t usage) {
    switch (usage) {
    case 40: return "ENTER"; case 41: return "ESC"; case 42: return "BKSP"; case 43: return "TAB";
    case 44: return "SPACE"; case 45: return "-"; case 46: return "="; case 47: return "[";
    case 48: return "]"; case 49: return "\\"; case 50: return "#"; case 51: return ";";
    case 52: return "'"; case 53: return "`"; case 54: return ","; case 55: return ".";
    case 56: return "/"; case 57: return "CAPS"; case 70: return "PRTSC"; case 71: return "SCRLK";
    case 72: return "PAUSE"; case 73: return "INS"; case 74: return "HOME"; case 75: return "PGUP";
    case 76: return "DEL"; case 77: return "END"; case 78: return "PGDN"; case 79: return "RIGHT";
    case 80: return "LEFT"; case 81: return "DOWN"; case 82: return "UP"; case 83: return "NUMLK";
    case 84: return "NUM/"; case 85: return "NUM*"; case 86: return "NUM-"; case 87: return "NUM+";
    case 88: return "NUMENT"; case 98: return "NUM0"; case 99: return "NUM."; case 100: return "\\";
    case 224: return "CTRL"; case 225: return "SHIFT"; case 226: return "ALT";
    case 228: return "RCTRL"; case 229: return "RSHIFT"; case 230: return "RALT";
    default: return nullptr;
    }
}
}  // namespace

std::string mag_prompt_fallback_label(PhysicalControl control) {
    if (control.source == InputSource::Mouse) {
        static constexpr std::array<const char*, 5> names{"LMB", "RMB", "MMB", "M4", "M5"};
        return control.code < names.size() ? names[control.code] : "M" + std::to_string(control.code + 1);
    }
    if (control.source != InputSource::Keyboard) return {};
    const uint32_t usage = control.code;
    if (usage >= 4 && usage <= 29) return std::string(1, char('A' + usage - 4));
    if (usage >= 30 && usage <= 38) return std::string(1, char('1' + usage - 30));
    if (usage == 39) return "0";
    if (usage >= 58 && usage <= 69) return "F" + std::to_string(usage - 57);
    if (usage >= 89 && usage <= 97) return "NUM" + std::to_string(usage - 88);
    if (const char* label = fixed_label(usage)) return label;
    return "KEY" + std::to_string(usage);
}

std::string mag_prompt_normalize_label(std::string_view name) {
    std::string label;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        // Only printable ASCII: the prompt text path is not proven to carry other characters.
        if (byte < 0x21 || byte > 0x7E) return {};
        label.push_back(byte >= 'a' && byte <= 'z' ? char(byte - 'a' + 'A') : c);
    }
    return label.size() <= 8 ? label : std::string{};
}

std::string mag_prompt_label(PhysicalControl control) {
    if (control.source == InputSource::Keyboard && layout_key(control.code)) {
        std::array<char, 64> name{};
        const size_t length = mag_prompt_host_key_name(control.code, name.data(), name.size() - 1);
        if (length && length < name.size()) {
            auto label = mag_prompt_normalize_label({name.data(), length});
            // The digit row prints symbols unshifted on some layouts (AZERTY "&"); its keycaps
            // also carry the digit, which reads better in a prompt.
            const bool digit_row = control.code >= 30 && control.code <= 39;
            const bool alnum = label.size() == 1 && ((label[0] >= '0' && label[0] <= '9') || (label[0] >= 'A' && label[0] <= 'Z'));
            if (!label.empty() && (!digit_row || alnum)) return label;
        }
    }
    return mag_prompt_fallback_label(control);
}
}  // namespace mag
