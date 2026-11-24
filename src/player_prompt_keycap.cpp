#include "player_prompt_keycap.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace {
// 5x7 glyphs, one byte per row, bit 4 is the left column. Authored for this file.
struct Glyph { char c; std::array<uint8_t, 7> rows; };
constexpr Glyph font[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}}, {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}}, {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'\\', {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01}},
    {'`', {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}}, {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}}, {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    // Arrow keys (selected by the labels UP, DOWN, LEFT, RIGHT).
    {'\x01', {0x04, 0x0E, 0x15, 0x04, 0x04, 0x04, 0x04}}, {'\x02', {0x04, 0x04, 0x04, 0x04, 0x15, 0x0E, 0x04}},
    {'\x03', {0x00, 0x04, 0x08, 0x1F, 0x08, 0x04, 0x00}}, {'\x04', {0x00, 0x04, 0x02, 0x1F, 0x02, 0x04, 0x00}},
};
constexpr std::array<uint8_t, 7> unknown{0x00, 0x1F, 0x11, 0x11, 0x11, 0x1F, 0x00};

const std::array<uint8_t, 7>& glyph(char c) {
    for (const auto& g : font)
        if (g.c == c) return g.rows;
    return unknown;
}

std::string drawn(std::string_view label) {
    if (label == "UP") return "\x01";
    if (label == "DOWN") return "\x02";
    if (label == "LEFT") return "\x03";
    if (label == "RIGHT") return "\x04";
    // Long key names take their short forms on a key cap.
    if (label == "ENTER") return "ENT";
    if (label == "SPACE") return "SPC";
    if (label == "BKSP") return "BS";
    std::string text(label.substr(0, 8));
    for (char& c : text) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return text;
}

// Signed distance from (x, y) to the border of a rounded rectangle [x0,x1]x[y0,y1] with radius r:
// negative inside.
double rounded_box(double x, double y, double x0, double y0, double x1, double y1, double r) {
    const double cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
    const double dx = x - cx, dy = y - cy;
    const double outside = std::sqrt(dx * dx + dy * dy) - r;
    if (x >= x0 + r && x <= x1 - r) return std::max(y0 - y, y - y1);
    if (y >= y0 + r && y <= y1 - r) return std::max(x0 - x, x - x1);
    return outside;
}
}  // namespace

MagKeycapImage mag_prompt_keycap(std::string_view label, uint32_t width, uint32_t height) {
    MagKeycapImage image;
    if (!width || !height || width > 4096 || height > 4096) return image;
    image.width = width, image.height = height;
    image.alpha.assign(size_t(width) * height, 0);
    const std::string text = drawn(label);
    const double w = width, h = height, side = std::min(w, h);
    // The key outline: a rounded box inset half a stroke from the edge.
    const double stroke = std::max(1.0, side / 12.0), inset = stroke / 2 + side / 24.0;
    const double radius = side / 5.0;
    // The label: 5x7 cells with one empty column between glyphs, scaled to fit inside the outline.
    const double columns = text.empty() ? 0 : 6.0 * double(text.size()) - 1.0;
    const double inner_w = w - 2 * (inset + stroke) - side / 8.0, inner_h = h - 2 * (inset + stroke) - side / 8.0;
    const double cell = text.empty() ? 0 : std::min(inner_w / columns, inner_h / 7.0);
    const double text_x = (w - columns * cell) / 2, text_y = (h - 7.0 * cell) / 2;
    constexpr int samples = 4;  // 4x4 supersampling per pixel
    for (uint32_t py = 0; py < height; ++py)
        for (uint32_t px = 0; px < width; ++px) {
            int covered = 0;
            for (int sy = 0; sy < samples; ++sy)
                for (int sx = 0; sx < samples; ++sx) {
                    const double x = px + (sx + 0.5) / samples, y = py + (sy + 0.5) / samples;
                    const double d = rounded_box(x, y, inset, inset, w - inset, h - inset, radius);
                    bool on = std::abs(d) <= stroke / 2;
                    if (!on && cell > 0) {
                        const double gx = (x - text_x) / cell, gy = (y - text_y) / cell;
                        if (gx >= 0 && gy >= 0 && gy < 7) {
                            const auto index = size_t(gx / 6.0), column = size_t(gx) % 6;
                            if (index < text.size() && column < 5)
                                on = glyph(text[index])[size_t(gy)] & (0x10 >> column);
                        }
                    }
                    covered += on;
                }
            image.alpha[size_t(py) * width + px] = uint8_t((covered * 255 + samples * samples / 2) / (samples * samples));
        }
    return image;
}

std::vector<uint8_t> mag_prompt_keycap_bgra(const MagKeycapImage& image) {
    std::vector<uint8_t> out;
    out.reserve(image.alpha.size() * 4);
    for (const uint8_t a : image.alpha) out.insert(out.end(), {0xFF, 0xFF, 0xFF, a});
    return out;
}

namespace {
template <typename Block>
std::vector<uint8_t> blocks(const MagKeycapImage& image, Block block) {
    if (!image.width || !image.height || image.width % 4 || image.height % 4) return {};
    std::vector<uint8_t> out;
    for (uint32_t by = 0; by < image.height; by += 4)
        for (uint32_t bx = 0; bx < image.width; bx += 4) {
            std::array<uint8_t, 16> a{};
            for (int i = 0; i < 16; ++i) a[i] = image.alpha[size_t(by + i / 4) * image.width + bx + i % 4];
            block(a, out);
        }
    return out;
}
}  // namespace

std::vector<uint8_t> mag_prompt_keycap_dxt1(const MagKeycapImage& image) {
    return blocks(image, [](const std::array<uint8_t, 16>& a, std::vector<uint8_t>& out) {
        // Colour 0 = colour 1 = white in the three-colour mode (c0 <= c1): index 0 white, 3 clear.
        out.insert(out.end(), {0xFF, 0xFF, 0xFF, 0xFF});
        uint32_t indices = 0;
        for (int i = 0; i < 16; ++i) indices |= uint32_t(a[i] >= 128 ? 0 : 3) << (2 * i);
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(indices >> (8 * i)));
    });
}

std::vector<uint8_t> mag_prompt_keycap_dxt5(const MagKeycapImage& image) {
    return blocks(image, [](const std::array<uint8_t, 16>& a, std::vector<uint8_t>& out) {
        const uint8_t hi = *std::max_element(a.begin(), a.end()), lo = *std::min_element(a.begin(), a.end());
        // Eight-level alpha (a0 > a1): level 0 = a0, 1 = a1, 2..7 interpolate from a0 towards a1.
        std::array<int, 8> level{hi, lo};
        for (int k = 1; k <= 6; ++k) level[k + 1] = ((7 - k) * hi + k * lo) / 7;
        uint64_t bits = 0;
        if (hi != lo)
            for (int i = 0; i < 16; ++i) {
                int best = 0;
                for (int k = 1; k < 8; ++k)
                    if (std::abs(level[k] - a[i]) < std::abs(level[best] - a[i])) best = k;
                bits |= uint64_t(best) << (3 * i);
            }
        out.push_back(hi);
        out.push_back(lo);
        for (int i = 0; i < 6; ++i) out.push_back(uint8_t(bits >> (8 * i)));
        // Colour: white everywhere (c0 = c1 = 0xFFFF, indices 0).
        out.insert(out.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00});
    });
}
