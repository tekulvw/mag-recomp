/* Synthetic checks of the native Edge geometry core against the specification's test vectors
 * (docs/render-frontend/behavior/edge-geometry-job.md ET-1..ET-9, and vectors of the codec and
 * deformation specifications it cites). Every input is generated here; no title data. */
#include "native_edgegeom_cases.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

static int failures;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } \
    } while (0)

static Reason run(const Case& c, Prepared& p) { static Engine engine; return engine.prepare(c.inputs(), p); }

static void et1_facing() {
    std::vector<Vertex> vs = {at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)};
    Case c; c.positions(vs); c.counts(3, 6); c.indices({0, 1, 2, 0, 2, 1});
    Prepared p;
    CHECK(run(c, p) == kHandled && p.kind == kIndexOnly);
    CHECK((kept(p) == std::vector<uint16_t>{0, 1, 2}));
    Case w = wind_case(vs, {0, 1, 2, 0, 2, 1});
    CHECK(run(w, p) == kHandled && p.kind == kWind);
    CHECK((kept(p) == std::vector<uint16_t>{0, 1, 2, 0, 2, 1}));
}

static void et2_frustum() {
    Case c; c.positions({at(1.5f, 0, 0), at(2, 0, 0), at(1.5f, 0.1f, 0)}); c.counts(3, 3); c.indices({0, 1, 2});
    Prepared p;
    CHECK(run(c, p) == kHandled && p.surviving == 0 && p.request == 0);
    c.positions({at(0.9f, 0, 0), at(2, 0, 0), at(1.5f, 0.1f, 0)});
    CHECK(run(c, p) == kHandled && p.surviving == 3);
}

/* Near plane (Sabotage shadow-cascade recordings): a triangle with every vertex past the near
 * plane (c.z < -c.w) is removed; one that only reaches past it is kept without the facing and
 * small-primitive rules, like a triangle with a vertex at w <= 0. */
static void near_plane() {
    Case c; c.positions({at(0, 0, -1.5f), at(0.1f, 0, -1.5f), at(0, 0.1f, -1.5f)}); c.counts(3, 3); c.indices({0, 1, 2});
    Prepared p;
    CHECK(run(c, p) == kHandled && p.surviving == 0);
    c.positions({at(0, 0, -1.5f), at(0.1f, 0, 0), at(0, 0.1f, 0)}); c.indices({0, 2, 1});   /* back-facing */
    CHECK(run(c, p) == kHandled && p.surviving == 3);
    c.positions({at(0, 0, -0.5f), at(0.1f, 0, 0), at(0, 0.1f, 0)});                       /* in front: facing applies */
    CHECK(run(c, p) == kHandled && p.surviving == 0);
}

/* ET-10: a triangle reaching behind the camera. Viewport sides are tested on positions divided
 * by |c.w|; a surviving triangle with a vertex behind skips the facing and small-primitive rules.
 * C maps p directly to clip coordinates, with w = z + 0.2. */
static void et10_behind_camera() {
    struct Case10 { float front_x0, front_x1, x0; uint16_t want; } cases[] = {
        {2, 2.5f, 0, 3}, {2, 2.5f, 3.5f, 0}, {-2, -2.5f, 0.5f, 0}, {-2, -2.5f, 1.5f, 3}};
    for (const auto& k : cases) {
        Case c; c.counts(3, 3); c.indices({0, 1, 2});
        for (int r = 0; r < 4; ++r) for (int q = 0; q < 4; ++q) putf(c.slot[17], 0x10 + 16 * r + 4 * q, 0.0f);
        putf(c.slot[17], 0x10, 1.0f); putf(c.slot[17], 0x24, 1.0f); putf(c.slot[17], 0x38, 1.0f);
        putf(c.slot[17], 0x48, 1.0f); putf(c.slot[17], 0x4C, 0.2f);
        c.positions({at(k.front_x0, 0.5f, 0.8f), at(k.front_x1, -0.5f, 0.8f), at(k.x0, 0, -1.2f)});
        Prepared p;
        CHECK(run(c, p) == kHandled && p.surviving == k.want);
    }
}

