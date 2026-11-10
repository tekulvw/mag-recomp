/* Synthetic check of the native Edge geometry engine shim against a stand-in runtime: a fake
 * job manager behind spu_native_service_call and a fake MFC behind spu_wrch/spu_rdch. It checks
 * the integration contract: identity, service calls and their order, refusal leaving the entry
 * state, ring update before data, data before hole, each hole line's first word last, and the
 * hand-back registers. Run with MAG_NATIVE_EDGEGEOM=1 (and without it, argv[1] = "disabled"). */
#include "mag_extension.h"
#include "native_edgegeom_cases.h"
#include "spu_context.h"
#include "spu_interp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

static int failures;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } \
    } while (0)

/* ---- stand-in runtime ---------------------------------------------------------------- */
static std::map<uint32_t, uint8_t> guest;
struct Transfer { uint32_t cmd, ea, size; };
static std::vector<Transfer> transfers;
static std::vector<std::string> services;
static bool lose_reservation_once;
static bool link_check = true;
static bool nested_in_finish;
static std::vector<uint8_t> next_inputs;   /* buffer 1 as the job manager loads it for the next job */
static int nested_handled;
static uint32_t buffer1_start, buffer1_len, buffer3_start, buffer3_len;
constexpr uint32_t kGet = 0x100, kFinish = 0x200, kRelease = 0x300, kContext = 0x1330, kUserdata = 0xC40;

static uint8_t peek(uint32_t ea) { const auto it = guest.find(ea); return it == guest.end() ? 0 : it->second; }
/* As the runtime records a verified streamed region: tools/wws_job_lift.py registers the lifted
 * code's span (code end minus the load base), not the module image size, and a load leaves it
 * pending until the dispatcher's resolver compares the code. */
constexpr uint32_t kCodeSpan = 0x12A40;
static int residency_state = 1;            /* pending */
static bool code_matches = true;
static unsigned lookups;
static void lifted_entry(spu_context*) {}

