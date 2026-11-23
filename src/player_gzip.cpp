// gzip with fixed-Huffman deflate (src/player_gzip.h), written from RFC 1951 and RFC 1952.
#include "player_gzip.h"
#include <vector>

namespace {
// Bits are packed least significant first (RFC 1951, 3.1.1).
struct BitWriter {
    std::string out;
    uint64_t buffer = 0;
    unsigned count = 0;
    void bits(uint32_t value, unsigned n) {
        buffer |= uint64_t(value) << count;
        count += n;
        while (count >= 8) { out += char(buffer & 0xFF); buffer >>= 8; count -= 8; }
    }
    // Huffman codes are defined most significant bit first.
    void code(uint32_t value, unsigned n) {
        uint32_t reversed = 0;
        for (unsigned i = 0; i < n; ++i) reversed |= ((value >> i) & 1u) << (n - 1 - i);
        bits(reversed, n);
    }
    void flush() { if (count) { out += char(buffer & 0xFF); buffer = 0; count = 0; } }
};

// The fixed literal/length code (RFC 1951, 3.2.6).
void literal(BitWriter &w, unsigned symbol) {
    if (symbol < 144) w.code(0x30 + symbol, 8);
    else if (symbol < 256) w.code(0x190 + (symbol - 144), 9);
    else if (symbol < 280) w.code(symbol - 256, 7);
    else w.code(0xC0 + (symbol - 280), 8);
}

constexpr uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void match(BitWriter &w, unsigned length, unsigned distance) {
    unsigned l = 28;
    while (length_base[l] > length) --l;
    literal(w, 257 + l);
    if (length_extra[l]) w.bits(length - length_base[l], length_extra[l]);
    unsigned d = 29;
    while (distance_base[d] > distance) --d;
    w.code(d, 5);
    if (distance_extra[d]) w.bits(distance - distance_base[d], distance_extra[d]);
}

constexpr unsigned window = 32768, min_match = 3, max_match = 258, max_chain = 48;
constexpr unsigned hash_bits = 15, hash_size = 1u << hash_bits;

unsigned hash3(const uint8_t *p) { return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (hash_size - 1); }
}  // namespace

uint32_t mag::crc32(const void *data, size_t size, uint32_t crc) {
    static uint32_t table[256];
    static bool ready = [] {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return true;
    }();
    (void)ready;
    const auto *p = static_cast<const uint8_t *>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::string mag::gzip_compress(const void *data, size_t size) {
    const auto *in = static_cast<const uint8_t *>(data);
    BitWriter w;
    // Header: magic, deflate, no flags, no time, no extra flags, unknown OS.
    w.out.assign("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    w.bits(1, 1);  // the final block
    w.bits(1, 2);  // fixed Huffman codes
    // Hash chains: head holds the latest position + 1 for each hash, previous links back.
    std::vector<uint32_t> head(hash_size, 0), previous(window, 0);
    size_t i = 0;
    auto insert = [&](size_t at) {
        if (at + min_match > size) return;
        unsigned h = hash3(in + at);
        previous[at % window] = head[h];
        head[h] = uint32_t(at + 1);
    };
    while (i < size) {
        unsigned best = 0, best_distance = 0;
        if (i + min_match <= size) {
            uint32_t candidate = head[hash3(in + i)];
            const size_t limit = size - i < max_match ? size - i : max_match;
            for (unsigned chain = 0; candidate && chain < max_chain; ++chain) {
                const size_t at = candidate - 1;
                if (at >= i || i - at > window - 1) break;
                unsigned length = 0;
                while (length < limit && in[at + length] == in[i + length]) ++length;
                if (length > best) { best = length; best_distance = unsigned(i - at); if (length == limit) break; }
                const uint32_t next = previous[at % window];
                if (!next || next - 1 >= at) break;  // the chain only runs backwards
                candidate = next;
            }
        }
        if (best >= min_match) {
            match(w, best, best_distance);
            for (unsigned k = 0; k < best; ++k) insert(i + k);
            i += best;
        } else {
            literal(w, in[i]);
            insert(i);
            ++i;
        }
    }
    literal(w, 256);  // end of block
    w.flush();
    const uint32_t crc = crc32(in, size), length = uint32_t(size);
    for (int k = 0; k < 4; ++k) w.out += char((crc >> (8 * k)) & 0xFF);
    for (int k = 0; k < 4; ++k) w.out += char((length >> (8 * k)) & 0xFF);
    return w.out;
}

std::string mag::base64_encode(const std::string &in) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = uint32_t((unsigned char)in[i]) << 16 |
                           (i + 1 < in.size() ? uint32_t((unsigned char)in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? uint32_t((unsigned char)in[i + 2]) : 0);
        out += alphabet[n >> 18 & 63];
        out += alphabet[n >> 12 & 63];
        out += i + 1 < in.size() ? alphabet[n >> 6 & 63] : '=';
        out += i + 2 < in.size() ? alphabet[n & 63] : '=';
    }
    return out;
}
