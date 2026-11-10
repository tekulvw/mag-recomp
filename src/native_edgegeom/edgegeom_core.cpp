/* Native Edge geometry child job, host-independent core. Behaviour is specified in
 * docs/render-frontend/behavior/edge-geometry-job.md; section numbers below refer to it. */
#include "edgegeom.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

/* No fused multiply-add: the facing rule must see an exact zero area when two screen positions
 * coincide, which a fused a*b - c*d does not give. Builds with GCC pass -ffp-contract=off. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace mag_edgegeom {

/* The game's wind values (edge-geometry-job.md, "Wind values (game-supplied)"); the same for group
 * 0 (which uses only KA1, KB1 and KC) and group 3. */
const WindTuning kWindTuning = {0.375f, 0.193f, 1.975f, 0.793f, 0.33f};

namespace {

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
/* Rebased half (geometry-streams.md): every exponent is a normal binary32 exponent, so the
 * binary32 bit pattern is assembled directly. */
inline float rebased_bits(uint32_t h) {
    const uint32_t magnitude = h & 0x7FFFu;
    const uint32_t u = (h & 0x8000u) << 16 | (magnitude ? (((h >> 10) & 31u) + 112u) << 23 | (h & 0x3FFu) << 13 : 0u);
    float f; std::memcpy(&f, &u, 4); return f;
}
inline float bef32(const uint8_t* p) { uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return f; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void putf32(uint8_t* p, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(p, u); }
inline bool finite(float f) { return std::isfinite(f); }
inline uint32_t roundup(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

/* Userdata offsets (section "Userdata"). */
enum : unsigned {
    kM0 = 0x20, kM1 = 0x30, kFade = 0x50, kQuat = 0x60, kTranslation = 0x70, kScale = 0x7C,
    kWindWords = 0x80, kPhase = 0x90, kRingTable = 0x94, kHoleAddress = 0x98, kHoleUnits = 0x9C,
    kBlendCount = 0xA4, kFlavour = 0xA5, kGroup = 0xA7, kModeWord = 0xAC, kInstanceCount = 0xB0
};
enum : unsigned { kSlotLayout = 0, kSlotIndices = 1, kSlotPalette0 = 3, kSlotPalette1 = 4, kSlotInfluences = 5,
                  kSlotVertices = 7, kSlotSecondary = 10, kSlotInstances = 13, kSlotView = 17,
                  kSlotConfiguration = 19, kSlotPrimaryTable = 20, kSlotSecondaryTable = 21 };
/* RSX vertex formats used by the output layouts. */
enum : uint8_t { kFormatFloat = 2, kFormatHalf = 3, kFormatUByte = 4, kFormatCmp = 6 };
enum : uint8_t { kSemPosition = 1, kSemNormal = 2, kSemTangent = 3, kSemWeights = 9 };

struct Declaration {
    uint8_t offset, codec, count, semantic, element;
};

inline float frac(float x) { return x - std::floor(x); }
inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
/* group-zero-deformation.md "Helper functions". */
inline float wave_of(float phase, float f) {
    const float x = f * (2.0f * frac(phase) - 1.0f) + 0.5f;
    return smooth(std::fabs(2.0f * frac(x) - 1.0f));
}
/* Whether all of n values in each of three arrays are finite, as a loop that vectorizes. */
inline bool all_finite(const float* a, const float* b, const float* c, uint32_t n) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t ua, ub, uc;
        std::memcpy(&ua, a + i, 4); std::memcpy(&ub, b + i, 4); std::memcpy(&uc, c + i, 4);
        bad |= uint32_t((ua & 0x7F800000u) == 0x7F800000u) | uint32_t((ub & 0x7F800000u) == 0x7F800000u) |
               uint32_t((uc & 0x7F800000u) == 0x7F800000u);
    }
    return !bad;
}

}  // namespace

const char* reason_name(Reason r) {
    static const char* const names[kReasonCount] = {
        "handled", "identity", "mode", "blend_shapes", "configuration", "hole", "stream_short", "kind",
        "codec", "index_range", "non_finite", "joint", "budget", "output_format", "hole_overflow",
        "ring_full", "inputs", "service", "forced"};
    return r < kReasonCount ? names[r] : "unknown";
}

const char* kind_name(Kind k) {
    static const char* const names[kKindCount] = {"none", "index_only", "instanced_wind", "wind", "skinned", "ripple"};
    return k < kKindCount ? names[k] : "unknown";
}

float rebased_half(uint16_t h) { return rebased_bits(h); }

bool spherical_normal(const uint8_t b[3], float out[4]) {
    const uint32_t n = uint32_t(b[0]) << 16 | uint32_t(b[1]) << 8 | b[2];
    const unsigned sel = n & 15u;
    if (sel >= 12) return false;
    const unsigned u = (n >> 14) & 1023u, v = (n >> 4) & 1023u;
    constexpr float kBase = -0.70710678118654752f, kStep = 1.41421356237309505f / 1023.0f;
    const float a = kBase + float(u) * kStep, c = kBase + float(v) * kStep;
    float r = 1.0f - (a * a + c * c);
    if (r < 0.0f) r = 0.0f;
    const float m = (sel & 1u) ? std::sqrt(r) : -std::sqrt(r);
    const unsigned axis = sel >> 2;
    float* lanes = out;
    if (axis == 0) { lanes[0] = m; lanes[1] = a; lanes[2] = c; }
    else if (axis == 1) { lanes[0] = a; lanes[1] = m; lanes[2] = c; }
    else { lanes[0] = a; lanes[1] = c; lanes[2] = m; }
    lanes[3] = (sel & 2u) ? 1.0f : -1.0f;
    return true;
}

float wave(float phase, float f) { return wave_of(phase, f); }

uint32_t pack_normal_cmp(float x, float y, float z) {
    auto q = [](float v, float scale, int lo, int hi) {
        float f = std::floor(v * scale - 0.5f);
        if (!(f >= float(lo))) f = float(lo);   /* NaN clamps low; inputs are checked finite */
        if (f > float(hi)) f = float(hi);
        return int32_t(f);
    };
    const uint32_t ix = uint32_t(q(x, 1023.5f, -1024, 1023)) & 0x7FFu;
    const uint32_t iy = uint32_t(q(y, 1023.5f, -1024, 1023)) & 0x7FFu;
    const uint32_t iz = uint32_t(q(z, 511.5f, -512, 511)) & 0x3FFu;
    return ix | iy << 11 | iz << 22;
}