static void et3_small_primitive() {
    Case c; c.positions({at(0, 0, 0), at(0.0003f, -0.1f, 0), at(0.0001f, 0.1f, 0)}); c.counts(3, 3); c.indices({0, 1, 2});
    Prepared p;
    CHECK(run(c, p) == kHandled && p.surviving == 0);
    c.positions({at(0, 0, 0), at(0.0005f, -0.1f, 0), at(0.0001f, 0.1f, 0)});
    CHECK(run(c, p) == kHandled && p.surviving == 3);
}

/* Group 3 with produced vertices under mode 0x08, flavour 2: the group-3 wind, culled back-face. */
static void group3_mode8_wind() {
    Vertex v0 = at(1, -1, 0); v0.n[0] = 0.5f; v0.n[1] = -1; v0.n[2] = 0.25f;
    v0.w[0] = 1; v0.w[1] = 0; v0.w[2] = 128.0f / 255.0f;
    Vertex v1 = at(0.1f, 0, 0), v2 = at(0, 0.1f, 0);
    v1.w[2] = 1; v2.w[2] = 1;
    Case c = wind_case({v0, v1, v2}, {0, 1, 2, 0, 2, 1});
    put32(c.userdata, 0xAC, 0x08); c.userdata[0xA5] = 2;
    putf(c.userdata, 0x20, 0.5f); putf(c.userdata, 0x24, 1.0f); putf(c.userdata, 0x30, 0.5f);
    putf(c.userdata, 0x80, 1.0f); putf(c.userdata, 0x88, 1.0f); putf(c.userdata, 0x8C, 0.25f);
    Prepared p;
    CHECK(run(c, p) == kHandled && p.kind == kWind && p.surviving == 3);
    const uint8_t* v = p.data + p.index_bytes;
    CHECK(near(getf(v), 1.5f) && near(getf(v + 8), -1));
}

static void et4_zero_area() {
    Case w = wind_case({at(0, 0, 0), at(0.1f, 0, 0), at(0.2f, 0, 0)}, {0, 1, 2});
    Prepared p;
    CHECK(run(w, p) == kHandled && p.surviving == 0);
}

static void et5_ring() {
    RingRecord r{0xC1000000u, 0x01000000u, 0xC1010000u, 0xC100FF00u, 0, 0xB00005D0u, 0xC1008000u};
    uint32_t x = 0, u = 0;
    CHECK(allocate(r, 0x200, nullptr, &x, &u) == Allocation::kPlaced && x == 0xC1000000u && u == 0xC1008000u);
    r.consumed = 0xC1000100u;
    CHECK(allocate(r, 0x200, nullptr, &x, &u) == Allocation::kNeedsRefresh);
    const uint32_t fresh = 0xC100F000u;
    CHECK(allocate(r, 0x200, &fresh, &x, &u) == Allocation::kPlaced && x == 0xC1000000u && u == fresh);
    /* Placed below U the allocation must end strictly before it (open question 4). */
    const uint32_t exact = 0xC1000200u;
    CHECK(allocate(r, 0x200, &exact, &x, &u) == Allocation::kFull);
    /* No wrap needed: X = T. */
    r = RingRecord{0xC1000000u, 0, 0xC1010000u, 0xC1001000u, 0, 0xB0000000u, 0xC1000000u};
    CHECK(allocate(r, 0x80, nullptr, &x, &u) == Allocation::kPlaced && x == 0xC1001000u);
    /* U above T: only [T, U) is free. */
    r.consumed = 0xC1001080u;
    CHECK(allocate(r, 0x80, nullptr, &x, &u) == Allocation::kNeedsRefresh);
    r.consumed = 0xC1001100u;
    CHECK(allocate(r, 0x80, nullptr, &x, &u) == Allocation::kPlaced && x == 0xC1001000u);
    /* The record write-back changes only T and U. */
    uint8_t line[128];
    for (int i = 0; i < 128; ++i) line[i] = uint8_t(i);
    write_ring(line, 0xC1000200u, 0xC100F000u);
    CHECK(get32(line + 12) == 0xC1000200u && get32(line + 24) == 0xC100F000u && line[16] == 16 && line[127] == 127);
}

static std::vector<uint32_t> hole_words(const Prepared& p, uint32_t x, uint32_t off, uint32_t label) {
    std::vector<uint8_t> bytes(p.hole_bytes);
    Engine::build_hole(p, x, off, label, bytes.data());
    std::vector<uint32_t> w;
    for (size_t i = 0; i < bytes.size(); i += 4) w.push_back(get32(&bytes[i]));
    return w;
}

