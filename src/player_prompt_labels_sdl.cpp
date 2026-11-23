/* SDL host key names for prompt labels (player_prompt_labels.h). SDL scancodes are the HID
 * usages; SDL maps them through the keyboard layout it tracks for the game window. */
#include "player_prompt_labels.h"
#include <SDL.h>
#include <cstring>

extern "C" size_t mag_prompt_host_key_name(uint32_t usage, char* out, size_t capacity) {
    if (!out || !capacity || usage < 4 || usage >= SDL_NUM_SCANCODES) return 0;
    const SDL_Keycode key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(usage));
    if (key == SDLK_UNKNOWN) return 0;
    const char* name = SDL_GetKeyName(key);
    const size_t length = name ? std::strlen(name) : 0;
    if (!length || length >= capacity) return 0;
    std::memcpy(out, name, length);
    return length;
}
