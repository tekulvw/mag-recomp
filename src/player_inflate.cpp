// A small decoder for zlib streams, written from RFC 1950 and RFC 1951.
#include "player_inflate.h"

namespace {
struct Reader {
    const uint8_t *data; size_t size, at; uint32_t held = 0; int count = 0; bool failed = false;
    uint32_t take(int bits) {
        while (count < bits) {
            if (at >= size) { failed = true; return 0; }
            held |= uint32_t(data[at++]) << count; count += 8;
        }
        uint32_t value = held & ((uint32_t(1) << bits) - 1);
        held >>= bits; count -= bits; return value;
    }
    void align() { held = 0; count = 0; }  // Fewer than 8 bits are ever buffered.
};

// A canonical prefix code: how many codes have each length, and the symbols in code order.
struct Code { uint16_t lengths[16] = {}; uint16_t symbols[320] = {}; };

bool build(Code &code, const uint8_t *lengths, int n) {
    for (auto &c : code.lengths) c = 0;
    for (int i = 0; i < n; ++i) code.lengths[lengths[i]]++;
    code.lengths[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) { left <<= 1; left -= code.lengths[len]; if (left < 0) return false; }
    uint16_t next[16] = {};
    for (int len = 1; len < 15; ++len) next[len + 1] = uint16_t(next[len] + code.lengths[len]);
    for (int i = 0; i < n; ++i) if (lengths[i]) code.symbols[next[lengths[i]]++] = uint16_t(i);
    return true;
}

int decode(Reader &in, const Code &code) {
    int value = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        value |= int(in.take(1));
        if (in.failed) return -1;
        int count = code.lengths[len];
        if (value - first < count) return code.symbols[index + value - first];
        index += count; first = (first + count) << 1; value <<= 1;
    }
    return -1;
}

const uint16_t length_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t length_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t distance_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                    4097,6145,8193,12289,16385,24577};
const uint8_t distance_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool symbols(Reader &in, const Code &literals, const Code &distances, uint8_t *out, size_t expected, size_t &written) {
    for (;;) {
        int symbol = decode(in, literals);
        if (symbol < 0) return false;
        if (symbol < 256) { if (written >= expected) return false; out[written++] = uint8_t(symbol); continue; }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        size_t length = length_base[symbol] + in.take(length_extra[symbol]);
        int d = decode(in, distances);
        if (d < 0 || d >= 30) return false;
        size_t distance = distance_base[d] + in.take(distance_extra[d]);
        if (in.failed || distance > written || length > expected - written) return false;
        for (size_t i = 0; i < length; ++i, ++written) out[written] = out[written - distance];
    }
}

bool fixed(Reader &in, uint8_t *out, size_t expected, size_t &written) {
    uint8_t lengths[288];
    for (int i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    Code literals, distances; build(literals, lengths, 288);
    for (int i = 0; i < 30; ++i) lengths[i] = 5;
    build(distances, lengths, 30);
    return symbols(in, literals, distances, out, expected, written);
}

bool dynamic(Reader &in, uint8_t *out, size_t expected, size_t &written) {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int literal_count = int(in.take(5)) + 257, distance_count = int(in.take(5)) + 1, length_count = int(in.take(4)) + 4;
    if (in.failed || literal_count > 286 || distance_count > 30) return false;
    uint8_t lengths[320] = {};
    for (int i = 0; i < length_count; ++i) lengths[order[i]] = uint8_t(in.take(3));
    Code lengths_code;
    if (in.failed || !build(lengths_code, lengths, 19)) return false;
    int total = literal_count + distance_count;
    for (int i = 0; i < total;) {
        int symbol = decode(in, lengths_code);
        if (symbol < 0) return false;
        if (symbol < 16) { lengths[i++] = uint8_t(symbol); continue; }
        uint8_t value = 0; int repeat;
        if (symbol == 16) { if (!i) return false; value = lengths[i - 1]; repeat = 3 + int(in.take(2)); }
        else if (symbol == 17) repeat = 3 + int(in.take(3));
        else repeat = 11 + int(in.take(7));
        if (in.failed || i + repeat > total) return false;
        while (repeat--) lengths[i++] = value;
    }
    if (!lengths[256]) return false;
    Code literals, distances;
    if (!build(literals, lengths, literal_count) || !build(distances, lengths + literal_count, distance_count)) return false;
    return symbols(in, literals, distances, out, expected, written);
}
}

bool mag::zlib_decode(const uint8_t *source, size_t size, uint8_t *output, size_t expected) {
    if (size < 6) return false;
    unsigned method = source[0], flags = source[1];
    if ((method & 15) != 8 || (method >> 4) > 7 || (method * 256 + flags) % 31 || (flags & 0x20)) return false;
    Reader in{source, size, 2};
    size_t written = 0;
    for (bool last = false; !last;) {
        last = in.take(1); unsigned type = in.take(2);
        if (in.failed) return false;
        if (type == 0) {
            in.align();
            if (size - in.at < 4) return false;
            size_t length = source[in.at] | source[in.at + 1] << 8, check = source[in.at + 2] | source[in.at + 3] << 8;
            in.at += 4;
            if ((length ^ 0xFFFF) != check || length > size - in.at || length > expected - written) return false;
            for (size_t i = 0; i < length; ++i) output[written++] = source[in.at++];
        } else if (type == 1) { if (!fixed(in, output, expected, written)) return false; }
        else if (type == 2) { if (!dynamic(in, output, expected, written)) return false; }
        else return false;
    }
    in.align();
    if (written != expected || size - in.at < 4) return false;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < written; ++i) { a = (a + output[i]) % 65521; b = (b + a) % 65521; }
    uint32_t stored = uint32_t(source[in.at]) << 24 | uint32_t(source[in.at + 1]) << 16 |
                      uint32_t(source[in.at + 2]) << 8 | source[in.at + 3];
    return stored == (b << 16 | a);
}