static void et6_et7_et8_holes() {
    Prepared p; p.hole_address = 0x70000000; p.hole_bytes = 112;
    std::vector<uint32_t> want = {0x00041710, 0, 0x00041714, 0, 0x00041714, 0, 0x00041714, 0};
    want.resize(28, 0);
    CHECK(hole_words(p, 0, 0, 0xB0000000u) == want);

    p.surviving = 36; p.request = 128; p.index_bytes = 80;
    const std::vector<uint32_t> et7 = {
        0x00041710, 0x00000000, 0x00041714, 0x00000000, 0x00041714, 0x00000000, 0x00041714, 0x00000000,
        0x00041714, 0x00000000, 0x0008181C, 0x0F000000, 0x00000010, 0x00041808, 0x00000005, 0x40041824,
        0x23000000, 0x00041808, 0x00000000, 0x00041D6C, 0x000005D0, 0x00041D74, 0xCF000080, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000};
    CHECK(hole_words(p, 0xCF000000u, 0x0F000000u, 0xB00005D0u) == et7);

    p.hole_address = 0x70000050; p.hole_bytes = 128;
    std::vector<uint32_t> w = hole_words(p, 0xCF000000u, 0x0F000000u, 0xB00005D0u);
    CHECK(w[10] == 0x0004181C && w[11] == 0x0F000000 && w[12] == 0x00041820 && w[13] == 0x00000010);
    p.hole_address = 0x70000054;
    w = hole_words(p, 0xCF000000u, 0x0F000000u, 0xB00005D0u);
    CHECK(w[10] == 0 && w[11] == 0x0008181C && w[12] == 0x0F000000 && w[13] == 0x00000010);

    /* More than 256 indices: one non-incrementing packet of several batches. */
    p.hole_address = 0x70000000; p.hole_bytes = 128; p.surviving = 300; p.index_bytes = 608; p.request = 640;
    w = hole_words(p, 0xCF000000u, 0x0F000000u, 0xB00005D0u);
    CHECK(w[15] == 0x40081824 && w[16] == 0xFF000000u && w[17] == ((43u << 24) | 256u));

    /* Produced vertices announce each declaration's format and offset first. */
    p.surviving = 3; p.index_bytes = 16; p.request = 128; p.stride = 12; p.declaration_count = 1;
    p.declarations[0] = OutputDeclaration{0, 2, 3, 1, 0};
    w = hole_words(p, 0xCF000000u, 0x0F000000u, 0xB0000010u);
    CHECK(w[8] == 0x00041740 && w[9] == 0x0C32 && w[10] == 0x00041680 && w[11] == 0x0F000010 && w[12] == 0x00041714);
}

static void et9_instances() {
    Case c(0x28, 1, 0);
    std::vector<Vertex> vs = {at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)};
    c.attributes(vs);
    c.slot[19][4] = 0x00;                                    /* built-in layout 0 */
    c.counts(3, 3); c.indices({0, 1, 2});
    c.userdata[0xB0] = 2;
    c.slot[13].assign(32, 0);
    for (int i = 0; i < 2; ++i) { put16(c.slot[13], 16 * i + 6, 0x3C00); put16(c.slot[13], 16 * i + 14, 0x3C00); }
    Prepared p;
    CHECK(run(c, p) == kHandled && p.kind == kInstancedWind);
    CHECK((kept(p) == std::vector<uint16_t>{0, 1, 2, 3, 4, 5}));
    CHECK(p.produced == 6 && p.vertex_bytes == 80 && p.index_bytes == 16 && p.request == 128);
    CHECK(p.declaration_count == 1 && p.stride == 12);
}