uint16_t to_binary16(float value) {
    uint32_t u; std::memcpy(&u, &value, 4);
    const uint16_t sign = uint16_t((u >> 16) & 0x8000u);
    const uint32_t abs = u & 0x7FFFFFFFu;
    if (abs - 0x38800000u < 0x477FF000u - 0x38800000u)                 /* normal binary16 result */
        return uint16_t(sign | (((abs + 0xFFFu + ((abs >> 13) & 1u)) >> 13) - (112u << 10)));
    if (abs >= 0x7F800000u) return uint16_t(sign | 0x7C00u | (abs > 0x7F800000u ? 0x200u : 0u));
    if (abs >= 0x477FF000u) return uint16_t(sign | 0x7C00u);            /* rounds to infinity */
    if (abs < 0x38800000u) {                                            /* binary16 subnormal or zero */
        if (abs < 0x33000000u) return sign;
        const uint32_t mant = (abs & 0x7FFFFFu) | 0x800000u;
        const int shift = 126 - int(abs >> 23);                         /* 14..24 */
        uint32_t half = mant >> shift;
        const uint32_t rest = mant & ((1u << shift) - 1u), mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (half & 1u))) ++half;
        return uint16_t(sign | half);
    }
    uint32_t half = ((abs >> 13) - (112u << 10));
    const uint32_t rest = abs & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;
    return uint16_t(sign | half);
}

RingRecord read_ring(const uint8_t r[128]) {
    return RingRecord{be32(r), be32(r + 4), be32(r + 8), be32(r + 12), be32(r + 16), be32(r + 20), be32(r + 24)};
}

void write_ring(uint8_t r[128], uint32_t top, uint32_t consumed) {
    put32(r + 12, top);
    put32(r + 24, consumed);
}

Allocation allocate(const RingRecord& ring, uint32_t n, const uint32_t* refreshed, uint32_t* start, uint32_t* consumed) {
    const uint64_t S = ring.start, E = ring.end, T = ring.top, N = n;
    const uint64_t U = refreshed ? *refreshed : ring.consumed;
    if (S > E || T < S || T > E || N == 0 || N > E - S) return refreshed ? Allocation::kFull : Allocation::kNeedsRefresh;
    const uint64_t X = T + N <= E ? T : S;
    bool free;
    if (U > T) free = X == T && X + N < U;          /* free bytes [T, U); must end before U */
    else free = X == T || X + N < U;                /* free bytes [T, E) and [S, U) */
    if (!free) return refreshed ? Allocation::kFull : Allocation::kNeedsRefresh;
    *start = uint32_t(X);
    *consumed = uint32_t(U);
    return Allocation::kPlaced;
}

namespace {

/* Reads one declaration's rows into four lanes (geometry-streams.md, "Descriptor codecs").
 * Returns kHandled, kStreamShort (extent) or kCodec. */
Reason decode_declaration(const Stream& s, uint32_t stride, const Declaration& d, uint32_t vertices,
                          std::vector<float>* lane, bool* any_nonfinite) {
    for (int k = 0; k < 4; ++k) lane[k].resize(vertices);
    if (d.semantic == 0) return kCodec;
    const bool position = d.semantic == kSemPosition;
    unsigned lane_bytes = 0;
    switch (d.codec) {
    case 1: case 3: case 5: lane_bytes = 2; break;
    case 2: lane_bytes = 4; break;
    case 4: case 7: lane_bytes = 1; break;
    case 6: case 9: break;
    default: return kCodec;
    }
    const unsigned decoded = position ? std::min<unsigned>(d.count, 4) : 4;
    const unsigned field = d.codec == 6 ? 4 : d.codec == 9 ? 3 : decoded * lane_bytes;
    const uint64_t A = s.address;
    *any_nonfinite = false;
    /* Fast paths for 16-byte aligned streams (every observed stream): the attribute of vertex i
     * is at i·S + O, so one extent check covers all rows. */
    if (!(A & 15) && vertices && (d.codec == 3 || d.codec == 4 || (d.codec == 2 && (position || d.count >= 4)))) {
        if (uint64_t(vertices - 1) * stride + d.offset + field > s.size) return kStreamShort;
        float* out[4] = {lane[0].data(), lane[1].data(), lane[2].data(), lane[3].data()};
        const uint8_t* p = s.data + d.offset;
        for (unsigned k = decoded; k < 4; ++k) {
            const float fill = k == 3 ? 1.0f : 0.0f;
            for (uint32_t i = 0; i < vertices; ++i) out[k][i] = fill;
        }
        if (d.codec == 3) {
            for (unsigned k = 0; k < decoded; ++k)
                for (uint32_t i = 0; i < vertices; ++i) out[k][i] = rebased_bits(be16(p + size_t(i) * stride + 2 * k));
        } else if (d.codec == 4) {
            for (unsigned k = 0; k < decoded; ++k)
                for (uint32_t i = 0; i < vertices; ++i) out[k][i] = float(p[size_t(i) * stride + k]) / 255.0f;
        } else {
            bool bad = false;
            for (unsigned k = 0; k < decoded; ++k)
                for (uint32_t i = 0; i < vertices; ++i) {
                    const float f = bef32(p + size_t(i) * stride + 4 * k);
                    out[k][i] = f; bad |= !finite(f);
                }
            *any_nonfinite = bad;
        }
        return kHandled;
    }
    for (uint32_t i = 0; i < vertices; ++i) {
        const uint64_t L = uint64_t(i) * stride + d.offset;
        const int64_t off = int64_t(((A + L) & ~uint64_t(15)) + (L & 15)) - int64_t(A);
        if (off < 0 || uint64_t(off) + field > s.size) return kStreamShort;
        const uint8_t* p = s.data + off;
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (d.codec == 6) {
            const uint32_t w = be32(p);
            const int32_t x = int32_t(w << 21) >> 21, y = int32_t(w << 10) >> 21, z = int32_t(w) >> 22;
            v[0] = float(2 * x + 1) / 2047.0f; v[1] = float(2 * y + 1) / 2047.0f; v[2] = float(2 * z + 1) / 1023.0f;
            if (position) for (unsigned k = d.count; k < 3; ++k) v[k] = 0.0f;
        } else if (d.codec == 9) {
            if (!spherical_normal(p, v)) return kCodec;
            if (position) { for (unsigned k = d.count; k < 3; ++k) v[k] = 0.0f; v[3] = 1.0f; }
        } else {
            for (unsigned k = 0; k < decoded; ++k) {
                const uint8_t* q = p + k * lane_bytes;
                uint8_t wrapped[4];
                if (d.codec == 2 && k >= d.count && (L & 15) + d.element <= 16) {
                    for (unsigned b = 0; b < 4; ++b) {
                        const uint64_t at = (L & 15) + 4 * k + b;
                        const int64_t o = off - int64_t(L & 15) + int64_t(at & 15);
                        wrapped[b] = s.data[o];   /* inside the vertex's own 16-byte block */
                    }
                    q = wrapped;
                }
                switch (d.codec) {
                case 1: v[k] = float(2 * int16_t(be16(q)) + 1) / 65535.0f; break;
                case 2: v[k] = bef32(q); if (!finite(v[k])) *any_nonfinite = true; break;
                case 3: v[k] = rebased_bits(be16(q)); break;
                case 4: v[k] = float(q[0]) / 255.0f; break;
                case 5: v[k] = float(int16_t(be16(q))); break;
                case 7: v[k] = float(q[0]); break;
                }
            }
        }
        for (int k = 0; k < 4; ++k) lane[k][i] = v[k];
    }
    return kHandled;
}

struct Table {
    unsigned count = 0, stride = 0;
    Declaration decl[32]{};
};

bool parse_table(const Stream& s, Table& t) {
    if (s.size < 8 || !s.data) return false;
    t.count = s.data[0]; t.stride = s.data[1];
    if (t.count > 32 || s.size < 8u + 8u * t.count) return false;
    for (unsigned i = 0; i < t.count; ++i) {
        const uint8_t* p = s.data + 8 + 8 * i;
        t.decl[i] = Declaration{p[0], p[1], p[2], p[3], p[4]};
    }
    return true;
}

}  // namespace