static uint32_t guest32(uint32_t ea) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = v << 8 | peek(ea + i); return v; }
static void guest_put32(uint32_t ea, uint32_t v) { for (int i = 0; i < 4; ++i) guest[ea + i] = uint8_t(v >> (24 - 8 * i)); }

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
extern "C" {
void spu_wrch(spu_context* c, uint32_t ch, u128 v) {
    const uint32_t x = v._u32[0];
    switch (ch) {
    case MFC_LSA: c->mfc_lsa = x; break;
    case MFC_EAH: c->mfc_eah = x; break;
    case MFC_EAL: c->mfc_eal = x; break;
    case MFC_Size: c->mfc_size = x; break;
    case MFC_TagID: c->mfc_tag = x; break;
    case MFC_Cmd: {
        const uint32_t lsa = c->mfc_lsa, ea = c->mfc_eal, size = c->mfc_size;
        if (c->mfc_tag != 31) { std::printf("FAIL tag %u\n", c->mfc_tag); ++failures; }
        const bool ok_size = size == 1 || size == 2 || size == 4 || size == 8 || (size % 16 == 0 && size <= 16384);
        if (!ok_size || (lsa & 15) != (ea & 15)) { std::printf("FAIL dma shape lsa=%x ea=%x size=%u\n", lsa, ea, size); ++failures; }
        transfers.push_back({x, ea, size});
        if (x == MFC_GET_CMD || x == MFC_GETLLAR_CMD) {
            for (uint32_t i = 0; i < size; ++i) c->ls[lsa + i] = peek(ea + i);
            if (x == MFC_GETLLAR_CMD) { c->resv_ea = ea; c->resv_valid = 1; c->atomic_stat = 4; }
        } else if (x == MFC_PUTLLC_CMD) {
            if (lose_reservation_once) { lose_reservation_once = false; c->resv_valid = 0; }
            if (c->resv_valid && c->resv_ea == ea) {
                for (uint32_t i = 0; i < size; ++i) guest[ea + i] = c->ls[lsa + i];
                c->atomic_stat = 0;
            } else c->atomic_stat = 1;
            c->resv_valid = 0;
        } else {
            for (uint32_t i = 0; i < size; ++i) guest[ea + i] = c->ls[lsa + i];
        }
        break;
    }
    default: break;
    }
}
u128 spu_rdch(spu_context* c, uint32_t ch) {
    if (ch == MFC_RdAtomicStat) return spu_make_preferred_u32(c->atomic_stat);
    if (ch == MFC_RdTagStat) return spu_make_preferred_u32(1u << 31);
    return spu_make_preferred_u32(0);
}
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
/* The stand-in job manager: one service per entry, run when the dispatcher reaches it. */
static void job_manager(spu_context* c, uint32_t entry) {
    if (c->int_enable) { std::printf("FAIL service with interrupts enabled\n"); ++failures; }
    if ((c->gpr[1]._u32[0] & 15) || c->gpr[1]._u32[0] > 0x3F990 - 512) { std::printf("FAIL stack\n"); ++failures; }
    if (c->host_depth < 2) { std::printf("FAIL service without a nested host frame\n"); ++failures; }
    for (int r = 5; r < 80; ++r) c->gpr[r] = spu_make_preferred_u32(0xDEAD0000u + r);   /* volatile clobber */
    if (entry == kGet) {
        const uint32_t buffer = c->gpr[4]._u32[0];
        services.push_back("get" + std::to_string(buffer));
        u128 d{};
        d._u32[0] = buffer << 24;
        if (buffer == 1) { d._u32[1] = (buffer1_start / 4) << 16 | (buffer1_len / 4); d._u32[2] = 0x30000100; d._u32[3] = 192; }
        else d._u32[1] = (buffer3_start / 4) << 16 | (buffer3_len / 4);
        c->gpr[3] = d;
    } else if (entry == kRelease) {
        services.push_back("release" + std::to_string(c->gpr[4]._u32[0]));
    } else if (entry == kFinish) {
        services.push_back("finish");
        if (nested_in_finish) {
            /* The job manager runs the next invocation of this job on the same thread, from
             * inside finish, on its own stack, before returning to this one. */
            nested_in_finish = false;
            spu_context saved_outer; std::memcpy(&saved_outer, c, sizeof saved_outer);
            std::memcpy(c->ls + buffer1_start, next_inputs.data(), next_inputs.size());
            for (int r = 80; r < 128; ++r) c->gpr[r] = spu_make_preferred_u32(0xBEEF0000u + r);
            c->gpr[1]._u32[0] -= 0x400;
            for (auto& w : c->gpr[3]._u32) w = 0x1330;
            c->gpr[0] = spu_make_preferred_u32(0x3260);
            c->pc = 0x15898;
            const uint32_t depth = c->host_depth;
            c->host_depth = depth + 1;
            nested_handled = mag_payload_override(c, 0x15898);
            c->host_depth = depth;
            /* finish then returns to the outer job with its own registers back in place, except
             * for those the call may change */
            std::memcpy(c->gpr + 80, saved_outer.gpr + 80, 48 * sizeof(u128));
            c->gpr[1] = saved_outer.gpr[1];
            c->gpr[0] = saved_outer.gpr[0];
        }
        for (auto& w : c->gpr[3]._u32) w = 0x00010009;
    } else {
        std::printf("FAIL unknown service 0x%05X\n", entry); ++failures;
    }
    c->pc = c->gpr[0]._u32[0];   /* `bi $r0` with a live host frame: return to the link */
}
/* The runtime's dispatch and drain, as a lifted job's call uses them. */
void spu_indirect_branch(spu_context* c) {
    if (g_spu_trampoline_fn) { std::printf("FAIL dispatch with a pending transfer\n"); ++failures; }
    if (link_check && (c->gpr[0]._u32[0] != 0x15898 || c->gpr[0]._u32[1])) { std::printf("FAIL link\n"); ++failures; }
    job_manager(c, c->pc & SPU_LS_MASK);
}
void spu_drain_call(spu_context* c, uint32_t ret) {
    if ((c->pc & SPU_LS_MASK) != ret) { std::printf("FAIL drain did not reach its return\n"); ++failures; }
}
void spu_img_restore(spu_context* c, int32_t image) { c->image_id = c->module_img_a00 ? c->module_img_a00 : image; }
/* The runtime's service scope refuses contexts that are not RUNNING (spu_channels.c); SPURS
 * policy contexts, which run every WWS job, are not. A shim that used it handled nothing. */
int spu_native_service_call(spu_context* c, uint32_t entry, uint32_t ret) {
    if (c->status != SPU_STATUS_RUNNING || c->irq_frame || c->irq_saved || g_spu_trampoline_fn) return -1;
    c->gpr[0] = spu_make_preferred_u32(ret); c->pc = entry; c->host_depth++;
    spu_indirect_branch(c); spu_drain_call(c, ret); c->host_depth--;
    return 0;
}
void spu_halt(spu_context*) { std::abort(); }
/* The dispatcher's resolver: compares a pending verified region once, as the runtime does. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa) {
    ++lookups;
    auto* c = const_cast<spu_context*>(ctx);
    for (auto& m : c->resident_code)
        if (m.image_id && lsa >= m.lsa && lsa - m.lsa < m.size) {
            if (m.verify_state == 1) m.verify_state = code_matches ? 2 : 3;
            return m.verify_state == 2 || m.verify_state == 0 ? lifted_entry : nullptr;
        }
    return nullptr;
}
}

/* ---- an invocation in local store ------------------------------------------------------- */
static spu_context* make_context(const Case& in, bool resident = true) {
    static spu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ls = ctx.ls_store;
    ctx.status = SPU_STATUS_STOPPED;   /* as the runtime's SPURS policy contexts are */
    ctx.pc = 0x15898;
    ctx.host_depth = 1;                /* inside the policy's bracket around the payload call */
    ctx.int_enable = 1;
    for (int r = 0; r < 128; ++r) for (int w = 0; w < 4; ++w) ctx.gpr[r]._u32[w] = 0x01000000u * r + w;
    for (auto& w : ctx.gpr[0]._u32) w = 0x3260;
    ctx.gpr[1]._u32[0] = 0x3F990; ctx.gpr[1]._u32[1] = 0x1980; ctx.gpr[1]._u32[2] = ctx.gpr[1]._u32[3] = 0;
    for (auto& w : ctx.gpr[3]._u32) w = kContext;
    if (resident) ctx.resident_code[1] = {0x4000, kCodeSpan, 0x01963A00, 77, residency_state, 0};
    auto put = [&](uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) ctx.ls[a + i] = uint8_t(v >> (24 - 8 * i)); };
    put(kContext + 0x00, 2);                     /* virtual SPU 2 */
    put(kContext + 0x20, kUserdata);
    put(kContext + 0x34, kGet); put(kContext + 0x44, kFinish); put(kContext + 0x48, kRelease);
    std::memcpy(ctx.ls + kUserdata, in.userdata.data(), 192);
    buffer1_start = 0x1FC00;
    uint32_t at = buffer1_start;
    for (int s = 0; s < 24; ++s) { std::memcpy(ctx.ls + at, in.slot[s].data(), in.slot[s].size()); at += uint32_t(in.slot[s].size()); }
    for (int s = 0; s < 24; ++s) { put(at + 8 * s, uint32_t(in.slot[s].size())); put(at + 8 * s + 4, in.address[s]); }
    at += 192;
    buffer1_len = at - buffer1_start;
    buffer3_start = at; buffer3_len = 0x8000;
    return &ctx;
}

