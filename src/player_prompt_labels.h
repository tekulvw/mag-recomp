#pragma once
/* Short labels for keys and mouse buttons in in-game button prompts (docs/native-controls.md,
 * section 12). A label names the key as the player's current keyboard layout prints it: on an
 * AZERTY layout the key at the US "Q" position is labelled "A". The host part asks the
 * operating system (macOS UCKeyTranslate, Windows MapVirtualKeyEx and GetKeyNameText, SDL on
 * Linux); the portable part shortens, upper-cases and falls back to US names when the host has
 * no answer. Codes are USB HID keyboard usages (4..231) and mouse buttons 0..4. */
#include "player_controls.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace mag {
// The prompt label for a keyboard key or mouse button; empty for a controller control.
std::string mag_prompt_label(PhysicalControl);
// The portable label: US layout, no host query.
std::string mag_prompt_fallback_label(PhysicalControl);
// Turns a host key name into a prompt label: upper-case ASCII, at most 8 characters, printable
// characters only. Returns empty when the name cannot be shown (then the fallback applies).
std::string mag_prompt_normalize_label(std::string_view host_name);
}  // namespace mag

extern "C" {
/* Host keyboard-layout query, one implementation per platform (player_prompt_labels_*.{cpp,mm};
 * player_prompt_labels_none.cpp answers nothing). Writes the key's name as UTF-8 and returns
 * its length, or 0 when the host has no name for the HID usage. Safe to call from any thread. */
size_t mag_prompt_host_key_name(uint32_t hid_usage, char* out, size_t capacity);
}