/* Section 4 and 5: projection with one composed matrix, then the three removal rules. Indices
 * into the points arrays come from indices_, repeated per instance. Survivors land in kept_. */
Reason Engine::project_and_cull(const Inputs& in, uint32_t vertices, uint32_t instances, const float* px,
                                const float* py, const float* pz, uint32_t points, uint8_t flavour) {
    const uint8_t* u = in.userdata;
    const uint8_t* view = in.slot[kSlotView].data;
    float C[4][4], k[2], o[2];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) C[r][c] = bef32(view + 0x10 + 16 * r + 4 * c);
    k[0] = bef32(view + 0x50); k[1] = bef32(view + 0x54);
    o[0] = bef32(view + 0x60); o[1] = bef32(view + 0x64);
    const float qx = bef32(u + kQuat), qy = bef32(u + kQuat + 4), qz = bef32(u + kQuat + 8), qw = bef32(u + kQuat + 12);
    const float t[3] = {bef32(u + kTranslation), bef32(u + kTranslation + 4), bef32(u + kTranslation + 8)};
    const float sigma = bef32(u + kScale);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) if (!finite(C[r][c])) return kNonFinite;
    if (!finite(k[0]) || !finite(k[1]) || !finite(o[0]) || !finite(o[1]) || !finite(qx) || !finite(qy) ||
        !finite(qz) || !finite(qw) || !finite(t[0]) || !finite(t[1]) || !finite(t[2]) || !finite(sigma))
        return kNonFinite;
    const float R[3][3] = {
        {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
        {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
        {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
    /* World position σ·R(q)·p + t, then the clip position C·(w, 1), evaluated in that order (the
     * order the recordings' borderline decisions agree with best). */
    sx_.resize(points); sy_.resize(points); qx_.resize(points); qy_.resize(points); code_.resize(points);
    float* sx = sx_.data(); float* sy = sy_.data(); float* fx = qx_.data(); float* fy = qy_.data();
    const float xlo = o[0] - std::fabs(k[0]), xhi = o[0] + std::fabs(k[0]);
    const float ylo = o[1] - std::fabs(k[1]), yhi = o[1] + std::fabs(k[1]);
    uint8_t* code = code_.data();
    for (uint32_t i = 0; i < points; ++i) {
        const float x = px[i], y = py[i], z = pz[i];
        const float wx = sigma * (R[0][0] * x + R[0][1] * y + R[0][2] * z) + t[0];
        const float wy = sigma * (R[1][0] * x + R[1][1] * y + R[1][2] * z) + t[1];
        const float wz = sigma * (R[2][0] * x + R[2][1] * y + R[2][2] * z) + t[2];
        const float cx = C[0][0] * wx + C[0][1] * wy + C[0][2] * wz + C[0][3];
        const float cy = C[1][0] * wx + C[1][1] * wy + C[1][2] * wz + C[1][3];
        const float cz = C[2][0] * wx + C[2][1] * wy + C[2][2] * wz + C[2][3];
        const float cw = C[3][0] * wx + C[3][1] * wy + C[3][2] * wz + C[3][3];
        /* Side position: divided by |w| (section 5); for w > 0 it is the screen position. */
        const float aw = cw == 0.0f ? std::numeric_limits<float>::min() : std::fabs(cw);
        const float ux = (cx * k[0] + cw * o[0]) / aw, uy = (cy * k[1] + cw * o[1]) / aw;
        const int behind = int(!(cw > 0.0f)) | int(cz < -cw);
        code[i] = uint8_t(int(ux < xlo) | int(ux > xhi) << 1 | int(uy < ylo) << 2 | int(uy > yhi) << 3 |
                          int(cz < -cw) << 5 | behind << 4);
        const float inv = 1.0f / cw;
        const float X = cx * inv * k[0] + o[0], Y = cy * inv * k[1] + o[1];
        sx[i] = X; sy[i] = Y;
        fx[i] = std::floor(4.0f * X);
        fy[i] = std::floor(4.0f * Y);
    }
    const uint32_t triangles = (uint32_t)indices_.size() / 3;
    kept_.resize(size_t(triangles) * 3 * instances);
    uint16_t* kept = kept_.data();
    uint32_t n = 0;
    const uint16_t* ix = indices_.data();
    for (uint32_t inst = 0; inst < instances; ++inst) {
        const uint32_t base = inst * vertices;
        for (uint32_t tri = 0; tri < triangles; ++tri) {
            const uint32_t a = base + ix[3 * tri], b = base + ix[3 * tri + 1], c = base + ix[3 * tri + 2];
            const uint8_t ca = code[a], cb = code[b], cc = code[c];
            bool keep;
            /* Rule 1: the near plane, or one viewport side for all three. A triangle that survives
             * it with a vertex past the near plane or at w <= 0 is kept, rules 2 and 3 skipped. */
            if (ca & cb & cc & 0x2F) keep = false;
            else if ((ca | cb | cc) & 0x10) keep = true;
            else {
                const float area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a]);
                keep = flavour == 2 ? area < 0.0f : area != 0.0f;
                if (keep && ((fx[a] == fx[b] && fx[b] == fx[c]) || (fy[a] == fy[b] && fy[b] == fy[c]))) keep = false;
            }
            kept[n] = uint16_t(a); kept[n + 1] = uint16_t(b); kept[n + 2] = uint16_t(c);
            n += keep ? 3 : 0;
        }
    }
    kept_.resize(n);
    return kHandled;
}

namespace {

/* Section 8: lays out the hole's words. Returns the word count, or a value above `capacity`
 * when the packed content does not fit. */
unsigned layout_hole(const Prepared& p, uint32_t x, uint32_t x_offset, uint32_t label, uint32_t* words, unsigned capacity) {
    unsigned at = 0;
    bool overflow = false;
    auto emit = [&](uint32_t w) { if (at < capacity) words[at] = w; else overflow = true; ++at; };
    auto packet = [&](uint32_t method, const uint32_t* args, unsigned count, bool incrementing) {
        while (count) {
            const unsigned left = 32u - (((p.hole_address + 4u * at) & 127u) >> 2);
            const unsigned room = left - 1u;
            if (room == 0) { emit(0); continue; }
            unsigned take = std::min(count, room);
            if (!incrementing) take = std::min(take, 2047u);
            emit((incrementing ? 0u : 0x40000000u) | take << 18 | method);
            for (unsigned i = 0; i < take; ++i) emit(args[i]);
            if (incrementing) method += 4 * take;
            args += take; count -= take;
        }
    };
    const uint32_t zero = 0;
    packet(0x1710, &zero, 1, true);
    for (int i = 0; i < 3; ++i) packet(0x1714, &zero, 1, true);
    if (p.surviving) {
        for (unsigned d = 0; d < p.declaration_count; ++d) {
            const OutputDeclaration& decl = p.declarations[d];
            const uint32_t format = p.stride * 256u + decl.count * 16u + decl.format;
            const uint32_t offset = (x_offset + p.index_bytes + decl.offset) & 0x7FFFFFFFu;
            packet(0x1740 + 4 * decl.attribute, &format, 1, true);
            packet(0x1680 + 4 * decl.attribute, &offset, 1, true);
        }
        packet(0x1714, &zero, 1, true);
        const uint32_t index_array[2] = {x_offset, 0x10};
        packet(0x181C, index_array, 2, true);
        const uint32_t triangles = 5;
        packet(0x1808, &triangles, 1, true);
        const unsigned batches = (p.surviving + 255) / 256;
        uint32_t batch[64];
        unsigned done = 0;
        while (done < batches) {
            /* Batches are generated in chunks; consecutive chunks belong to one logical packet,
             * which the packing rule splits exactly as it would split the whole. */
            const unsigned chunk = std::min(batches - done, 64u);
            for (unsigned i = 0; i < chunk; ++i) {
                const uint32_t first = (done + i) * 256u, count = std::min(256u, p.surviving - first);
                batch[i] = (count - 1) << 24 | first;
            }
            if (batches > 64) return capacity + 1;   /* cannot fit any hole of at most 255 units */
            packet(0x1824, batch, chunk, false);
            done += chunk;
        }
        packet(0x1808, &zero, 1, true);
        const uint32_t semaphore = label - 0xB0000000u, top = x + p.request;
        packet(0x1D6C, &semaphore, 1, true);
        packet(0x1D74, &top, 1, true);
    }
    if (overflow) return capacity + 1;
    for (unsigned i = at; i < capacity; ++i) words[i] = 0;
    return at;
}

}  // namespace

void Engine::build_hole(const Prepared& p, uint32_t x, uint32_t x_offset, uint32_t label, uint8_t* out) {
    uint32_t words[1024];
    const unsigned capacity = p.hole_bytes / 4;
    layout_hole(p, x, x_offset, label, words, capacity);
    for (unsigned i = 0; i < capacity; ++i) put32(out + 4 * i, words[i]);
}

Reason Engine::prepare(const Inputs& in, Prepared& out) {
    out = Prepared{};
    const uint8_t* u = in.userdata;
    if (!u) return kInputs;
    const uint32_t mode = be32(u + kModeWord);
    const uint8_t flavour = u[kFlavour], group = u[kGroup];
    /* Check 2. */
    if (!(((mode == 0x08 || mode == 0x0A) && flavour == 2) || (mode == 0x28 && flavour == 1))) return kMode;
    /* Check 3. */
    if (u[kBlendCount] != 0) return kBlendShapes;
    /* Check 4. */
    const Stream& cs = in.slot[kSlotConfiguration];
    if (cs.size < 16 || !cs.data) return kStreamShort;
    const uint8_t* cfg = cs.data;
    if ((cfg[6] != 0x10 && cfg[6] != 0x11) || cfg[7] != 0 || cfg[5] != 0xFF || be32(cfg + 12) != 0xFFFFFFFFu)
        return kConfiguration;
    if ((cfg[0] != 0x81 && cfg[0] != 0x83 && cfg[0] != 0x84) || (cfg[2] != 0x00 && cfg[2] != 0xFF) || cfg[3] != 0xFF ||
        (cfg[4] != 0x00 && cfg[4] != 0xFF))
        return kConfiguration;
    /* Check 5. */
    const uint32_t hole_units = cfg[1];
    out.hole_address = be32(u + kHoleAddress);
    if (hole_units == 0 || hole_units != be32(u + kHoleUnits) || out.hole_address == 0 || (out.hole_address & 15u))
        return kHole;
    out.hole_bytes = hole_units * 16u;
    out.ring_table = be32(u + kRingTable);
    const uint32_t V = be16(cfg + 8), K = be16(cfg + 10);
    out.vertex_count = V; out.index_count = K;
    const bool skinned = (cfg[6] & 0x0F) == 1;
    /* Stream set: 0x81 primary only; 0x83 primary and secondary; 0x84 primary and a secondary
     * that also carries a tangent (ripple). */
    const bool has_secondary = cfg[0] == 0x83 || cfg[0] == 0x84;
    const uint32_t I = (group == 0 && mode == 0x28) ? u[kInstanceCount] : 1;
    out.instances = I;

    /* Check 7: the production rule (section 3). The group and the output layout select what is
     * produced; the mode word and flavour select culling. The layout is read below. */
    Kind kind = kKindNone;
    if (skinned) {
        if ((group == 7 || group == 11) && cfg[4] == 0xFF) kind = kSkinned; else return kKind;
    } else if (group == 0 && mode == 0x28 && cfg[4] == 0x00) {
        kind = kInstancedWind;
    } else if (cfg[4] == 0xFF) {
        kind = kIndexOnly;   /* becomes wind for group 3 with a non-empty layout */
    } else {
        return kKind;
    }
    if (u[kInstanceCount] != 1 && kind != kInstancedWind) return kKind;
    if (kind == kInstancedWind && I == 0) return kKind;
    out.kind = kind;

    /* Output layout. */
    if (cfg[4] == 0x00) {
        out.stride = 12; out.declaration_count = 1;
        out.declarations[0] = OutputDeclaration{0, kFormatFloat, 3, kSemPosition, 0};
    } else {
        const Stream& ls = in.slot[kSlotLayout];
        Table t;
        if (ls.size == 0) t.count = 0;
        else if (!parse_table(ls, t) || t.count > 16) return kConfiguration;
        out.declaration_count = t.count;
        out.stride = t.count ? t.stride : 0;
        for (unsigned i = 0; i < t.count; ++i) {
            const uint8_t* p = ls.data + 8 + 8 * i;
            out.declarations[i] = OutputDeclaration{p[0], p[1], p[2], p[3], p[5]};
            if (p[5] > 15) return kOutputFormat;
        }
    }
    /* Index only: an empty layout, whatever the group. A non-empty layout produces vertices:
     * group 3 is the wind under either mode word; other groups are not specified. */
    if (kind == kIndexOnly && out.declaration_count) {
        if (group == 3) kind = kWind;
        else if (group == 4) kind = kRipple;
        else return kKind;
    }
    out.kind = kind;
    if (kind == kSkinned && out.declaration_count == 0) return kKind;

    /* Check 6 and the primary format. A stream longer than 16 KiB arrives as its first 0x4000
     * bytes in slot 1 (or 7) and the rest in slot 2 (or 8), contiguous in guest memory and packed
     * contiguously in buffer 1: read the two as one stream. */
    auto joined = [](const Stream& first, const Stream& rest) {
        Stream s = first;
        if (first.size == 0x4000u && rest.size && rest.address == first.address + 0x4000u && first.data &&
            rest.data == first.data + 0x4000u)
            s.size += rest.size;
        return s;
    };
    const Stream is = joined(in.slot[kSlotIndices], in.slot[2]);
    const Stream vs = joined(in.slot[kSlotVertices], in.slot[8]);
    if (in.slot[kSlotView].size < 128 || !in.slot[kSlotView].data) return kStreamShort;
    Table primary;
    const bool primary_list = cfg[2] == 0xFF;
    if (primary_list && !parse_table(in.slot[kSlotPrimaryTable], primary)) return kStreamShort;
    const uint32_t primary_stride = primary_list ? primary.stride : 12;
    const bool indices_short = is.size < 2u * K || (K && !is.data);
    const bool vertices_short = uint64_t(vs.size) < uint64_t(V) * primary_stride || (V && !vs.data);
    /* A stream still shorter than its counts: the original's behaviour is not specified, so the
     * invocation is refused (spec 2f4a283). */
    if (indices_short || vertices_short) return kStreamShort;
    if (kind == kInstancedWind && in.slot[kSlotInstances].size < 16u * I) return kStreamShort;
    if (uint64_t(V) * I > 65536u) return kBudget;

    /* Check 8. */
    indices_.resize(K - K % 3);
    for (uint32_t i = 0; i < indices_.size(); ++i) {
        const uint16_t idx = be16(is.data + 2 * i);
        if (idx >= V) return kIndexRange;
        indices_[i] = idx;
    }
    for (uint32_t i = uint32_t(indices_.size()); i < K; ++i) if (be16(is.data + 2 * i) >= V) return kIndexRange;

    /* Decoding (section 2): positions, then the banks the kind needs. */
    Table secondary;
    if (has_secondary && !parse_table(in.slot[kSlotSecondaryTable], secondary)) return kStreamShort;
    auto lookup = [&](uint8_t semantic, Bank& bank, bool required) -> Reason {
        bank.present = false;
        const Stream* stream = nullptr; const Table* table = nullptr; const Declaration* decl = nullptr;
        if (primary_list) for (unsigned i = 0; i < primary.count && !decl; ++i)
            if (primary.decl[i].semantic == semantic) { decl = &primary.decl[i]; table = &primary; stream = &vs; }
        if (!decl && has_secondary) for (unsigned i = 0; i < secondary.count && !decl; ++i)
            if (secondary.decl[i].semantic == semantic) { decl = &secondary.decl[i]; table = &secondary; stream = &in.slot[kSlotSecondary]; }
        if (!decl) return required ? kCodec : kHandled;
        bool nonfinite = false;
        const Reason r = decode_declaration(*stream, table->stride, *decl, V, bank.lane, &nonfinite);
        if (r != kHandled) return r == kStreamShort ? kStreamShort : kCodec;
        bank.present = true; bank.codec = decl->codec;
        bank.raw = stream->data + decl->offset; bank.raw_stride = table->stride;
        return nonfinite ? kNonFinite : kHandled;
    };
    Reason r;
    if (primary_list) {
        if ((r = lookup(kSemPosition, position_, true)) != kHandled) return r;
    } else {
        position_.present = true;
        for (int k = 0; k < 4; ++k) position_.lane[k].resize(V);
        for (uint32_t i = 0; i < V; ++i) {
            for (int k = 0; k < 3; ++k) {
                const float f = bef32(vs.data + 12 * i + 4 * k);
                if (!finite(f)) return kNonFinite;
                position_.lane[k][i] = f;
            }
            position_.lane[3][i] = 1.0f;
        }
    }
    const float* px = position_.lane[0].data();
    const float* py = position_.lane[1].data();
    const float* pz = position_.lane[2].data();

    const uint32_t Vout = kind == kIndexOnly ? 0 : V * I;
    out.produced = Vout;
    if (kind == kInstancedWind) {
        if ((r = lookup(kSemNormal, normal_, true)) != kHandled) return r;
        if ((r = lookup(kSemWeights, weight_, true)) != kHandled) return r;
        const float M0x = bef32(u + kM0), M0y = bef32(u + kM0 + 4), M1x = bef32(u + kM1);
        const float w0 = bef32(u + kWindWords), w2 = bef32(u + kWindWords + 8), w3 = bef32(u + kWindWords + 12);
        const float theta = bef32(u + kPhase), fm = bef32(u + kFade), fs = bef32(u + kFade + 4);
        const float T[3] = {bef32(u + kTranslation), bef32(u + kTranslation + 4), bef32(u + kTranslation + 8)};
        const uint8_t* view = in.slot[kSlotView].data;
        const float E3[4] = {bef32(view + 0x40), bef32(view + 0x44), bef32(view + 0x48), bef32(view + 0x4C)};
        for (float f : {M0x, M0y, M1x, w0, w2, w3, theta, fm, fs, T[0], T[1], T[2], E3[0], E3[1], E3[2], E3[3]})
            if (!finite(f)) return kNonFinite;
        const float f = w0 * M1x, A1 = M0x * w2, A2 = M0y * w3;
        const WindTuning& K = kWindTuning;
        out_x_.resize(Vout); out_y_.resize(Vout); out_z_.resize(Vout);
        const float* nx = normal_.lane[0].data(); const float* ny = normal_.lane[1].data();
        const float* ww = weight_.lane[3].data();
        const uint8_t* rows = in.slot[kSlotInstances].data;
        for (uint32_t i = 0; i < I; ++i) {
            const uint8_t* row = rows + 16 * i;
            const float o[3] = {rebased_bits(be16(row)), rebased_bits(be16(row + 2)), rebased_bits(be16(row + 4))};
            const float s0 = rebased_bits(be16(row + 6));
            float qx = rebased_bits(be16(row + 8)), qy = rebased_bits(be16(row + 10)),
                  qz = rebased_bits(be16(row + 12)), qw = rebased_bits(be16(row + 14));
            const float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
            float R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            if (len > 0.0f) {
                qx /= len; qy /= len; qz /= len; qw /= len;
                const float m[3][3] = {
                    {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)},
                    {2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx)},
                    {2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy)}};
                std::memcpy(R, m, sizeof R);
            }
            const float c[3] = {T[0] + o[0], T[1] + o[1], T[2] + o[2]};
            const float d = E3[0] * c[0] + E3[1] * c[1] + E3[2] * c[2] + E3[3];
            const float fade_raw = (d - fs) * fm;
            if (!finite(fade_raw)) return kNonFinite;
            const float fade = std::min(1.0f, std::max(0.0f, fade_raw));
            const float s = s0 * (1.0f - fade);
            float SR[3][3];
            for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) SR[a][b] = s * R[a][b];
            float* ox = out_x_.data() + size_t(i) * V; float* oy = out_y_.data() + size_t(i) * V;
            float* oz = out_z_.data() + size_t(i) * V;
            for (uint32_t v = 0; v < V; ++v) {
                const float Bx = SR[0][0] * px[v] + SR[0][1] * py[v] + SR[0][2] * pz[v] + o[0];
                const float By = SR[1][0] * px[v] + SR[1][1] * py[v] + SR[1][2] * pz[v] + o[1];
                const float Bz = SR[2][0] * px[v] + SR[2][1] * py[v] + SR[2][2] * pz[v] + o[2];
                const float alpha = K.ka1 * (theta + Bx + By);
                const float beta = K.kb1 * (theta + K.kc * (Bx + By + Bz));
                const float D = (A1 * wave_of(beta, f) + A2 * wave_of(alpha, f)) * (1.0f - ww[v]);
                ox[v] = Bx + D * nx[v]; oy[v] = By + D * ny[v]; oz[v] = Bz + D * nx[v];
            }
            if (!all_finite(ox, oy, oz, V)) return kNonFinite;
        }
    } else if (kind == kWind) {
        if ((r = lookup(kSemNormal, normal_, true)) != kHandled) return r;
        if ((r = lookup(kSemWeights, weight_, true)) != kHandled) return r;
        const float M0x = bef32(u + kM0), M0y = bef32(u + kM0 + 4), M1x = bef32(u + kM1);
        const float w0 = bef32(u + kWindWords), w2 = bef32(u + kWindWords + 8), w3 = bef32(u + kWindWords + 12);
        const float theta = bef32(u + kPhase);
        const float T[3] = {bef32(u + kTranslation), bef32(u + kTranslation + 4), bef32(u + kTranslation + 8)};
        for (float f : {M0x, M0y, M1x, w0, w2, w3, theta, T[0], T[1], T[2]}) if (!finite(f)) return kNonFinite;
        const float f = M1x * w0, A1 = M0x * w2, A2 = M0y * w3, sum = T[0] + T[1] + T[2];
        const WindTuning& K = kWindTuning;
        out_x_.resize(V); out_y_.resize(V); out_z_.resize(V);
        const float* nx = normal_.lane[0].data(); const float* ny = normal_.lane[1].data();
        const float* Wx = weight_.lane[0].data(); const float* Wy = weight_.lane[1].data();
        const float* Wz = weight_.lane[2].data();
        float* ox = out_x_.data(); float* oy = out_y_.data(); float* oz = out_z_.data();
        for (uint32_t v = 0; v < V; ++v) {
            const float t = Wy[v] + sum, a = theta + t, b = theta + (K.kc + t) * (px[v] + py[v] + pz[v]);
            const float wb = wave_of(K.kb1 * b, f) + wave_of(K.kb2 * b, f);
            const float wa = wave_of(K.ka1 * a, f) + wave_of(K.ka2 * a, f);
            const float g = Wx[v] * A1;
            ox[v] = px[v] + wb * g * nx[v];
            oy[v] = py[v] + wa * (1.0f - Wz[v]) * A2;
            oz[v] = pz[v] + wb * g * ny[v];
        }
        if (!all_finite(ox, oy, oz, V)) return kNonFinite;
        out_nx_ = normal_.lane[0]; out_ny_ = normal_.lane[1]; out_nz_ = normal_.lane[2];
    } else if (kind == kRipple) {
        /* noninstanced-deformation.md, group 4, with o = frac(t.x + t.y + t.z). */
        if ((r = lookup(kSemNormal, normal_, true)) != kHandled) return r;
        if ((r = lookup(kSemWeights, weight_, true)) != kHandled) return r;
        if ((r = lookup(kSemTangent, tangent_, true)) != kHandled) return r;
        float M[3][4];
        for (int row = 0; row < 3; ++row) for (int l = 0; l < 4; ++l) M[row][l] = bef32(u + kM0 + 16 * row + 4 * l);
        const float theta = bef32(u + kPhase);
        const float T[3] = {bef32(u + kTranslation), bef32(u + kTranslation + 4), bef32(u + kTranslation + 8)};
        for (float f : {M[0][0], M[0][1], M[0][2], M[0][3], M[1][0], M[1][1], M[1][2], M[2][0], M[2][1], M[2][2],
                        theta, T[0], T[1], T[2]})
            if (!finite(f)) return kNonFinite;
        const float off = frac(T[0] + T[1] + T[2]);
        const float r1 = 1.0f / M[1][2], r2 = 1.0f / M[2][2];
        if (!finite(r1) || !finite(r2)) return kNonFinite;
        out_x_.resize(V); out_y_.resize(V); out_z_.resize(V);
        const float* nx = normal_.lane[0].data(); const float* ny = normal_.lane[1].data(); const float* nz = normal_.lane[2].data();
        const float* tx = tangent_.lane[0].data(); const float* ty = tangent_.lane[1].data();
        const float* tz = tangent_.lane[2].data(); const float* th = tangent_.lane[3].data();
        const float* Wx = weight_.lane[0].data(); const float* Wy = weight_.lane[1].data(); const float* Wz = weight_.lane[2].data();
        float* ox = out_x_.data(); float* oy = out_y_.data(); float* oz = out_z_.data();
        auto ripple = [](float phase) { return 2.0f * smooth(std::fabs(2.0f * frac(phase) - 1.0f)) - 1.0f; };
        for (uint32_t v = 0; v < V; ++v) {
            /* Bitangent h·(N × t), then the two ripple directions. */
            const float bx = th[v] * (ny[v] * tz[v] - nz[v] * ty[v]);
            const float by = th[v] * (nz[v] * tx[v] - nx[v] * tz[v]);
            const float bz = th[v] * (nx[v] * ty[v] - ny[v] * tx[v]);
            const float d1x = M[0][0] * tx[v] + M[0][1] * bx, d1y = M[0][0] * ty[v] + M[0][1] * by, d1z = M[0][0] * tz[v] + M[0][1] * bz;
            const float d2x = M[0][2] * tx[v] + M[0][3] * bx, d2y = M[0][2] * ty[v] + M[0][3] * by, d2z = M[0][2] * tz[v] + M[0][3] * bz;
            const float phi1 = r1 * (px[v] * d1x + py[v] * d1y + pz[v] * d1z) + off - M[1][1] * theta + Wz[v] + 0.5f;
            const float phi2 = r2 * (px[v] * d2x + py[v] * d2y + pz[v] * d2z) + off - M[2][1] * theta + Wz[v] + 0.5f;
            const float D = ripple(phi1) * M[1][0] * Wx[v] + ripple(phi2) * M[2][0] * Wy[v];
            ox[v] = px[v] + D * nx[v]; oy[v] = py[v] + D * ny[v]; oz[v] = pz[v] + D * nz[v];
        }
        if (!all_finite(ox, oy, oz, V)) return kNonFinite;
        out_nx_ = normal_.lane[0]; out_ny_ = normal_.lane[1]; out_nz_ = normal_.lane[2];
        out_tx_ = tangent_.lane[0]; out_ty_ = tangent_.lane[1]; out_tz_ = tangent_.lane[2];
    } else if (kind == kSkinned) {
        if ((r = lookup(kSemNormal, normal_, false)) != kHandled) return r;
        if ((r = lookup(kSemTangent, tangent_, false)) != kHandled) return r;
        const Stream& s0 = in.slot[kSlotPalette0];
        const Stream& s1 = in.slot[kSlotPalette1];
        const Stream& inf = in.slot[kSlotInfluences];
        if (inf.size < 8u * V || (V && !inf.data)) return kStreamShort;
        const uint32_t total = s0.size + s1.size, J = total / 48;
        if (J == 0) return kKind;
        std::vector<float>& palette = out_tw_;   /* reused scratch: J·12 floats */
        palette.resize(size_t(J) * 12);
        for (uint32_t w = 0; w < J * 12; ++w) {
            uint8_t b[4];
            for (unsigned k = 0; k < 4; ++k) {
                const uint32_t at = 4 * w + k;
                b[k] = at < s0.size ? s0.data[at] : s1.data[at - s0.size];
            }
            palette[w] = bef32(b);
            if (!finite(palette[w])) return kNonFinite;
        }
        out_x_.resize(V); out_y_.resize(V); out_z_.resize(V);
        const bool has_n = normal_.present, has_t = tangent_.present;
        /* Normals and tangents are then turned by O = σ·R(q), the stored quaternion used
         * literally (skinning-deformation.md, steps 5 and 6). */
        const float oq[4] = {bef32(u + kQuat), bef32(u + kQuat + 4), bef32(u + kQuat + 8), bef32(u + kQuat + 12)};
        const float os = bef32(u + kScale);
        for (float f : {oq[0], oq[1], oq[2], oq[3], os, bef32(u + kTranslation), bef32(u + kTranslation + 4),
                        bef32(u + kTranslation + 8)})
            if (!finite(f)) return kNonFinite;
        const float qx = oq[0], qy = oq[1], qz = oq[2], qw = oq[3];
        const float O[3][3] = {
            {os * (1 - 2 * (qy * qy + qz * qz)), os * (2 * (qx * qy - qz * qw)), os * (2 * (qx * qz + qy * qw))},
            {os * (2 * (qx * qy + qz * qw)), os * (1 - 2 * (qx * qx + qz * qz)), os * (2 * (qy * qz - qx * qw))},
            {os * (2 * (qx * qz - qy * qw)), os * (2 * (qy * qz + qx * qw)), os * (1 - 2 * (qx * qx + qy * qy))}};
        if (has_n) { out_nx_.resize(V); out_ny_.resize(V); out_nz_.resize(V); }
        if (has_t) { out_tx_.resize(V); out_ty_.resize(V); out_tz_.resize(V); }
        for (uint32_t v = 0; v < V; ++v) {
            const uint8_t* rec = inf.data + 8 * v;
            float B[12] = {};
            for (unsigned k = 0; k < 4; ++k) {
                const uint8_t j = rec[2 * k + 1];
                if (j >= J) return kJoint;
                /* A zero weight adds signed zeros only, which leaves the blend unchanged. */
                if (!rec[2 * k]) continue;
                const float w = float(rec[2 * k]) / 255.0f;
                const float* m = palette.data() + 12 * j;
                for (unsigned e = 0; e < 12; ++e) B[e] += w * m[e];
            }
            const float x = px[v], y = py[v], z = pz[v];
            out_x_[v] = B[0] * x + B[1] * y + B[2] * z + B[3];
            out_y_[v] = B[4] * x + B[5] * y + B[6] * z + B[7];
            out_z_[v] = B[8] * x + B[9] * y + B[10] * z + B[11];
            if (!finite(out_x_[v]) || !finite(out_y_[v]) || !finite(out_z_[v])) return kNonFinite;
            if (has_n) {
                const float a = normal_.lane[0][v], b = normal_.lane[1][v], c = normal_.lane[2][v];
                const float n0 = B[0] * a + B[1] * b + B[2] * c, n1 = B[4] * a + B[5] * b + B[6] * c,
                            n2 = B[8] * a + B[9] * b + B[10] * c;
                out_nx_[v] = O[0][0] * n0 + O[0][1] * n1 + O[0][2] * n2;
                out_ny_[v] = O[1][0] * n0 + O[1][1] * n1 + O[1][2] * n2;
                out_nz_[v] = O[2][0] * n0 + O[2][1] * n1 + O[2][2] * n2;
            }
            if (has_t) {
                const float a = tangent_.lane[0][v], b = tangent_.lane[1][v], c = tangent_.lane[2][v];
                const float t0 = B[0] * a + B[1] * b + B[2] * c, t1 = B[4] * a + B[5] * b + B[6] * c,
                            t2 = B[8] * a + B[9] * b + B[10] * c;
                out_tx_[v] = O[0][0] * t0 + O[0][1] * t1 + O[0][2] * t2;
                out_ty_[v] = O[1][0] * t0 + O[1][1] * t1 + O[1][2] * t2;
                out_tz_[v] = O[2][0] * t0 + O[2][1] * t1 + O[2][2] * t2;
            }
        }
    }

    /* Sections 4 and 5. */
    const bool produced = kind != kIndexOnly;
    r = project_and_cull(in, V, I, produced ? out_x_.data() : px, produced ? out_y_.data() : py,
                         produced ? out_z_.data() : pz, produced ? Vout : V, flavour);
    if (r != kHandled) return r;
    out.surviving = uint32_t(kept_.size());

    /* Section 6 sizes and section 7 data. */
    if (out.surviving) {
        out.index_bytes = 2 * roundup(out.surviving, 8);
        out.vertex_bytes = produced ? roundup(out.stride * Vout, 16) : 0;
        out.request = roundup(out.index_bytes + out.vertex_bytes, 128);
        if ((r = encode_vertices(out)) != kHandled) return r;
    } else if (produced) {
        /* Nothing is drawn, but the layout must still be one this implementation encodes. */
        out.index_bytes = 0;
        if ((r = encode_vertices(out)) != kHandled) return r;
        data_.clear();
    }
    /* Check 9 (with placeholder placement; the word count does not depend on it). */
    uint32_t words[1024];
    if (out.hole_bytes / 4 > 1024 || layout_hole(out, 0, 0, 0xB0000000u, words, out.hole_bytes / 4) > out.hole_bytes / 4)
        return kHoleOverflow;
    out.data = data_.data();
    return kHandled;
}