static void ring(uint32_t start, uint32_t end, uint32_t top, uint32_t consumed, uint32_t label) {
    const uint32_t line = 0x10000000u + 0x80u * 3u;             /* R + 0x80·(s + 1), s = 2 */
    guest_put32(line + 0x00, start); guest_put32(line + 0x04, start - 0xC0000000u); guest_put32(line + 0x08, end);
    guest_put32(line + 0x0C, top); guest_put32(line + 0x10, 0); guest_put32(line + 0x14, label);
    guest_put32(line + 0x18, consumed);
    for (uint32_t i = 0x1C; i < 0x80; i += 4) guest_put32(line + i, 0);
}

static Case triangle_case() {
    Case c; c.positions({at(0, 0, 0), at(0.1f, 0, 0), at(0, 0.1f, 0)}); c.counts(3, 3); c.indices({0, 1, 2});
    return c;
}

static void reset() { guest.clear(); transfers.clear(); services.clear(); }

static void handled_invocation() {
    reset();
    const Case c = triangle_case();
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    spu_context* ctx = make_context(c);
    const u128 r1 = ctx->gpr[1], r80 = ctx->gpr[80], r127 = ctx->gpr[127];
    CHECK(mag_payload_override(ctx, 0x15898) == 1);
    CHECK((services == std::vector<std::string>{"get1", "get3", "release3", "finish"}));
    CHECK(ctx->pc == 0x3260 && ctx->int_enable == 1);
    CHECK(ctx->gpr[3]._u32[0] == 9 && !ctx->gpr[3]._u32[1] && !ctx->gpr[3]._u32[2] && !ctx->gpr[3]._u32[3]);
    CHECK(!std::memcmp(&ctx->gpr[1], &r1, 16) && !std::memcmp(&ctx->gpr[80], &r80, 16) && !std::memcmp(&ctx->gpr[127], &r127, 16));
    /* Ring: T advanced by n = 128 from X = old T; U and every other field unchanged. */
    const uint32_t line = 0x10000180u;
    CHECK(guest32(line + 0x0C) == 0xCF000180u && guest32(line + 0x18) == 0xCF000000u && guest32(line + 0x14) == 0xB00005D0u);
    /* Data: the three indices at X. */
    CHECK(guest32(0xCF000100u) == 0x00000001u && (guest32(0xCF000104u) >> 16) == 2);
    /* Hole bytes as the core lays them out. */
    Prepared p; Engine e; CHECK(e.prepare(c.inputs(), p) == kHandled);
    uint8_t hole[112]; Engine::build_hole(p, 0xCF000100u, 0x0F000100u, 0xB00005D0u, hole);
    for (uint32_t i = 0; i < 112; ++i) CHECK(peek(0x70000000u + i) == hole[i]);
    CHECK(guest32(0x70000000u + 4 * 21) == 0x00041D74u && guest32(0x70000000u + 4 * 22) == 0xCF000180u);
    /* Order: ring line, then data, then the hole with each line's first word last. */
    size_t ring_at = 0, data_at = 0, first_word_at = 0, last_rest_at = 0;
    for (size_t i = 0; i < transfers.size(); ++i) {
        const Transfer& t = transfers[i];
        if (t.cmd == MFC_PUTLLC_CMD) ring_at = i + 1;
        if (t.ea >= 0xCF000000u && t.ea < 0xCF010000u) data_at = i + 1;
        if (t.ea == 0x70000000u) first_word_at = i + 1;
        else if (t.ea > 0x70000000u && t.ea < 0x70000070u) last_rest_at = i + 1;
    }
    CHECK(ring_at && data_at > ring_at && last_rest_at > data_at && first_word_at > last_rest_at);
}

