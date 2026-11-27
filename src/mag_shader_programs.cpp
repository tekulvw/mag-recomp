// Finding MAG's RSX programs in its archives (src/mag_shader_programs.h).
#include "mag_shader_programs.h"

#include "player_inflate.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>

namespace mag::shaders {
namespace {

constexpr uint32_t kRecordMarker = 0x600df00du;
constexpr uint32_t kShaderEffect = 0xb66091f2u;

uint32_t be16(const uint8_t *p) { return (uint32_t(p[0]) << 8) | p[1]; }
uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be_n(const uint8_t *p, unsigned n) { uint64_t v = 0; while (n--) v = (v << 8) | *p++; return v; }
/* A fragment program word as the RSX reads it: big-endian, then its halves swapped. */
uint32_t fp_word(const uint8_t *p) { const uint32_t v = be32(p); return (v << 16) | (v >> 16); }

/* A vertex program found behind its header: program at `at`, `count` instructions. */
struct VertexAt { size_t header, at, count; };

/* Vertex program headers in [lo, hi), each with the program that follows it: the end bit must be
 * set on exactly its last declared instruction. */
std::vector<VertexAt> find_vertex_programs(const uint8_t *d, size_t lo, size_t hi) {
    std::vector<VertexAt> out;
    for (size_t h = lo; h + 32 < hi; h += 2) {
        const size_t count = be16(d + h);
        if (count < 1 || count > 512 || be16(d + h + 2) != 0 || d[h + 14] != 0xff || d[h + 15] != 0xff) continue;
        for (size_t off = 16; off < 16 * 40; off += 16) {
            const size_t at = h + off, last = at + 16 * (count - 1);
            if (last + 16 > hi) break;
            if (!(be32(d + last + 12) & 1u)) continue;
            bool early = false;
            for (size_t k = 0; k + 1 < count && !early; ++k) early = (be32(d + at + 16 * k + 12) & 1u) != 0;
            if (early) continue;
            out.push_back({h, at, count});
            h = at + 16 * count - 2;  // resume after this program (the loop adds 2)
            break;
        }
    }
    return out;
}

/* A fragment program header at `h`: its program's place and size, and its control word. */
bool fragment_header(const uint8_t *d, size_t h, size_t hi, size_t &at, size_t &size, uint32_t &control,
                     size_t min_rel, size_t max_rel, size_t max_size) {
    if (h + 0x14 > hi) return false;
    size = be32(d + h);
    const size_t rel = be32(d + h + 0x10);
    if (!size || size % 16 || size > max_size || rel < min_rel || rel >= max_rel || rel > hi - h ||
        size > hi - h - rel) return false;
    if (fragment_program_size(d + h + rel, size) != size) return false;
    at = h + rel;
    control = be32(d + h + 8);
    return true;
}

Program vertex_program(const uint8_t *d, size_t at, size_t count) {
    Program p;
    p.vertex = true;
    p.ucode.resize(16 * count);
    for (size_t i = 0; i < 16 * count; i += 4) {  // big-endian words to the guest's upload order
        p.ucode[i] = d[at + i + 3]; p.ucode[i + 1] = d[at + i + 2];
        p.ucode[i + 2] = d[at + i + 1]; p.ucode[i + 3] = d[at + i];
    }
    return p;
}

Program fragment_program(const uint8_t *d, size_t at, size_t size, uint32_t control) {
    Program p;
    p.ucode.assign(d + at, d + at + size);
    p.control = control;
    return p;
}

/* One effect record in [lo, hi): every technique's vertex and fragment program, or none. */
bool parse_effect(const uint8_t *d, size_t lo, size_t hi, std::vector<Program> &out, ScanStats &stats) {
    const uint32_t declared = lo + 0x2c <= hi ? be32(d + lo + 0x28) : 0;
    const auto vps = find_vertex_programs(d, lo, hi);
    if (vps.empty() || vps.size() != declared) return false;
    std::vector<Program> found;
    for (const auto &v : vps) {
        size_t at = 0, size = 0;
        uint32_t control = 0;
        bool ok = false;
        for (size_t extra = 0; extra <= 32 && !ok; extra += 16) {
            const size_t h = v.at + 16 * v.count + extra;
            ok = h < hi && fragment_header(d, h, hi, at, size, control, 1, hi - h, SIZE_MAX);
        }
        if (!ok) return false;
        found.push_back(vertex_program(d, v.at, v.count));
        found.push_back(fragment_program(d, at, size, control));
    }
    stats.techniques += vps.size();
    for (auto &p : found) { (p.vertex ? stats.vertex : stats.fragment)++; out.push_back(std::move(p)); }
    return true;
}

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

size_t fragment_program_size(const uint8_t *p, size_t limit) {
    for (size_t off = 0; off + 16 <= limit;) {
        const uint32_t w0 = fp_word(p + off), w1 = fp_word(p + off + 4);
        const uint32_t w2 = fp_word(p + off + 8), w3 = fp_word(p + off + 12);
        off += 16;
        if ((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u) off += 16;  // its inline constant
        if (w0 & 1u) return off <= limit ? off : 0;
    }
    return 0;
}

bool resource_block_may_hold_effects(const uint8_t *d, size_t size) {
    if (size < 0x30) return true;
    const size_t table = be32(d + 4), types = be32(d + 0x2c);
    if (types > 4096 || table < 0x30 || table + 8 * types > size) return true;
    for (size_t i = 0; i < types; ++i)
        if (be32(d + table + 8 * i) == kShaderEffect) return be32(d + table + 8 * i + 4) != 0;
    return false;
}

void scan_resource_block(const uint8_t *d, size_t size, std::vector<Program> &out, ScanStats &stats) {
    std::vector<size_t> marks;
    for (size_t i = 0; i + 4 <= size; ++i)  // records need not be word-aligned
        if (d[i] == 0x60 && be32(d + i) == kRecordMarker) marks.push_back(i);
    bool any = false;
    for (size_t k = 0; k < marks.size(); ++k) {
        const size_t lo = marks[k], hi = k + 1 < marks.size() ? marks[k + 1] : size;
        if (lo + 0x20 > hi || be32(d + lo + 0x1c) != kShaderEffect) continue;
        any = true;
        if (parse_effect(d, lo, hi, out, stats)) stats.effects++;
        else stats.effects_skipped++;
    }
    if (any) stats.effect_blocks++;
}

void scan_loose_file(const uint8_t *d, size_t size, std::vector<Program> &out, ScanStats &stats) {
    const size_t before = out.size();
    for (const auto &v : find_vertex_programs(d, 0, size)) {
        out.push_back(vertex_program(d, v.at, v.count));
        stats.vertex++;
    }
    for (size_t h = 0; h + 0x14 <= size; h += 2) {
        size_t at = 0, n = 0;
        uint32_t control = 0;
        if (!fragment_header(d, h, size, at, n, control, 0x14, 0x1000, 0x4000)) continue;
        out.push_back(fragment_program(d, at, n, control));
        stats.fragment++;
    }
    if (out.size() != before) stats.loose_files++;
}

Psarc::~Psarc() { if (file_) std::fclose(file_); }

bool Psarc::fetch(uint64_t offset, size_t size, uint8_t *out) const {
    if (offset > size_ || size > size_ - offset) return false;
    if (!file_) { std::memcpy(out, memory_.data() + offset, size); return true; }
#ifdef _WIN32
    if (_fseeki64(file_, (long long)offset, SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, (off_t)offset, SEEK_SET) != 0) return false;
#endif
    return std::fread(out, 1, size, file_) == size;
}

bool Psarc::open_file(const std::string &path, std::string &error) {
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) { error = "cannot open " + path; return false; }
#ifdef _WIN32
    if (_fseeki64(file_, 0, SEEK_END) != 0) { error = "cannot read " + path; return false; }
    const long long end = _ftelli64(file_);
#else
    if (fseeko(file_, 0, SEEK_END) != 0) { error = "cannot read " + path; return false; }
    const long long end = (long long)ftello(file_);
#endif
    if (end < 0) { error = "cannot read " + path; return false; }
    size_ = uint64_t(end);
    return parse(error);
}

bool Psarc::open_memory(std::vector<uint8_t> bytes, std::string &error) {
    memory_ = std::move(bytes);
    size_ = memory_.size();
    return parse(error);
}

bool Psarc::parse(std::string &error) {
    uint8_t h[32];
    if (!fetch(0, 32, h)) { error = "not an archive (too short)"; return false; }
    if (std::memcmp(h, "PSAR", 4) || be16(h + 4) != 1 || std::memcmp(h + 8, "zlib", 4) || be32(h + 16) != 30) {
        error = "not a zlib PSARC archive";
        return false;
    }
    const uint64_t toc = be32(h + 12), count = be32(h + 20);
    block_size_ = be32(h + 24);
    if (count < 1 || count > 1000000 || block_size_ < 2 || block_size_ > (1u << 24) || toc < 32 + 30 * count || toc > size_) {
        error = "damaged archive header";
        return false;
    }
    unsigned width = 0;
    for (uint64_t v = block_size_ - 1; v; v >>= 8) ++width;
    std::vector<uint8_t> t(size_t(toc - 32));
    if (!fetch(32, t.size(), t.data())) { error = "damaged archive table"; return false; }
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t *e = t.data() + 30 * i;
        entries_.push_back({be32(e + 16), be_n(e + 20, 5), be_n(e + 25, 5)});
    }
    for (size_t at = size_t(30 * count); at + width <= t.size(); at += width) {
        const uint64_t n = be_n(t.data() + at, width);
        blocks_.push_back(n ? n : block_size_);
    }
    std::vector<uint8_t> list;
    if (!read(0, list, error, 64u << 20)) return false;
    const std::string text(list.begin(), list.end());
    size_t from = 0;
    for (size_t at = 0; at < text.size(); ++at)
        if (text[at] == '\n') { names_.push_back(text.substr(from, at - from)); from = at + 1; }
    if (from < text.size()) names_.push_back(text.substr(from));
    if (names_.size() != entries_.size() - 1) { error = "archive names do not match its members"; return false; }
    return true;
}

bool Psarc::read(size_t index, std::vector<uint8_t> &out, std::string &error, size_t limit) const {
    if (index >= entries_.size()) { error = "no such archive member"; return false; }
    const Entry &e = entries_[index];
    const uint64_t want = std::min<uint64_t>(e.length, limit);
    if (want > (uint64_t(1) << 32)) { error = "archive member too large"; return false; }
    out.assign(size_t(want), 0);
    uint64_t done = 0, at = e.offset, block = e.block;
    std::vector<uint8_t> stored, plain;
    while (done < want) {
        if (block >= blocks_.size()) { error = "damaged archive member"; return false; }
        const uint64_t n = blocks_[block++], expected = std::min<uint64_t>(block_size_, e.length - done);
        stored.resize(size_t(n));
        if (!fetch(at, size_t(n), stored.data())) { error = "truncated archive member"; return false; }
        at += n;
        const size_t take = size_t(std::min<uint64_t>(expected, want - done));
        if (n == expected) {
            std::memcpy(out.data() + done, stored.data(), take);
        } else {
            plain.assign(size_t(expected), 0);
            if (!mag::zlib_decode(stored.data(), stored.size(), plain.data(), plain.size())) {
                error = "damaged compressed block";
                return false;
            }
            std::memcpy(out.data() + done, plain.data(), take);
        }
        done += take;
    }
    return true;
}

bool scan_archive(const Psarc &archive, bool loose, std::vector<Program> &out, ScanStats &stats, std::string &error) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < archive.names().size(); ++i) {
        const std::string &name = archive.names()[i];
        const bool block = ends_with(name, ".zrb");
        const bool file = loose && (ends_with(name, ".vbin") || ends_with(name, ".fbin") || ends_with(name, ".sho"));
        if (!block && !file) continue;
        if (block) {
            // The type table is at the start: most blocks hold no shader effects and are not read whole.
            if (!archive.read(i + 1, bytes, error, 4096)) return false;
            if (!resource_block_may_hold_effects(bytes.data(), bytes.size())) continue;
        }
        if (!archive.read(i + 1, bytes, error)) return false;
        stats.members++;
        if (block) scan_resource_block(bytes.data(), bytes.size(), out, stats);
        else scan_loose_file(bytes.data(), bytes.size(), out, stats);
    }
    return true;
}

void unique_programs(std::vector<Program> &programs) {
    std::set<std::tuple<bool, uint32_t, std::vector<uint8_t>>> seen;
    std::vector<Program> kept;
    for (auto &p : programs)
        if (seen.insert({p.vertex, p.control, p.ucode}).second) kept.push_back(std::move(p));
    programs = std::move(kept);
}

}  // namespace mag::shaders