Reason Engine::encode_vertices(Prepared& out) {
    const uint32_t Vout = out.produced, stride = out.stride;
    const bool sized = out.surviving != 0;
    for (unsigned d = 0; d < out.declaration_count; ++d) {
        const OutputDeclaration& decl = out.declarations[d];
        unsigned bytes;
        if (decl.semantic == kSemPosition && decl.format == kFormatFloat && decl.count == 3) bytes = 12;
        else if (decl.semantic == kSemNormal && decl.format == kFormatCmp && decl.count == 1 &&
                 (out.kind == kWind || out.kind == kRipple || (out.kind == kSkinned && normal_.present))) bytes = 4;
        else if (decl.semantic == kSemTangent && decl.format == kFormatHalf && decl.count == 4 &&
                 (out.kind == kSkinned || out.kind == kRipple) && tangent_.present) bytes = 8;
        else if (decl.semantic == kSemWeights && decl.format == kFormatUByte && decl.count == 4 &&
                 (out.kind == kWind || out.kind == kRipple) && weight_.codec == 4) bytes = 4;
        else return kOutputFormat;
        if (decl.offset + bytes > stride) return kOutputFormat;
    }
    if (!sized) return kHandled;
    data_.assign(size_t(out.index_bytes) + out.vertex_bytes, 0);
    uint8_t* p = data_.data();
    for (uint32_t i = 0; i < out.surviving; ++i) put16(p + 2 * i, kept_[i]);
    uint8_t* vtx = p + out.index_bytes;
    for (unsigned d = 0; d < out.declaration_count; ++d) {
        const OutputDeclaration& decl = out.declarations[d];
        uint8_t* q = vtx + decl.offset;
        if (decl.semantic == kSemPosition) {
            for (uint32_t v = 0; v < Vout; ++v, q += stride) {
                putf32(q, out_x_[v]); putf32(q + 4, out_y_[v]); putf32(q + 8, out_z_[v]);
            }
        } else if (decl.semantic == kSemNormal) {
            for (uint32_t v = 0; v < Vout; ++v, q += stride) put32(q, pack_normal_cmp(out_nx_[v], out_ny_[v], out_nz_[v]));
        } else if (decl.semantic == kSemTangent) {
            const float* tw = tangent_.lane[3].data();
            for (uint32_t v = 0; v < Vout; ++v, q += stride) {
                put16(q, to_binary16(out_tx_[v])); put16(q + 2, to_binary16(out_ty_[v]));
                put16(q + 4, to_binary16(out_tz_[v])); put16(q + 6, to_binary16(tw[v]));
            }
        } else {
            const uint8_t* src = weight_.raw;
            for (uint32_t v = 0; v < Vout; ++v, q += stride, src += weight_.raw_stride) std::memcpy(q, src, 4);
        }
    }
    return kHandled;
}

}  // namespace mag_edgegeom
