#pragma once
/* Opt-in watchdog for stalled job-completion waits (MAG_JOB_WAIT_WATCHDOG=<ms>).
 *
 * The retail game drains pending SPU jobs in a wait loop that sleeps and then
 * asks a completion predicate whether all seven progress slots of the job's
 * marker block have reached the job's marker. A lost job leaves that loop
 * spinning forever. This core decides when a wait has been stuck past the
 * threshold and describes the job once, as one structured line.
 *
 * It only observes: it reads guest memory through the supplied reader, never
 * writes it, never alters a return value and never releases a wait. With the
 * variable unset (or invalid) the threshold is zero and nothing is reported.
 * Reads race with the SPU side and are not a coherent snapshot.
 *
 * Retail mage_g layout, as already modelled by mag_job_marker_snapshot
 * (src/stubs.cpp): job pool 0x020CDB70 with 1024 0x2B0-byte jobs after a
 * 0x80-byte bitmap, module at job+0x288, u16 marker at job+0x2A0, container at
 * job+0x2A4 whose first word is the marker block base, and seven u16 progress
 * slots at base+0x72..0x7E. A wait completes once the minimum slot is at least
 * the marker (unsigned). */
#include <stdint.h>
#include <stdio.h>
#include <string>

namespace mag_job_wait_watchdog {

constexpr uint32_t kWaitFunction = 0x0023D7D0;      // wait-for-job-completion loop; r3 = job
constexpr uint32_t kPredicateFunction = 0x00A047D8;  // seven-slot completion predicate
constexpr uint64_t kPool = 0x020CDB70, kPoolJobs = 0x80, kJobBytes = 0x2B0;
constexpr unsigned kPoolCapacity = 1024, kSlots = 7, kMaxReports = 64;
constexpr uint64_t kMaxThresholdMs = 86400000;  // one day

/* Parses the variable's value: decimal milliseconds, 1..one day. Anything else
 * (unset, empty, zero, signs, suffixes, overflow) disables the watchdog. */
inline uint64_t threshold_ms(const char* text) {
    if (!text || !*text) return 0;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        value = value * 10 + uint64_t(*text - '0');
        if (value > kMaxThresholdMs) return 0;
    }
    return value;
}

/* Names come from the existing tools/graphics_final_marker.py table; other
 * modules are reported by address only. */
inline const char* module_name(uint32_t module) {
    switch (module) {
    case 0x01F6B930: return "MaterialFlushMain";
    case 0x01F6BA00: return "MaterialFlushDepthNormal";
    default: return nullptr;
    }
}

/* One wait on one guest thread. Begin on wait entry, end on its exit; each
 * predicate pass reports whether the wait is now due for its single line. */
struct Wait {
    const void* owner = nullptr;
    uint32_t job = 0;
    uint64_t start_ns = 0, passes = 0;
    bool active = false, reported = false;
};

inline void begin(Wait& w, const void* owner, uint32_t job, uint64_t now_ns) {
    w = Wait{};
    w.owner = owner; w.job = job; w.start_ns = now_ns; w.active = true;
}
inline void end(Wait& w, const void* owner) {
    if (w.owner == owner) w.active = false;
}
/* Counts a predicate pass; true when this wait has run for at least the
 * threshold and has not been reported yet. */
inline bool due(Wait& w, const void* owner, uint64_t now_ns, uint64_t threshold) {
    if (!threshold || !w.active || w.owner != owner) return false;
    ++w.passes;
    return !w.reported && now_ns >= w.start_ns && (now_ns - w.start_ns) / 1000000 >= threshold;
}

/* The fields of one stuck job, read through Reader:
 *   bool read(uint64_t ea, unsigned width, uint32_t& value)  // width 2 or 4, big-endian value */
struct Job {
    uint32_t job = 0, module = 0, marker = 0, container = 0, base = 0;
    int pool_index = -1;
    bool container_valid = false, base_valid = false;
    uint16_t slots[kSlots]{};
    unsigned lagging_mask = 0, minimum = 0xFFFF;
};