/* group-zero-deformation.md GZ-1 (placement and rotation) through the whole job. */
static void gz1_placement() {
    Case c(0x28, 1, 0);
    c.attributes({at(1, 0, 0), at(0, 2, 0), at(0, 0, 0)});
    c.slot[19][4] = 0x00;
    c.counts(3, 3); c.indices({0, 1, 2});
    c.userdata[0xB0] = 2;
    c.slot[13].assign(32, 0);
    put16(c.slot[13], 0, 0x4200);                            /* o = (3, 0, 0) */
    put16(c.slot[13], 6, 0x3C00); put16(c.slot[13], 14, 0x3C00);
    put16(c.slot[13], 16 + 4, 0xBC00);                       /* o = (0, 0, -1) */
    put16(c.slot[13], 16 + 6, 0x3800);                       /* sigma 0.5 */
    put16(c.slot[13], 16 + 12, 0x3C00); put16(c.slot[13], 16 + 14, 0x3C00);   /* q = (0, 0, 1, 1) */
    Prepared p;
    CHECK(run(c, p) == kHandled && p.surviving == 3);
    const float want[6][3] = {{4, 0, 0}, {3, 2, 0}, {3, 0, 0}, {0, 0.5f, -1}, {-1, 0, -1}, {0, 0, -1}};
    const uint8_t* v = p.data + p.index_bytes;
    for (int i = 0; i < 6; ++i)
        for (int k = 0; k < 3; ++k) CHECK(near(getf(v + 12 * i + 4 * k), want[i][k]));
    CHECK((kept(p) == std::vector<uint16_t>{3, 4, 5}));
}

/* GZ-3: wind whose phases do not depend on the coefficients. */
static void gz3_wind() {
    for (int row = 0; row < 2; ++row) {
        Case c(0x28, 1, 0);
        Vertex v0 = at(0, 0, 0); v0.n[0] = 1; v0.n[1] = 2; v0.n[2] = 3; v0.w[3] = 0.5f;
        Vertex v1 = at(0.1f, 0, 0), v2 = at(0, 0.1f, 0);
        c.attributes({v0, v1, v2});
        c.slot[19][4] = 0x00; c.counts(3, 3); c.indices({0, 1, 2});
        c.slot[13].assign(16, 0); put16(c.slot[13], 6, 0x3C00); put16(c.slot[13], 14, 0x3C00);
        putf(c.userdata, 0x20, 0.5f); putf(c.userdata, 0x24, 1.0f);          /* M0 */
        putf(c.userdata, 0x30, row ? 1.0f : 0.5f);                           /* M1.x */
        putf(c.userdata, 0x80, 1.0f); putf(c.userdata, 0x88, 0.5f); putf(c.userdata, 0x8C, 0.5f);
        Prepared p;
        CHECK(run(c, p) == kHandled && p.surviving == 3);
        const uint8_t* v = p.data + p.index_bytes;
        const float want = row ? 0.0f : 0.375f;
        CHECK(near(getf(v), want) && near(getf(v + 4), 2 * want) && near(getf(v + 8), want));
    }
}

/* noninstanced-deformation.md NI-1, through the group-3 job. */
static void ni1_wind() {
    Vertex v0 = at(1, -1, 0); v0.n[0] = 0.5f; v0.n[1] = -1; v0.n[2] = 0.25f;
    v0.w[0] = 1; v0.w[1] = 0; v0.w[2] = 128.0f / 255.0f;
    Vertex v1 = at(0.1f, 0, 0), v2 = at(0, 0.1f, 0);
    v1.w[2] = 1; v2.w[2] = 1;                                 /* those two stay in place */
    for (int row = 0; row < 2; ++row) {
        Case c = wind_case({v0, v1, v2}, {0, 1, 2});
        putf(c.userdata, 0x20, 0.5f); putf(c.userdata, 0x24, 1.0f);
        putf(c.userdata, 0x30, row ? 1.0f : 0.5f);
        putf(c.userdata, 0x80, 1.0f); putf(c.userdata, 0x88, 1.0f); putf(c.userdata, 0x8C, 0.25f);
        Prepared p;
        CHECK(run(c, p) == kHandled && p.surviving == 3 && p.stride == 20);
        const uint8_t* v = p.data + p.index_bytes;
        const float wy = 2.0f * (1.0f - 128.0f / 255.0f) * 0.25f;
        if (row == 0) CHECK(near(getf(v), 1.5f) && near(getf(v + 4), -1 + wy) && near(getf(v + 8), -1));
        else CHECK(near(getf(v), 1) && near(getf(v + 4), -1) && near(getf(v + 8), 0));
        /* Weights are the input bytes; the normal is the packed input normal. */
        CHECK(v[16] == 255 && v[17] == 0 && v[18] == 128 && v[19] == 255);
        CHECK(get32(v + 12) == pack_normal_cmp(0.5f, -1, 0.25f));
        CHECK(near(getf(v + 20), 0.1f) && near(getf(v + 40 + 4), 0.1f));
    }
}

