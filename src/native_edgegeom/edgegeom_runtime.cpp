/* Native Edge geometry child job: the engine shim. Registers a payload override
 * (src/mag_extension.h) that runs the job natively when MAG_NATIVE_EDGEGEOM=1, following the
 * integration contract and job API calling contract of
 * docs/render-frontend/behavior/edge-geometry-job.md. Every guest-memory effect goes through
 * the runtime's MFC channel interface (spu_wrch/spu_rdch) on tag 31, and every job-manager
 * service is called through spu_native_service_call. MAG_NATIVE_EDGEGEOM_STATS=1 adds
 * handled/refused counters per kind and reason, printed every few seconds and at exit. */
#include "edgegeom.h"
#include "mag_extension.h"
#include "spu_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* The runtime's channel interface (runtime/spu/spu_channels.c), declared as the interpreter does. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4190)
#endif
extern "C" {
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
int spu_native_service_call(spu_context* ctx, uint32_t entry_pc, uint32_t return_pc);
void spu_halt(spu_context* ctx);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {
using namespace mag_edgegeom;

/* Identity (section "Identity"). */
constexpr uint32_t kEntry = 0x15898, kModuleEa = 0x01963A00, kModuleLs = 0x4000, kModuleSpan = 0x1B870;
/* Runtime residency state of a verified streamed region (runtime/spu/spu_channels.c). */
constexpr int kResidencyVerified = 2;
/* Job API context fields and service slots ("Job API calling contract"). */
constexpr uint32_t kCtxSpu = 0x00, kCtxUserdata = 0x20, kSlotGetBuffer = 0x34, kSlotFinish = 0x44, kSlotRelease = 0x48;
constexpr uint32_t kTag = 31;
constexpr uint32_t kFrame = 512;
constexpr uint32_t kMaxDma = 16384;

const bool enabled = [] { const char* e = std::getenv("MAG_NATIVE_EDGEGEOM"); return e && !std::strcmp(e, "1"); }();
const bool stats = [] { const char* e = std::getenv("MAG_NATIVE_EDGEGEOM_STATS"); return e && !std::strcmp(e, "1"); }();

/* Diagnostic counters: [kind][reason]; kind none for refusals decided before the kind. */
std::atomic<uint64_t> counters[kKindCount][kReasonCount];
std::atomic<uint64_t> handled_ns{0};
std::atomic<int64_t> last_report_ms{0};

void count(Kind kind, Reason reason, uint64_t ns) {
    if (!stats) return;
    counters[kind][reason].fetch_add(1, std::memory_order_relaxed);
    if (reason == kHandled) handled_ns.fetch_add(ns, std::memory_order_relaxed);
}

void report(const char* when) {
    uint64_t total = 0, handled = 0;
    for (auto& row : counters) for (unsigned r = 0; r < kReasonCount; ++r) {
        const uint64_t n = row[r].load(std::memory_order_relaxed);
        total += n; if (r == kHandled) handled += n;
    }
    std::fprintf(stderr, "[native-edgegeom] %s: invocations=%llu handled=%llu refused=%llu mean_native_us=%.2f\n", when,
                 (unsigned long long)total, (unsigned long long)handled, (unsigned long long)(total - handled),
                 handled ? handled_ns.load() / 1000.0 / double(handled) : 0.0);
    for (unsigned k = 0; k < kKindCount; ++k)
        for (unsigned r = 0; r < kReasonCount; ++r) {
            const uint64_t n = counters[k][r].load(std::memory_order_relaxed);
            if (n) std::fprintf(stderr, "[native-edgegeom]   kind=%s %s=%llu\n", kind_name(Kind(k)), reason_name(Reason(r)),
                                (unsigned long long)n);
        }
    std::fflush(stderr);
}

void on_frame() {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t last = last_report_ms.load(std::memory_order_relaxed);
    if (!last) { last_report_ms.store(now); return; }
    if (now - last >= 5000 && last_report_ms.compare_exchange_strong(last, now)) report("periodic");
}

void on_stop() { report("exit"); }

inline uint32_t ls32(const spu_context* c, uint32_t a) {
    const uint8_t* p = c->ls + (a & SPU_LS_MASK);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline u128 splat(uint32_t v) { u128 r; for (auto& w : r._u32) w = v; return r; }
inline void wr(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, spu_make_preferred_u32(v)); }

/* One MFC command on tag 31; the runtime completes ordinary transfers synchronously, and the
 * tag wait keeps that explicit. */
void mfc(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size) {
    wr(c, MFC_LSA, lsa); wr(c, MFC_EAH, 0); wr(c, MFC_EAL, ea); wr(c, MFC_Size, size); wr(c, MFC_TagID, kTag);
    wr(c, MFC_Cmd, cmd);
}
void wait_tag(spu_context* c) {
    wr(c, MFC_WrTagMask, 1u << kTag); wr(c, MFC_WrTagUpdate, 2);
    (void)spu_rdch(c, MFC_RdTagStat);
}

/* Calls a job-manager service as an SPU subroutine, interrupts disabled around the call.
 * Returns spu_native_service_call's result: 0 returned, 1 halted, -1 refused or mismatched. */
int service(spu_context* c, uint32_t context, uint32_t slot, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6,
            bool with_a6) {
    const uint32_t entry = ls32(c, context + slot) & SPU_LS_MASK & ~3u;
    c->gpr[3] = splat(a3); c->gpr[4] = splat(a4); c->gpr[5] = splat(a5);
    if (with_a6) c->gpr[6] = splat(a6);
    c->int_enable = 0;
    const int r = spu_native_service_call(c, entry, kEntry);
    c->int_enable = 1;
    return r;
}

struct Saved {
    u128 gpr[128];
    uint32_t int_enable;
};

bool run(spu_context* c) {
    const auto t0 = std::chrono::steady_clock::now();
    /* Check 1: the resident module and the target (the policy's identity is checked when the
     * payload-override lift step is applied). */
    bool resident = false, verified = false;
    for (const auto& m : c->resident_code)
        if (m.size == kModuleSpan && m.lsa == kModuleLs && m.source_ea == kModuleEa) {
            resident = true; verified = m.verify_state == kResidencyVerified;
        }
    if (!resident || !verified || c->ls != c->ls_store) { count(kKindNone, kIdentity, 0); return false; }

    const uint32_t context = c->gpr[3]._u32[0] & SPU_LS_MASK;
    static thread_local Saved saved;
    std::memcpy(saved.gpr, c->gpr, sizeof saved.gpr);
    saved.int_enable = c->int_enable;
    const uint32_t sp = saved.gpr[1]._u32[0];
    if ((sp & 15u) || sp < 0x1000u + kFrame || sp >= SPU_LS_SIZE) { count(kKindNone, kInputs, 0); return false; }

    /* Every register as at entry: a refusal hands the lifted job exactly its entry state. */
    auto restore = [&] {
        std::memcpy(c->gpr, saved.gpr, sizeof saved.gpr);
        c->int_enable = saved.int_enable;
        c->pc = kEntry;
    };
    /* Acquire the buffers as the job does, on a frame below the entry stack. */
    for (auto& w : c->gpr[1]._u32) w -= kFrame;
    std::memcpy(c->ls + (c->gpr[1]._u32[0] & SPU_LS_MASK), &saved.gpr[1], 16);   /* back chain */
    int s = service(c, context, kSlotGetBuffer, 0, 1, 0, 1, true);
    if (s) { restore(); if (s == 1) spu_halt(c); count(kKindNone, kService, 0); return false; }
    const u128 buffer1 = c->gpr[3];
    s = service(c, context, kSlotGetBuffer, 0, 3, 0, 0, true);
    if (s) { restore(); if (s == 1) spu_halt(c); count(kKindNone, kService, 0); return false; }
    const u128 buffer3 = c->gpr[3];

    /* Inputs (section "Job API calling contract", buffer descriptor). */
    const uint32_t b1 = (buffer1._u32[1] >> 16) * 4u, b1_len = (buffer1._u32[1] & 0xFFFFu) * 4u;
    const uint32_t b3 = (buffer3._u32[1] >> 16) * 4u, b3_len = (buffer3._u32[1] & 0xFFFFu) * 4u;
    Inputs in;
    const uint32_t userdata = ls32(c, context + kCtxUserdata) & SPU_LS_MASK;
    bool inputs_ok = b1_len >= 192 && b1 + b1_len <= SPU_LS_SIZE && userdata + 192 <= SPU_LS_SIZE &&
                     b3 + b3_len <= SPU_LS_SIZE;
    if (inputs_ok) {
        in.userdata = c->ls + userdata;
        const uint32_t list = b1 + b1_len - 192;
        uint32_t at = b1;
        for (unsigned k = 0; k < 24 && inputs_ok; ++k) {
            const uint32_t len = ls32(c, list + 8 * k), addr = ls32(c, list + 8 * k + 4);
            if (len > list - at) { inputs_ok = false; break; }
            in.slot[k] = Stream{c->ls + at, len, addr};
            at += len;
        }
    }
    static thread_local Engine engine;
    Prepared p;
    Reason reason = inputs_ok ? engine.prepare(in, p) : kInputs;
    Kind kind = p.kind;

    /* Staging in buffer 3: ring line, label word, hole, then a data window. */
    const uint32_t stage = (b3 + 127u) & ~127u;
    const uint32_t ring_lsa = stage, label_lsa = stage + 128, hole_lsa = stage + 256, window = stage + 768;
    const uint32_t window_len = b3 + b3_len > window ? std::min<uint32_t>(kMaxDma, (b3 + b3_len - window) & ~127u) : 0;
    if (reason == kHandled && (window_len < 1024 || p.hole_bytes > 240)) reason = kInputs;

    uint32_t X = 0, x_offset = 0, label = 0;
    if (reason == kHandled && p.request) {
        /* Section 6: reserve the ring line, place the request, write it back conditionally. */
        const uint32_t spu = ls32(c, context + kCtxSpu);
        const uint32_t line = p.ring_table + 0x80u * (spu + 1u);
        reason = kRingFull;
        for (int attempt = 0; attempt < 64; ++attempt) {
            mfc(c, MFC_GETLLAR_CMD, ring_lsa, line, 128);
            (void)spu_rdch(c, MFC_RdAtomicStat);
            uint8_t record[128];
            std::memcpy(record, c->ls + ring_lsa, 128);
            const RingRecord ring = read_ring(record);
            if (ring.label < 0xB0000000u) { reason = kInputs; break; }
            uint32_t consumed = 0;
            Allocation a = allocate(ring, p.request, nullptr, &X, &consumed);
            if (a == Allocation::kNeedsRefresh) {
                const uint32_t lsa = label_lsa + (ring.label & 15u);
                mfc(c, MFC_GET_CMD, lsa, ring.label, 4); wait_tag(c);
                const uint32_t fresh = ls32(c, lsa);
                a = allocate(ring, p.request, &fresh, &X, &consumed);
            }
            if (a != Allocation::kPlaced) break;
            write_ring(record, X + p.request, consumed);
            std::memcpy(c->ls + ring_lsa, record, 128);
            mfc(c, MFC_PUTLLC_CMD, ring_lsa, line, 128);
            if (spu_rdch(c, MFC_RdAtomicStat)._u32[0] & 1u) continue;   /* reservation lost: retry */
            x_offset = X - ring.start + ring.start_offset;
            label = ring.label;
            reason = kHandled;
            break;
        }
    }
    if (reason != kHandled) {
        /* Refusal after acquisition: the lifted job runs from its entry and repeats the calls. */
        restore();
        count(kind, reason, 0);
        return false;
    }

    /* Section 7: data, in windows of at most 16 KiB. */
    if (p.request) {
        const uint32_t total = p.index_bytes + p.vertex_bytes;
        for (uint32_t done = 0; done < total;) {
            const uint32_t n = std::min(window_len, total - done);
            std::memcpy(c->ls + window, p.data + done, n);
            mfc(c, MFC_PUT_CMD, window, X + done, (n + 15u) & ~15u);
            wait_tag(c);
            done += n;
        }
    }
    /* Section 8 and 9: the hole, every line's first word last. */
    uint8_t hole[256];
    Engine::build_hole(p, X, x_offset, label, hole);
    const uint32_t H = p.hole_address, hole_base = hole_lsa + (H & 127u);
    std::memcpy(c->ls + hole_base, hole, p.hole_bytes);
    for (uint32_t seg = H; seg < H + p.hole_bytes; seg = (seg + 128u) & ~127u) {
        const uint32_t end = std::min(H + p.hole_bytes, (seg + 128u) & ~127u), l = hole_base + (seg - H);
        mfc(c, MFC_PUT_CMD, l + 4, seg + 4, 4);
        mfc(c, MFC_PUT_CMD, l + 8, seg + 8, 8);
        if (end - seg > 16) mfc(c, MFC_PUT_CMD, l + 16, seg + 16, end - seg - 16);
    }
    wait_tag(c);
    std::atomic_thread_fence(std::memory_order_release);
    for (uint32_t seg = H; seg < H + p.hole_bytes; seg = (seg + 128u) & ~127u)
        mfc(c, MFC_PUTF_CMD, hole_base + (seg - H), seg, 4);
    wait_tag(c);

    /* Hand back: release buffer 3, finish, and return as the job does. */
    s = service(c, context, kSlotRelease, 0, 3, 0, 0, false);
    if (s == 0) s = service(c, context, kSlotFinish, 0, 0, 0, 0, false);
    if (s) {
        /* The job's guest-visible work is done; a failing service is a runtime fault, not a
         * refusal. Leave it visible. */
        std::fprintf(stderr, "[native-edgegeom] job manager service failed (%d) after a native invocation\n", s);
        if (s == 1) spu_halt(c);
        std::abort();
    }
    const uint32_t result = c->gpr[3]._u32[0] & 0xFFFFu;
    const uint32_t enable = c->int_enable;
    restore();
    c->int_enable = enable;
    c->gpr[3] = splat(result);
    c->pc = saved.gpr[0]._u32[0] & SPU_LS_MASK;
    count(kind, kHandled, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count()));
    return true;
}

bool payload_override(void* opaque, uint32_t target) {
    if (!enabled || target != kEntry) return false;
    return run(static_cast<spu_context*>(opaque));
}

struct Register {
    Register() {
        if (!enabled) return;
        MagExtensionCallbacks callbacks{};
        callbacks.payload_override = payload_override;
        if (stats) { callbacks.frame_poll = on_frame; callbacks.stop = on_stop; }
        if (mag_extension_register(&callbacks) != 0)
            std::fprintf(stderr, "[native-edgegeom] could not register the payload override; the lifted job runs\n");
        else
            std::fprintf(stderr, "[native-edgegeom] enabled (requires the payload-override lift step)\n");
    }
} registration;

}  // namespace
