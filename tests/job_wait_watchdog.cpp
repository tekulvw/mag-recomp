// Synthetic job-wait watchdog decisions (src/mag_job_wait_watchdog.h): a fake
// job in a fake big-endian guest buffer, a wait driven through the same poll()
// the runtime calls after each predicate pass, and a synthetic clock.
#include "mag_job_wait_watchdog.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace w = mag_job_wait_watchdog;

static int failures;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

// A sparse guest: one window that covers the job pool, a container and a marker block.
struct Guest {
    static constexpr uint64_t kBase = 0x02000000, kBytes = 0x00400000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kBytes);
    uint8_t* at(uint64_t ea) { return bytes.data() + (ea - kBase); }
    void put16(uint64_t ea, uint16_t v) { at(ea)[0] = uint8_t(v >> 8); at(ea)[1] = uint8_t(v); }
    void put32(uint64_t ea, uint32_t v) { for (int i = 0; i < 4; i++) at(ea)[i] = uint8_t(v >> (24 - 8 * i)); }
    bool read(uint64_t ea, unsigned width, uint32_t& value) {
        if (ea < kBase || ea + width > kBase + kBytes) return false;
        value = 0;
        for (unsigned i = 0; i < width; i++) value = (value << 8) | at(ea)[i];
        return true;
    }
};

constexpr uint32_t kIndex = 5;
constexpr uint32_t kJob = uint32_t(w::kPool + w::kPoolJobs + kIndex * w::kJobBytes);
constexpr uint32_t kContainer = 0x02300000, kMarkerBase = 0x02310000, kModule = 0x01F6B930;
constexpr uint16_t kMarker = 0x0123;

static void build(Guest& g, const uint16_t (&slots)[w::kSlots]) {
    g.put32(kJob + 0x288, kModule);
    g.put16(kJob + 0x2A0, kMarker);
    g.put32(kJob + 0x2A4, kContainer);
    g.put32(kContainer, kMarkerBase);
    g.put16(kMarkerBase + 0x70, 0xFFFF);  // not one of the seven slots
    for (unsigned i = 0; i < w::kSlots; i++) g.put16(kMarkerBase + 0x72 + 2 * i, slots[i]);
}

// Runs one wait for `ms` of synthetic time with a predicate pass every 10 ms;
// returns every line poll() asked to log.
static std::vector<std::string> run(Guest& g, uint64_t threshold, uint64_t ms) {
    std::vector<std::string> lines;
    w::Wait wait;
    int owner = 0;
    const uint64_t start = 1000000000ull;
    w::begin(wait, &owner, kJob, start);
    auto reader = [&](uint64_t ea, unsigned width, uint32_t& value) { return g.read(ea, width, value); };
    for (uint64_t t = 0; t <= ms; t += 10) {
        w::Context c;
        c.thread = 7;
        std::string line;
        if (w::poll(wait, &owner, start + t * 1000000, threshold, reader, c, line)) lines.push_back(line);
    }
    w::end(wait, &owner);
    return lines;
}

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    // Environment parsing: unset or invalid disables; decimal milliseconds enable.
    check(w::threshold_ms(nullptr) == 0, "unset variable disables the watchdog");
    check(w::threshold_ms("") == 0 && w::threshold_ms("0") == 0, "empty or zero disables the watchdog");
    check(w::threshold_ms("2s") == 0 && w::threshold_ms("-5") == 0 && w::threshold_ms("99999999999") == 0,
          "suffixes, signs and overflow disable rather than guess");
    check(w::threshold_ms("2000") == 2000, "decimal milliseconds select the threshold");

    {
        Guest g;
        const uint16_t slots[w::kSlots] = {kMarker, kMarker, kMarker - 1, kMarker, kMarker + 4, kMarker, kMarker};
        build(g, slots);
        const auto lines = run(g, 2000, 6000);
        check(lines.size() == 1, "one lagging slot past the threshold gives exactly one line");
        if (!lines.empty()) {
            const std::string& l = lines[0];
            std::printf("%s\n", l.c_str());
            check(l.rfind("[mag job-wait watchdog] {", 0) == 0, "line carries the watchdog prefix and a JSON object");
            check(contains(l, "\"lagging\":[2]"), "line names the lagging slot");
            check(contains(l, "\"job\":\"0x020CE960\"") && contains(l, "\"pool_index\":5"), "line names the job and pool index");
            check(contains(l, "\"module\":\"0x01F6B930\"") && contains(l, "\"module_name\":\"MaterialFlushMain\""),
                  "line names the module from the existing table");
            check(contains(l, "\"marker\":291") && contains(l, "\"container\":\"0x02300000\"") &&
                      contains(l, "\"base\":\"0x02310000\""),
                  "line carries the marker, container and base");
            check(contains(l, "\"slots\":[291,291,290,291,295,291,291]") && contains(l, "\"minimum\":290"),
                  "line carries all seven slots and their minimum");
            check(contains(l, "\"elapsed_ms\":2000") && contains(l, "\"threshold_ms\":2000"), "line reports elapsed time");
            check(l.back() == '}' && l.find('\n') == std::string::npos, "line is a single line");
        }
        check(run(g, 2000, 1990).empty(), "a wait shorter than the threshold gives no line");
    }
    {
        Guest g;
        const uint16_t slots[w::kSlots] = {kMarker, kMarker, kMarker, kMarker + 1, kMarker, kMarker, kMarker};
        build(g, slots);
        check(run(g, 2000, 6000).empty(), "all slots at the marker give no line");
    }
    {
        Guest g;
        const uint16_t slots[w::kSlots] = {0, 0, 0, 0, 0, 0, 0};
        build(g, slots);
        check(run(g, w::threshold_ms(nullptr), 60000).empty(), "unset variable gives no line, however long the wait");
        const auto lines = run(g, 2000, 6000);
        check(lines.size() == 1 && contains(lines[0], "\"lagging\":[0,1,2,3,4,5,6]"), "every lagging slot is named");
    }
    {
        // A wait owned by another thread's context, or one that already ended, is never reported.
        Guest g;
        const uint16_t slots[w::kSlots] = {0, 0, 0, 0, 0, 0, 0};
        build(g, slots);
        w::Wait wait;
        int owner = 0, other = 0;
        auto reader = [&](uint64_t ea, unsigned width, uint32_t& value) { return g.read(ea, width, value); };
        std::string line;
        w::begin(wait, &owner, kJob, 0);
        check(!w::poll(wait, &other, 5000000000ull, 2000, reader, w::Context{}, line), "another context's predicate pass is ignored");
        w::end(wait, &owner);
        check(!w::poll(wait, &owner, 5000000000ull, 2000, reader, w::Context{}, line), "an ended wait is not reported");
    }
    {
        // Unreadable job memory is still reported once, marked unreadable.
        Guest g;
        w::Wait wait;
        int owner = 0;
        auto reader = [&](uint64_t ea, unsigned width, uint32_t& value) { return g.read(ea, width, value); };
        std::string line;
        w::begin(wait, &owner, 0x00001000, 0);
        const bool first = w::poll(wait, &owner, 3000000000ull, 2000, reader, w::Context{}, line);
        check(first && contains(line, "\"readable\":false") && contains(line, "\"pool_index\":null"),
              "unreadable job is reported as unreadable");
        check(!w::poll(wait, &owner, 4000000000ull, 2000, reader, w::Context{}, line), "and only once");
    }

    std::printf("[job_wait_watchdog] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