static Case skinned_case(const std::vector<Vertex>& vs, std::initializer_list<uint16_t> idx,
                         const std::vector<std::vector<float>>& matrices, const std::vector<uint8_t>& influences) {
    Case c(0x08, 2, 7);
    c.slot[19][0] = 0x83; c.slot[19][6] = 0x11;
    c.attributes(vs);
    c.layout({{0, 2, 3, 1, 0}, {12, 6, 1, 2, 2}, {16, 3, 4, 3, 14}}, 24);
    c.hole(15);
    c.counts(uint16_t(vs.size()), uint16_t(idx.size())); c.indices(idx);
    c.slot[21].assign(8, 0);
    c.slot[3].assign(matrices.size() * 48, 0);
    for (size_t m = 0; m < matrices.size(); ++m) for (int e = 0; e < 12; ++e) putf(c.slot[3], 48 * m + 4 * e, matrices[m][e]);
    c.slot[5].assign(((influences.size() + 15) / 16) * 16, 0);
    std::memcpy(c.slot[5].data(), influences.data(), influences.size());
    /* Bring the produced triangle into view: object scale 0.1, translation (-0.1, -0.3, 0). */
    putf(c.userdata, 0x7C, 0.1f); putf(c.userdata, 0x70, -0.1f); putf(c.userdata, 0x74, -0.3f);
    return c;
}

/* skinning-deformation.md SK-1, SK-2 and SK-7 (no object direction transform in this job). */
static void sk_skinning() {
    const std::vector<float> rot = {0, -1, 0, 1, 1, 0, 0, 2, 0, 0, 1, 3};
    Vertex v0 = at(1, 0, 0); v0.n[0] = 1; v0.t[0] = 0; v0.t[1] = 1; v0.t[3] = -1;
    Vertex v1 = at(1, -0.5f, 0), v2 = at(0.5f, 0, 0);
    const std::vector<uint8_t> one = {255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0};
    Case c = skinned_case({v0, v1, v2}, {0, 1, 2, 0, 2, 1}, {rot}, one);
    Prepared p;
    CHECK(run(c, p) == kHandled && p.kind == kSkinned);
    CHECK((kept(p) == std::vector<uint16_t>{0, 2, 1}));
    const uint8_t* v = p.data + p.index_bytes;
    CHECK(near(getf(v), 1) && near(getf(v + 4), 3) && near(getf(v + 8), 3));
    /* Normals and tangents are then turned by O = σ·R(q): here σ = 0.1 and q = identity. */
    CHECK(get32(v + 12) == pack_normal_cmp(0, 0.1f, 0));
    CHECK(get16(v + 16) == to_binary16(-0.1f) && (get16(v + 18) & 0x7FFF) == 0 && (get16(v + 20) & 0x7FFF) == 0 && get16(v + 22) == 0xBC00);

    /* Group 11, and a non-identity object rotation (90° about z): positions are not turned, but
     * normals and tangents are (skinning-deformation.md SK-5 with σ = 0.1). */
    Case g = skinned_case({v0, v1, v2}, {0, 1, 2, 0, 2, 1}, {rot}, one);
    g.userdata[0xA7] = 11;
    putf(g.userdata, 0x68, std::sqrt(0.5f)); putf(g.userdata, 0x6C, std::sqrt(0.5f));
    CHECK(run(g, p) == kHandled && p.kind == kSkinned && p.surviving == 3);
    v = p.data + p.index_bytes;
    CHECK(near(getf(v), 1) && near(getf(v + 4), 3) && near(getf(v + 8), 3));
    CHECK(get32(v + 12) == pack_normal_cmp(-0.1f, 0, 0));
    CHECK(get16(v + 16) == 0 || get16(v + 16) == 0x8000 || (get16(v + 16) & 0x7FFF) < 4);
    CHECK(get16(v + 18) == to_binary16(-0.1f) && get16(v + 22) == 0xBC00);
    /* An empty palette is refused. */
    Case e = skinned_case({v0, v1, v2}, {0, 1, 2}, {}, one);
    CHECK(run(e, p) == kKind);

    const std::vector<float> ident = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const std::vector<float> twice = {2, 0, 0, 4, 0, 2, 0, 0, 0, 0, 2, 0};
    Vertex b0 = at(1, 1, 1); b0.n[2] = 1;
    std::vector<uint8_t> blend = {51, 0, 204, 1, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0};
    Case b = skinned_case({b0, at(2.0f, 3.0f, 0), at(1.0f, 3.5f, 0)}, {0, 1, 2, 0, 2, 1}, {ident, twice}, blend);
    putf(b.userdata, 0x70, -0.5f); putf(b.userdata, 0x74, -0.25f);
    CHECK(run(b, p) == kHandled && p.surviving == 3);
    v = p.data + p.index_bytes;
    CHECK(near(getf(v), 5) && near(getf(v + 4), 1.8f) && near(getf(v + 8), 1.8f));

    blend[3] = 1; blend[2] = 0;   /* joint 1 with weight 0 and a one-matrix palette */
    Case j = skinned_case({b0, at(2.0f, 3.0f, 0), at(1.0f, 3.5f, 0)}, {0, 1, 2}, {ident}, blend);
    CHECK(run(j, p) == kJoint);
}

