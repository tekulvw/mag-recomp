/* Synthetic invocation builders shared by the native Edge geometry fixtures. No title data. */
#pragma once
#include "edgegeom.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace mag_edgegeom;

inline void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = uint8_t(x >> 8); v[at + 1] = uint8_t(x); }
inline void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (24 - 8 * i));
}
inline void putf(std::vector<uint8_t>& v, size_t at, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, at, u); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline float getf(const uint8_t* p) { uint32_t u = get32(p); float f; std::memcpy(&f, &u, 4); return f; }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline bool near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

struct Vertex { float p[3]; float n[3]; float w[4]; float t[4]; };

/* One invocation's inputs, with the specification's default view and object. */
struct Case {
    std::vector<uint8_t> userdata = std::vector<uint8_t>(192, 0);
    std::vector<uint8_t> slot[24];
    uint32_t address[24]{};

    Case(uint32_t mode = 0x08, uint8_t flavour = 2, uint8_t group = 1) {
        put32(userdata, 0xAC, mode); userdata[0xA5] = flavour; userdata[0xA7] = group; userdata[0xB0] = 1;
        putf(userdata, 0x6C, 1.0f); putf(userdata, 0x7C, 1.0f);           /* quaternion w, scale */
        put32(userdata, 0x94, 0x10000000); put32(userdata, 0x98, 0x70000000); put32(userdata, 0x9C, 7);
        slot[17].assign(128, 0);
        for (int i = 0; i < 4; ++i) putf(slot[17], 0x10 + 20 * i, 1.0f);   /* C = identity */
        putf(slot[17], 0x50, 640); putf(slot[17], 0x54, -360); putf(slot[17], 0x58, 0.5f);
        putf(slot[17], 0x60, 640); putf(slot[17], 0x64, 360); putf(slot[17], 0x68, 0.5f);
        slot[19].assign(16, 0xFF);
        slot[19][0] = 0x81; slot[19][1] = 7; slot[19][2] = 0x00; slot[19][6] = 0x10; slot[19][7] = 0;
        slot[0].assign(8, 0);                                                /* no declarations */
        for (int i = 0; i < 24; ++i) address[i] = 0x20000000u + 0x10000u * i;
    }
    void hole(uint8_t units) { slot[19][1] = units; put32(userdata, 0x9C, units); }
    void counts(uint16_t v, uint16_t k) { put16(slot[19], 8, v); put16(slot[19], 10, k); }
    void indices(std::initializer_list<uint16_t> list) {
        slot[1].assign(((list.size() * 2 + 15) / 16) * 16, 0);
        size_t i = 0; for (uint16_t x : list) put16(slot[1], 2 * i++, x);
    }
    void positions(const std::vector<Vertex>& vs) {             /* three f32 per vertex (codec 0) */
        slot[7].assign(((vs.size() * 12 + 15) / 16) * 16, 0);
        for (size_t i = 0; i < vs.size(); ++i) for (int k = 0; k < 3; ++k) putf(slot[7], 12 * i + 4 * k, vs[i].p[k]);
    }
    /* Descriptor-driven primary stream: f32 position, f32 normal, f32 or byte weights, f32 tangent. */
    void attributes(const std::vector<Vertex>& vs, bool weights_bytes = false) {
        slot[19][2] = 0xFF;
        const uint8_t stride = 56;
        slot[20].assign(8 + 8 * 4, 0);
        slot[20][0] = 4; slot[20][1] = stride;
        const uint8_t decl[4][5] = {{0, 2, 3, 1, 4}, {12, 2, 3, 2, 4}, {24, uint8_t(weights_bytes ? 4 : 2), 4, 9, 4}, {40, 2, 4, 3, 4}};
        for (int d = 0; d < 4; ++d) std::memcpy(&slot[20][8 + 8 * d], decl[d], 5);
        slot[7].assign(((vs.size() * stride + 15) / 16) * 16, 0);
        for (size_t i = 0; i < vs.size(); ++i) {
            const size_t b = i * stride;
            for (int k = 0; k < 3; ++k) { putf(slot[7], b + 4 * k, vs[i].p[k]); putf(slot[7], b + 12 + 4 * k, vs[i].n[k]); }
            for (int k = 0; k < 4; ++k) {
                if (weights_bytes) slot[7][b + 24 + k] = uint8_t(std::lround(vs[i].w[k] * 255.0f));
                else putf(slot[7], b + 24 + 4 * k, vs[i].w[k]);
                putf(slot[7], b + 40 + 4 * k, vs[i].t[k]);
            }
        }
    }
    void layout(std::initializer_list<OutputDeclaration> decls, uint8_t stride) {
        slot[19][4] = 0xFF;
        slot[0].assign(8 + 8 * decls.size(), 0);
        slot[0][0] = uint8_t(decls.size()); slot[0][1] = stride;
        size_t i = 0;
        for (const auto& d : decls) {
            uint8_t* p = &slot[0][8 + 8 * i++];
            p[0] = d.offset; p[1] = d.format; p[2] = d.count; p[3] = d.semantic; p[5] = d.attribute;
        }
    }
    Inputs inputs() const {
        Inputs in; in.userdata = userdata.data();
        for (int i = 0; i < 24; ++i) in.slot[i] = Stream{slot[i].empty() ? nullptr : slot[i].data(), uint32_t(slot[i].size()), address[i]};
        return in;
    }
};

inline std::vector<uint16_t> kept(const Prepared& p) {
    std::vector<uint16_t> out;
    if (!p.data) return out;
    for (uint32_t i = 0; i < p.surviving; ++i) out.push_back(get16(p.data + 2 * i));
    return out;
}

inline Vertex at(float x, float y, float z) { Vertex v{}; v.p[0] = x; v.p[1] = y; v.p[2] = z; v.w[3] = 1; v.t[3] = 1; return v; }

/* A group-3 wind invocation with zero wind amplitude: positions pass through, flavour 1. */
inline Case wind_case(const std::vector<Vertex>& vs, std::initializer_list<uint16_t> idx) {
    Case c(0x28, 1, 3);
    c.attributes(vs, true);
    c.layout({{0, 2, 3, 1, 0}, {12, 6, 1, 2, 2}, {16, 4, 4, 9, 7}}, 20);
    c.hole(15);
    c.counts(uint16_t(vs.size()), uint16_t(idx.size()));
    c.indices(idx);
    return c;
}