static void hole_across_lines() {
    reset();
    Case c = triangle_case();
    put32(c.userdata, 0x98, 0x70000050);           /* 48 bytes in the first line, 64 in the next */
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    spu_context* ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 1);
    size_t first0 = 0, first1 = 0, rest0 = 0, rest1 = 0;
    for (size_t i = 0; i < transfers.size(); ++i) {
        const Transfer& t = transfers[i];
        if (t.ea == 0x70000050u) first0 = i + 1;
        else if (t.ea == 0x70000080u) first1 = i + 1;
        else if (t.ea > 0x70000050u && t.ea < 0x70000080u) rest0 = std::max(rest0, i + 1);
        else if (t.ea > 0x70000080u && t.ea < 0x700000C0u) rest1 = std::max(rest1, i + 1);
    }
    CHECK(first0 > rest0 && first1 > rest1 && rest0 && rest1);
}

static void ring_refresh_and_retry() {
    reset();
    const Case c = triangle_case();
    /* T at the end: wrap to S, which the cached U blocks; the label frees it. */
    ring(0xCF000000u, 0xCF010000u, 0xCF00FFC0u, 0xCF000040u, 0xB00005D0u);
    guest_put32(0xB00005D0u, 0xCF00F000u);
    lose_reservation_once = true;
    spu_context* ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 1);
    const uint32_t line = 0x10000180u;
    CHECK(guest32(line + 0x0C) == 0xCF000080u && guest32(line + 0x18) == 0xCF00F000u);
    unsigned getllar = 0, label_reads = 0;
    for (const Transfer& t : transfers) { getllar += t.cmd == MFC_GETLLAR_CMD; label_reads += t.cmd == MFC_GET_CMD && t.ea == 0xB00005D0u; }
    CHECK(getllar == 2 && label_reads == 2);
}