/* A stream longer than 16 KiB arrives as slot 1 (or 7) holding its first 0x4000 bytes and slot 2
 * (or 8) holding the rest, contiguous in guest memory and in the buffer. */
static void continuation_slots() {
    Case c; c.positions({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)});
    const uint16_t K = 8196;
    c.counts(3, K); c.hole(15);
    std::vector<uint8_t> blob(0x4000 + 16, 0);
    for (uint32_t i = 0; i < K; ++i) put16(blob, 2 * i, uint16_t(i % 3));
    Inputs in = c.inputs();
    in.slot[1] = Stream{blob.data(), 0x4000, 0x30000000};
    in.slot[2] = Stream{blob.data() + 0x4000, 16, 0x30004000};
    Prepared p;
    static Engine engine;
    CHECK(engine.prepare(in, p) == kHandled && p.surviving == K);
    /* Not contiguous in guest memory: not a continuation, so the index stream is short. */
    in.slot[2].address = 0x30005000;
    CHECK(engine.prepare(in, p) == kStreamShort);
}

static void refusals() {
    auto base = [] { Case c; c.positions({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)}); c.counts(3, 3); c.indices({0, 1, 2}); return c; };
    Prepared p;
    Case c = base(); CHECK(run(c, p) == kHandled);
    c = base(); put32(c.userdata, 0xAC, 0x18); CHECK(run(c, p) == kMode);
    c = base(); c.userdata[0xA5] = 1; CHECK(run(c, p) == kMode);
    c = base(); c.userdata[0xA4] = 1; CHECK(run(c, p) == kBlendShapes);
    c = base(); c.slot[19][7] = 1; CHECK(run(c, p) == kConfiguration);
    c = base(); c.slot[19][12] = 0; CHECK(run(c, p) == kConfiguration);
    c = base(); put32(c.userdata, 0x9C, 8); CHECK(run(c, p) == kHole);
    c = base(); put32(c.userdata, 0x98, 0x70000008); CHECK(run(c, p) == kHole);
    c = base(); c.indices({0, 1, 3}); CHECK(run(c, p) == kIndexRange);
    /* A stream shorter than its counts is refused, whatever the kind (spec 2f4a283). */
    c = base(); c.counts(3, 9); CHECK(run(c, p) == kStreamShort);
    c = base(); c.counts(5, 3); CHECK(run(c, p) == kStreamShort);
    Case sw = wind_case({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)}, {0, 1, 2}); sw.counts(9, 3);
    CHECK(run(sw, p) == kStreamShort);
    /* Index only applies whatever the group. */
    c = base(); c.userdata[0xA7] = 12; CHECK(run(c, p) == kHandled && p.kind == kIndexOnly);
    c = base(); c.userdata[0xA7] = 4; CHECK(run(c, p) == kHandled && p.kind == kIndexOnly);
    /* Produced vertices for a group without a specified production rule. */
    c = base(); c.userdata[0xA7] = 1; c.layout({{0, 2, 3, 1, 0}}, 12); c.hole(15); CHECK(run(c, p) == kKind);
    c = base(); c.slot[19][6] = 0x11; c.userdata[0xA7] = 11; CHECK(run(c, p) == kKind);
    c = base(); putf(c.slot[7], 4, std::nanf("")); CHECK(run(c, p) == kNonFinite);
    c = base(); putf(c.slot[17], 0x10, INFINITY); CHECK(run(c, p) == kNonFinite);
    c = base(); c.slot[19][1] = 1; put32(c.userdata, 0x9C, 1); CHECK(run(c, p) == kHoleOverflow);
    c = base(); c.layout({{0, 2, 3, 1, 0}}, 12); CHECK(run(c, p) == kKind);
    /* Ripple (group 4) stays refused until confirmed. */
    Case r = wind_case({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)}, {0, 1, 2}); r.userdata[0xA7] = 4;
    CHECK(run(r, p) == kKind);
    /* Codec 8 position: undecodable. */
    Case k = wind_case({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)}, {0, 1, 2}); k.slot[20][8 + 1] = 8;
    CHECK(run(k, p) == kCodec);
    /* An output declaration this implementation does not encode. */
    Case o = wind_case({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)}, {0, 1, 2});
    o.slot[0][8 + 8 + 1] = 2;   /* normal declared as f32 */
    CHECK(run(o, p) == kOutputFormat);
}

