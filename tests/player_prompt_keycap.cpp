// Key cap images and their texture encodings; synthetic only.
#include "player_prompt_keycap.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

namespace {
// Reference decoders (written from the public block-compression format).
std::array<uint8_t, 16> dxt5_alpha(const uint8_t* b) {
    std::array<int, 8> level{b[0], b[1]};
    if (b[0] > b[1]) for (int k = 1; k <= 6; ++k) level[k + 1] = ((7 - k) * b[0] + k * b[1]) / 7;
    else { for (int k = 1; k <= 4; ++k) level[k + 1] = ((5 - k) * b[0] + k * b[1]) / 5; level[6] = 0; level[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 16; ++i) out[i] = uint8_t(level[(bits >> (3 * i)) & 7]);
    return out;
}
std::array<uint8_t, 16> dxt1_alpha(const uint8_t* b) {
    const unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    const uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 16; ++i) out[i] = (c0 <= c1 && ((idx >> (2 * i)) & 3) == 3) ? 0 : 255;
    return out;
}
uint8_t at(const MagKeycapImage& im, uint32_t x, uint32_t y) { return im.alpha[size_t(y) * im.width + x]; }

void render() {
    const auto x = mag_prompt_keycap("X", 24, 24);
    require(x.width == 24 && x.height == 24 && x.alpha.size() == 576, "size");
    require(at(x, 0, 0) == 0 && at(x, 23, 23) == 0, "corners outside the rounded key are clear");
    unsigned border = 0, centre = 0;
    for (uint32_t i = 0; i < 24; ++i) border += at(x, i, 1) > 0 || at(x, i, 2) > 0;
    require(border >= 12, "a top border stroke");
    for (uint32_t y = 8; y < 16; ++y) for (uint32_t xx = 8; xx < 16; ++xx) centre += at(x, xx, y) > 127;
    require(centre >= 6, "the glyph covers the middle");
    // The X glyph is symmetric left/right.
    unsigned asym = 0;
    for (uint32_t y = 0; y < 24; ++y) for (uint32_t xx = 0; xx < 12; ++xx) asym += std::abs(int(at(x, xx, y)) - int(at(x, 23 - xx, y))) > 64;
    require(asym <= 8, "X is mirror symmetric");
    require(mag_prompt_keycap("X", 0, 24).alpha.empty(), "zero size");
    require(mag_prompt_keycap("e", 32, 32).alpha == mag_prompt_keycap("E", 32, 32).alpha, "labels are upper-cased");
    require(mag_prompt_keycap("ENTER", 32, 32).alpha == mag_prompt_keycap("ENT", 32, 32).alpha, "ENTER uses its short form");
    require(mag_prompt_keycap("UP", 32, 32).alpha != mag_prompt_keycap("DOWN", 32, 32).alpha, "arrow glyphs");
    require(mag_prompt_keycap("X", 32, 32).alpha != mag_prompt_keycap("Z", 32, 32).alpha, "glyphs differ");
    const auto wide = mag_prompt_keycap("SPC", 48, 24);
    require(wide.width == 48 && wide.alpha.size() == 48 * 24, "non-square");
}

void encode() {
    const auto im = mag_prompt_keycap("C", 32, 16);
    const auto bgra = mag_prompt_keycap_bgra(im);
    require(bgra.size() == 32 * 16 * 4, "bgra size");
    for (size_t i = 0; i < im.alpha.size(); ++i)
        require(bgra[4 * i] == 255 && bgra[4 * i + 1] == 255 && bgra[4 * i + 2] == 255 && bgra[4 * i + 3] == im.alpha[i], "bgra pixel");
    const auto d5 = mag_prompt_keycap_dxt5(im), d1 = mag_prompt_keycap_dxt1(im);
    require(d5.size() == (32 / 4) * (16 / 4) * 16 && d1.size() == (32 / 4) * (16 / 4) * 8, "block sizes");
    for (uint32_t by = 0; by < 4; ++by)
        for (uint32_t bx = 0; bx < 8; ++bx) {
            const size_t block = by * 8 + bx;
            const auto a5 = dxt5_alpha(&d5[block * 16]), a1 = dxt1_alpha(&d1[block * 8]);
            for (int i = 0; i < 16; ++i) {
                const int want = at(im, bx * 4 + i % 4, by * 4 + i / 4);
                require(std::abs(int(a5[i]) - want) <= 20, "dxt5 alpha within one interpolation step");
                require(a1[i] == (want >= 128 ? 255 : 0), "dxt1 cuts alpha at half");
            }
            require(d5[block * 16 + 8] == 0xFF && d5[block * 16 + 9] == 0xFF, "dxt5 colour is white");
        }
    MagKeycapImage odd = mag_prompt_keycap("X", 30, 30);
    require(mag_prompt_keycap_dxt5(odd).empty() && mag_prompt_keycap_dxt1(odd).empty(), "blocks need multiples of 4");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string which = argc > 1 ? argv[1] : "";
        if (which == "render") render();
        else if (which == "encode") encode();
        else throw std::runtime_error("unknown test");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