static void residency() {
    const Case c = triangle_case();
    /* The in-game state: a code-span record, verified on an earlier dispatch. */
    reset(); residency_state = 2; code_matches = true;
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    spu_context* ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 1);
    /* First dispatch after a load: the resolver verifies it and the invocation runs natively. */
    reset(); residency_state = 1; lookups = 0;
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 1 && lookups == 1 && ctx->resident_code[1].verify_state == 2);
    /* Loaded bytes that do not match the lifted code: refused before any service call. */
    reset(); residency_state = 1; code_matches = false;
    ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 0 && services.empty() && transfers.empty());
    /* A region registered without code bytes, another module's source, or a span that does not
     * cover the entry: refused. */
    reset(); residency_state = 0; code_matches = true;
    ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 0 && services.empty());
    residency_state = 2;
    ctx = make_context(c); ctx->resident_code[1].source_ea = 0x01963B00;
    CHECK(mag_payload_override(ctx, 0x15898) == 0 && services.empty());
    ctx = make_context(c); ctx->resident_code[1].size = 0x11000;
    CHECK(mag_payload_override(ctx, 0x15898) == 0 && services.empty());
    ctx = make_context(c); ctx->resident_code[1].image_id = 0;
    CHECK(mag_payload_override(ctx, 0x15898) == 0 && services.empty());
    residency_state = 1;
}

/* Small jobs get small buffers: buffer 1 just holds the inputs and buffer 3 follows it. */
static void small_buffers() {
    reset();
    const Case c = triangle_case();
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    spu_context* ctx = make_context(c);
    buffer3_len = 0x300;
    CHECK(mag_payload_override(ctx, 0x15898) == 1);
    CHECK(guest32(0xCF000100u) == 0x00000001u && guest32(0x70000000u) == 0x00041710u);
}

/* Finish may run another invocation of this job on the same thread before it returns: the
 * outer invocation must still hand back its own r1 and r80-r127. */
static void invocation_inside_finish() {
    reset();
    const Case c = triangle_case();
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    spu_context* ctx = make_context(c);
    const u128 r1 = ctx->gpr[1], r80 = ctx->gpr[80], r127 = ctx->gpr[127];
    next_inputs.assign(ctx->ls + buffer1_start, ctx->ls + buffer3_start);
    nested_in_finish = true; nested_handled = 0;
    CHECK(mag_payload_override(ctx, 0x15898) == 1 && nested_handled == 1);
    CHECK(!std::memcmp(&ctx->gpr[1], &r1, 16) && !std::memcmp(&ctx->gpr[80], &r80, 16) && !std::memcmp(&ctx->gpr[127], &r127, 16));
    CHECK(ctx->pc == 0x3260);
}