static void codecs() {
    CHECK(rebased_half(0x3C00) == 1.0f && std::signbit(rebased_half(0x8000)) && rebased_half(0x8000) == 0.0f);
    CHECK(rebased_half(0x7C00) == 65536.0f);
    { const float f = rebased_half(0x0001); uint32_t u; std::memcpy(&u, &f, 4); CHECK(u == 0x38002000u); }
    struct N { uint8_t b[3]; float want[4]; } normals[] = {
        {{0x80, 0x12, 0xC5}, {0.000691f, 0.956302f, -0.292381f, -1}},
        {{0xC8, 0x06, 0x4A}, {0.398828f, -0.568865f, -0.719256f, 1}},
        {{0x7F, 0xDF, 0xF0}, {-1.0f, -0.000691f, -0.000691f, -1}},
        {{0x4B, 0x2B, 0xCB}, {-0.292381f, 0.260586f, 0.920113f, 1}}};
    for (const auto& n : normals) {
        float v[4];
        CHECK(spherical_normal(n.b, v));
        for (int k = 0; k < 4; ++k) CHECK(near(v[k], n.want[k], 1e-4f));
    }
    { const uint8_t b[3] = {0, 0, 0x0C}; float v[4]; CHECK(!spherical_normal(b, v)); }
    { const uint8_t b[3] = {0xFF, 0xC0, 0x03}; float v[4]; CHECK(spherical_normal(b, v) && v[0] >= 0 && v[0] <= 0.025f); }
    /* group-zero-deformation.md GZ-4. */
    CHECK(near(wave(0, 1), 0) && near(wave(0.25f, 1), 1) && near(wave(0.125f, 1), 0.5f));
    CHECK(near(wave(-0.375f, 2), 1) && near(wave(1000000.25f, 1), 1));
    CHECK(near(wave(3.7f, 0.3f), 0.145152f) && near(wave(-2.6f, 1.5f), 0.648f));
    /* RSX CMP quantization and binary16. */
    CHECK(pack_normal_cmp(1, 0, 0) == (0x3FFu | 0x7FFu << 11 | 0x3FFu << 22));
    CHECK(pack_normal_cmp(-1, -1, -1) == (0x400u | 0x400u << 11 | 0x200u << 22));
    CHECK(pack_normal_cmp(0, 0, 1) == (0x7FFu | 0x7FFu << 11 | 0x1FFu << 22));
    CHECK(to_binary16(1.0f) == 0x3C00 && to_binary16(-2.0f) == 0xC000 && to_binary16(65504.0f) == 0x7BFF);
    CHECK(to_binary16(6.103515625e-05f) == 0x0400 && to_binary16(1e-8f) == 0 && to_binary16(5.9604645e-08f) == 0x0001);
    CHECK(to_binary16(1.00048828125f) == 0x3C00 && to_binary16(1.00146484375f) == 0x3C02);   /* ties to even */
    /* The game's wind values. */
    CHECK(kWindTuning.ka1 == 0.375f && kWindTuning.ka2 == 0.193f && kWindTuning.kb1 == 1.975f &&
          kWindTuning.kb2 == 0.793f && kWindTuning.kc == 0.33f);
}

int main() {
    codecs();
    et1_facing(); et2_frustum(); et3_small_primitive(); near_plane(); et10_behind_camera(); group3_mode8_wind(); et4_zero_area(); et5_ring();
    et6_et7_et8_holes(); et9_instances();
    gz1_placement(); gz3_wind(); ni1_wind(); sk_skinning();
    continuation_slots();
    refusals();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::puts("PASS native edgegeom core");
    return 0;
}
