#pragma once
/* Native Edge geometry child job: the host-independent core.
 *
 * Written from docs/render-frontend/behavior/edge-geometry-job.md and the specifications it
 * cites (geometry-streams, geometry-normals, group-zero-deformation, noninstanced-deformation,
 * skinning-deformation). It is a pure function of the invocation's inputs: it decides admission
 * (checks 2-9 of the specification), produces and culls the geometry, and lays out the output
 * data, the hole and the ring record. Reading inputs and writing guest memory belong to the
 * caller (edgegeom_runtime.cpp in the engine, or a test). */
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mag_edgegeom {

/* Why an invocation is left to the original job. Stable order: diagnostics print these names. */
enum Reason : uint8_t {
    kHandled = 0,
    kIdentity,        /* check 1: module, entry or policy identity */
    kMode,            /* check 2: mode word and cull flavour */
    kBlendShapes,     /* check 3 */
    kConfiguration,   /* check 4, and configuration bytes outside the observed values */
    kHole,            /* check 5: hole size or address */
    kStreamShort,     /* check 6: an input stream shorter than its counts need */
    kKind,            /* check 7: production rule not supported (group, layout, skinning form) */
    kCodec,           /* check 7: undecodable codec, selector, missing or failed attribute */
    kIndexRange,      /* check 8 */
    kNonFinite,       /* a used input lane or produced value is not finite */
    kJoint,           /* a palette joint index out of range */
    kBudget,          /* produced vertices above 65,536 */
    kOutputFormat,    /* an output declaration this implementation does not encode */
    kHoleOverflow,    /* check 9 */
    kRingFull,        /* check 10 */
    kInputs,          /* the caller could not read or stage the inputs */
    kService,         /* a job-manager service could not be called before admission */
    kForced,          /* MAG_NATIVE_EDGEGEOM_FORCE_REFUSE diagnostic */
    kReasonCount
};
const char* reason_name(Reason reason);

/* Kinds, for per-kind counters. */
enum Kind : uint8_t { kKindNone = 0, kIndexOnly, kInstancedWind, kWind, kSkinned, kRipple, kKindCount };
const char* kind_name(Kind kind);

struct Stream {
    const uint8_t* data = nullptr;
    uint32_t size = 0;      /* bytes available to read */
    uint32_t address = 0;   /* guest address from the input list (alignment only) */
};

struct Inputs {
    const uint8_t* userdata = nullptr; /* 192 bytes */
    Stream slot[24];
};

/* One output-layout declaration as the hole announces it. */
struct OutputDeclaration {
    uint8_t offset, format, count, semantic, attribute;
};

/* A prepared invocation: admitted, produced, culled and laid out, but not yet placed. */
struct Prepared {
    Kind kind = kKindNone;
    uint32_t vertex_count = 0;   /* V */
    uint32_t index_count = 0;    /* K */
    uint32_t instances = 1;      /* I */
    uint32_t surviving = 0;      /* K' */
    uint32_t produced = 0;       /* Vout */
    uint32_t stride = 0;         /* output vertex stride, 0 for index only */
    uint32_t index_bytes = 0;    /* bi */
    uint32_t vertex_bytes = 0;   /* bv */
    uint32_t request = 0;        /* n: 0 when nothing is drawn */
    uint32_t hole_address = 0;   /* H */
    uint32_t hole_bytes = 0;     /* 16·h */
    uint32_t ring_table = 0;     /* R */
    unsigned declaration_count = 0;
    OutputDeclaration declarations[16]{};
    /* bi + bv bytes to place at X: big-endian u16 indices, padding, then vertices. */
    const uint8_t* data = nullptr;
};

/* Ring record fields (section "Output ring record"). */
struct RingRecord {
    uint32_t start, start_offset, end, top, zero, label, consumed;
};
RingRecord read_ring(const uint8_t record[128]);
/* Writes top and consumed back into a copy of the line; every other byte is unchanged. */
void write_ring(uint8_t record[128], uint32_t top, uint32_t consumed);

enum class Allocation { kPlaced, kNeedsRefresh, kFull };
/* Section 6, one attempt. With refreshed_consumed null the cached U is used and kNeedsRefresh
 * is returned when the candidate is not free; with it, U is replaced first and kFull is returned
 * when the candidate is still not free. On kPlaced, *start is X and *consumed the U to store. */
Allocation allocate(const RingRecord& ring, uint32_t request, const uint32_t* refreshed_consumed,
                    uint32_t* start, uint32_t* consumed);

/* Per-thread working memory; reused across invocations, grown only when an invocation needs more. */
class Engine {
public:
    /* Checks 2-9 and every refusal of "Edge cases and errors"; on kHandled fills `out`. Nothing
     * guest-visible depends on this call. */
    Reason prepare(const Inputs& in, Prepared& out);

    /* The hole for a placed invocation (section 8). `x_offset` is the RSX local offset of X.
     * Writes exactly prepared.hole_bytes bytes. */
    static void build_hole(const Prepared& prepared, uint32_t x, uint32_t x_offset, uint32_t label,
                           uint8_t* out);

private:
    struct Bank {
        bool present = false;
        Reason error = kHandled;      /* when present but failed */
        const uint8_t* raw = nullptr; /* source of a byte-copy encoding (weights) */
        uint32_t raw_stride = 0;
        uint8_t codec = 0;
        std::vector<float> lane[4];
    };
    Reason project_and_cull(const Inputs& in, uint32_t vertices, uint32_t instances,
                            const float* x, const float* y, const float* z, uint32_t points,
                            uint8_t flavour);
    Reason encode_vertices(Prepared& out);

    Bank position_, normal_, tangent_, weight_;
    std::vector<float> out_x_, out_y_, out_z_;        /* produced positions */
    std::vector<float> out_nx_, out_ny_, out_nz_, out_nw_;
    std::vector<float> out_tx_, out_ty_, out_tz_, out_tw_;
    std::vector<float> sx_, sy_;
    std::vector<float> qx_, qy_;   /* floor(4·sx), floor(4·sy) */
    std::vector<uint8_t> code_;
    std::vector<uint16_t> indices_;
    std::vector<uint16_t> kept_;
    std::vector<uint8_t> data_;
};

/* Exposed for tests: decoders and helpers with the specifications' definitions. */
float rebased_half(uint16_t h);
bool spherical_normal(const uint8_t bytes[3], float out[4]);
float wave(float phase, float frequency);
uint32_t pack_normal_cmp(float x, float y, float z);
uint16_t to_binary16(float value);

/* Wind coefficients this job uses (edge-geometry-job.md, "Wind values (game-supplied)"). */
struct WindTuning {
    float ka1, ka2, kb1, kb2, kc;
};
extern const WindTuning kWindTuning;

}  // namespace mag_edgegeom
