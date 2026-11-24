#pragma once
/* Key cap images for button prompts that a game movie draws as a picture (docs/native-controls.md,
 * section 12.5). The picture is our own: a rounded key outline with the key's label in a 5x7 pixel
 * font authored for this file, white on transparent, rendered at any size with box-filtered
 * edges. Encoders turn it into the block-compressed or plain layouts textures use. Nothing here
 * reads or derives from game data. */
#include <cstdint>
#include <string_view>
#include <vector>

struct MagKeycapImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> alpha;  // one coverage byte per pixel, row-major; the colour is white
};

// The key cap for a label (upper-case ASCII letters, digits, a few symbols; the arrows are the
// labels "UP", "DOWN", "LEFT" and "RIGHT"). Unknown characters draw as a small box. Returns an
// empty image for a zero size.
MagKeycapImage mag_prompt_keycap(std::string_view label, uint32_t width, uint32_t height);

// Encodings. Block formats need width and height that are multiples of 4; otherwise they return
// an empty vector. All are little-endian as stored in textures and DDS files.
std::vector<uint8_t> mag_prompt_keycap_bgra(const MagKeycapImage&);  // B, G, R, A per pixel
std::vector<uint8_t> mag_prompt_keycap_dxt1(const MagKeycapImage&);  // 1-bit alpha (cut at half)
std::vector<uint8_t> mag_prompt_keycap_dxt5(const MagKeycapImage&);  // interpolated alpha