static void refusals_leave_entry_state() {
    /* Not resident: no service call and no transfer. */
    reset();
    Case c = triangle_case();
    spu_context* ctx = make_context(c, false);
    CHECK(mag_payload_override(ctx, 0x15898) == 0 && services.empty() && transfers.empty());
    /* Another job's entry: not asked. */
    ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x12D28) == 0 && services.empty());

    /* Admission refusal after acquiring the buffers: the entry state is back, nothing written. */
    reset();
    c.userdata[0xA7] = 11; c.slot[19][6] = 0x11;
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    ctx = make_context(c);
    spu_context before; std::memcpy(&before, ctx, sizeof before);
    const auto guest_before = guest;
    CHECK(mag_payload_override(ctx, 0x15898) == 0);
    CHECK((services == std::vector<std::string>{"get1", "get3"}));
    CHECK(!std::memcmp(ctx->gpr, before.gpr, sizeof before.gpr) && ctx->pc == 0x15898 && ctx->int_enable == 1);
    CHECK(guest == guest_before);
    for (const Transfer& t : transfers) CHECK(t.cmd == MFC_GET_CMD || t.cmd == MFC_GETLLAR_CMD);

    /* Ring full after one refresh: refused before touching the record. */
    reset();
    c = triangle_case();
    ring(0xCF000000u, 0xCF010000u, 0xCF00FFC0u, 0xCF000040u, 0xB00005D0u);
    guest_put32(0xB00005D0u, 0xCF000040u);
    ctx = make_context(c);
    std::memcpy(&before, ctx, sizeof before);
    const auto ring_before = guest;
    CHECK(mag_payload_override(ctx, 0x15898) == 0);
    CHECK(guest == ring_before && !std::memcmp(ctx->gpr, before.gpr, sizeof before.gpr) && ctx->pc == 0x15898);
    /* The lifted job that runs next reads its inputs from the buffers: a refusal after the ring
     * reservation must leave every buffer byte as the job manager loaded it (a ring line staged
     * in buffer 1 corrupted them and froze hosted Sabotage). */
    CHECK(!std::memcmp(ctx->ls + buffer1_start, before.ls_store + buffer1_start, buffer3_start + buffer3_len - buffer1_start));
}

static void nothing_drawn() {
    reset();
    Case c; c.positions({at(1.5f, 0, 0), at(2, 0, 0), at(1.5f, 0.1f, 0)}); c.counts(3, 3); c.indices({0, 1, 2});
    ring(0xCF000000u, 0xCF010000u, 0xCF000100u, 0xCF000000u, 0xB00005D0u);
    spu_context* ctx = make_context(c);
    CHECK(mag_payload_override(ctx, 0x15898) == 1);
    for (const Transfer& t : transfers) CHECK(t.cmd != MFC_GETLLAR_CMD && t.cmd != MFC_PUTLLC_CMD && !(t.ea >= 0xCF000000u && t.ea < 0xCF010000u));
    CHECK(guest32(0x70000000u) == 0x00041710u && guest32(0x70000000u + 4 * 8) == 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "disabled")) {
        /* Without MAG_NATIVE_EDGEGEOM=1 nothing registers: the seam stays inert. */
        Case c = triangle_case();
        spu_context* ctx = make_context(c);
        CHECK(!mag_extension()->payload_override && mag_payload_override(ctx, 0x15898) == 0 && services.empty());
        if (failures) return 1;
        std::puts("PASS native edgegeom shim disabled");
        return 0;
    }
    residency();
    handled_invocation();
    small_buffers();
    invocation_inside_finish();
    hole_across_lines();
    ring_refresh_and_retry();
    refusals_leave_entry_state();
    nothing_drawn();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::puts("PASS native edgegeom shim");
    return 0;
}