template <class Reader>
inline bool read_job(uint32_t job, Reader&& read, Job& out) {
    out = Job{};
    out.job = job;
    if (job >= kPool + kPoolJobs && (job - kPool - kPoolJobs) % kJobBytes == 0 &&
        (job - kPool - kPoolJobs) / kJobBytes < kPoolCapacity)
        out.pool_index = int((job - kPool - kPoolJobs) / kJobBytes);
    uint32_t value = 0;
    if (!read(uint64_t(job) + 0x288, 4, out.module)) return false;
    if (!read(uint64_t(job) + 0x2A0, 2, value)) return false;
    out.marker = value & 0xFFFF;
    if (!read(uint64_t(job) + 0x2A4, 4, out.container)) return false;
    if ((out.container & 3) || !read(out.container, 4, out.base)) return true;
    out.container_valid = true;
    if (out.base & 1) return true;
    for (unsigned i = 0; i < kSlots; i++) {
        if (!read(uint64_t(out.base) + 0x72 + 2 * i, 2, value)) return true;
        out.slots[i] = uint16_t(value);
    }
    out.base_valid = true;
    for (unsigned i = 0; i < kSlots; i++) {
        if (out.slots[i] < out.minimum) out.minimum = out.slots[i];
        if (out.slots[i] < out.marker) out.lagging_mask |= 1u << i;
    }
    return true;
}

/* A stuck job is worth a line only if a slot is still behind its marker, or if
 * the slots could not be read at all (a completed job is never reported). */
inline bool stalled(const Job& j, bool readable) {
    return !readable || !j.base_valid || j.lagging_mask != 0;
}

struct Context {
    uint64_t elapsed_ms = 0, threshold_ms = 0, passes = 0, thread = 0;
    uint64_t predicate_args[3]{}, predicate_result = 0;
};

inline std::string hex32(uint32_t v) {
    char text[16];
    snprintf(text, sizeof(text), "\"0x%08X\"", v);
    return text;
}

/* One JSON object, no newline. Addresses are hex strings; counts are numbers. */
inline std::string report(const Job& j, bool readable, const Context& c) {
    std::string o = "{\"job\":" + hex32(j.job);
    o += ",\"pool_index\":" + (j.pool_index < 0 ? std::string("null") : std::to_string(j.pool_index));
    o += ",\"readable\":"; o += readable ? "true" : "false";
    if (readable) {
        const char* name = module_name(j.module);
        o += ",\"module\":" + hex32(j.module);
        o += ",\"module_name\":" + (name ? "\"" + std::string(name) + "\"" : std::string("null"));
        o += ",\"marker\":" + std::to_string(j.marker);
        o += ",\"container\":" + hex32(j.container);
        o += ",\"container_valid\":"; o += j.container_valid ? "true" : "false";
        if (j.container_valid) o += ",\"base\":" + hex32(j.base);
        o += ",\"base_valid\":"; o += j.base_valid ? "true" : "false";
        if (j.base_valid) {
            o += ",\"slots\":[";
            for (unsigned i = 0; i < kSlots; i++) o += (i ? "," : "") + std::to_string(j.slots[i]);
            o += "],\"lagging\":[";
            bool first = true;
            for (unsigned i = 0; i < kSlots; i++)
                if (j.lagging_mask & (1u << i)) { o += (first ? "" : ",") + std::to_string(i); first = false; }
            o += "],\"minimum\":" + std::to_string(j.minimum);
        }
    }
    o += ",\"elapsed_ms\":" + std::to_string(c.elapsed_ms);
    o += ",\"threshold_ms\":" + std::to_string(c.threshold_ms);
    o += ",\"predicate_passes\":" + std::to_string(c.passes);
    o += ",\"predicate_args\":[" + hex32(uint32_t(c.predicate_args[0])) + "," + hex32(uint32_t(c.predicate_args[1])) +
         "," + hex32(uint32_t(c.predicate_args[2])) + "]";
    o += ",\"predicate_result\":" + std::to_string(c.predicate_result);
    o += ",\"thread\":" + std::to_string(c.thread);
    o += ",\"noncoherent\":true}";
    return o;
}

inline const char* prefix() { return "[mag job-wait watchdog] "; }

/* Called after each predicate pass of the wait owned by `owner`. Returns true,
 * with the full line in `line`, at most once per wait: when the wait has run
 * past the threshold and its job still has a slot behind the marker. Guest
 * memory is read only once the wait is due. */
template <class Reader>
inline bool poll(Wait& w, const void* owner, uint64_t now_ns, uint64_t threshold, Reader&& read, Context c,
                 std::string& line) {
    if (!due(w, owner, now_ns, threshold)) return false;
    Job job;
    const bool readable = read_job(w.job, read, job);
    if (!stalled(job, readable)) return false;
    w.reported = true;
    c.elapsed_ms = (now_ns - w.start_ns) / 1000000;
    c.threshold_ms = threshold;
    c.passes = w.passes;
    line = prefix() + report(job, readable, c);
    return true;
}

}  // namespace mag_job_wait_watchdog
