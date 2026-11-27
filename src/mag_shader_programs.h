#pragma once
/* The RSX programs MAG ships, found in the player's own game files without running the game
 * (tools: MagShaderWarm, src/mag_shader_warm.cpp; docs/shader-warm.md).
 *
 * Layout, in our own words, from inspecting the containers:
 *
 * - Archives are PSARC (Sony's public container): a 32-byte header ("PSAR", version 1.x,
 *   "zlib", TOC length, 30-byte entries, entry count, block size), the TOC entries (a 16-byte
 *   name digest, the first block index, a 40-bit length and a 40-bit offset), then one size per
 *   block, as wide as the block size needs. A block whose stored size equals its length is
 *   stored as it is; any other block is a zlib stream. Member 0 is the newline-separated list of
 *   the other members' names. game.psarc.sdat is the same archive inside an SDATA wrapper.
 * - A .zrb resource block starts with a header: the word at +4 is the offset of a type table,
 *   the word at +0x2c its entry count; each entry is a 32-bit type hash and a record count.
 *   Each record starts with the marker word 0x600df00d (not always word-aligned), and its type
 *   hash sits 0x1c bytes after the marker. Type 0xb66091f2 is a shader effect; the word at
 *   marker+0x28 is its technique count.
 * - Each technique holds a vertex program header (a 16-bit instruction count, a 16-bit zero,
 *   two words, a half-word and a half-word 0xffff), then within a few 16-byte steps the program:
 *   big-endian words, 16 bytes an instruction, the last with bit 0 of its fourth word set. After
 *   the program, possibly past one or two 16-byte blocks, comes a fragment program header: its
 *   byte size, a word, the SET_SHADER_CONTROL value, a word, and at +0x10 the distance from the
 *   header to the fragment program, which sits elsewhere in the record.
 * - Fragment programs are kept as the RSX reads them: each 32-bit word big-endian with its
 *   16-bit halves swapped. An instruction is 16 bytes; one with a source of type 2 (an inline
 *   constant) in its second, third or fourth word is followed by a 16-byte constant; the one
 *   with bit 0 of its first word set ends the program.
 * - Loose shader files (.vbin, .fbin, .sho) carry programs behind the same two header forms.
 *
 * Nothing here is game data: it finds programs in bytes it is given. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mag::shaders {

struct Program {
    bool vertex = false;
    /* Vertex: the instruction words as the guest uploads them (32-bit little-endian, as the
     * runtime's decompiler reads them). Fragment: the bytes as stored, inline constants in place. */
    std::vector<uint8_t> ucode;
    uint32_t control = 0;  /* fragment: the stored SET_SHADER_CONTROL value */
};

struct ScanStats {
    size_t members = 0;          /* archive members read whole */
    size_t effect_blocks = 0;    /* resource blocks with shader effects */
    size_t effects = 0;          /* effect records parsed */
    size_t effects_skipped = 0;  /* records whose techniques could not all be found */
    size_t techniques = 0;
    size_t loose_files = 0;      /* loose shader files with at least one program */
    size_t vertex = 0, fragment = 0;  /* programs found, before de-duplication */
};

/* Length of the fragment program at `p` (inline constants included) up to its end, or 0 when it
 * does not end within `limit` bytes. */
size_t fragment_program_size(const uint8_t *p, size_t limit);

/* The programs of every shader effect record in one resource block. */
void scan_resource_block(const uint8_t *data, size_t size, std::vector<Program> &out, ScanStats &stats);
/* Whether a resource block's type table lists shader effect records (true when the header does
 * not parse, so the caller reads it whole). Needs only the block's start. */
bool resource_block_may_hold_effects(const uint8_t *data, size_t size);
/* The programs in one loose shader file. */
void scan_loose_file(const uint8_t *data, size_t size, std::vector<Program> &out, ScanStats &stats);

/* A PSARC archive read from memory or from a file, member by member. */
class Psarc {
public:
    Psarc() = default;
    Psarc(const Psarc &) = delete;
    Psarc &operator=(const Psarc &) = delete;
    ~Psarc();
    bool open_file(const std::string &path, std::string &error);
    bool open_memory(std::vector<uint8_t> bytes, std::string &error);
    const std::vector<std::string> &names() const { return names_; }  /* member i + 1 */
    /* Member `index` (1-based, as names()[index - 1]), or at most its first `limit` bytes. */
    bool read(size_t index, std::vector<uint8_t> &out, std::string &error, size_t limit = SIZE_MAX) const;

private:
    struct Entry { uint64_t block, length, offset; };
    bool parse(std::string &error);
    bool fetch(uint64_t offset, size_t size, uint8_t *out) const;
    std::FILE *file_ = nullptr;
    std::vector<uint8_t> memory_;
    uint64_t size_ = 0, block_size_ = 0;
    std::vector<uint64_t> blocks_;
    std::vector<Entry> entries_;
    std::vector<std::string> names_;
};

/* Every program in one archive: the effect records of its .zrb members and, with `loose`, its
 * .vbin, .fbin and .sho members. */
bool scan_archive(const Psarc &archive, bool loose, std::vector<Program> &out, ScanStats &stats,
                  std::string &error);

/* Drops exact duplicates (same stage, bytes and control word), keeping the first. */
void unique_programs(std::vector<Program> &programs);

}  // namespace mag::shaders
