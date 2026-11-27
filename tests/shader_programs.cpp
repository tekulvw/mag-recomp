// Synthetic checks of the program finder (src/mag_shader_programs.h), driven by
// tests/test_shader_programs.py, which writes the archives. With an archive argument it prints
// every program found, one per line ("vp|fp <control> <hex>"), then the scan counts; without one
// it runs the checks that need no archive.
#include "mag_shader_programs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mag::shaders;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void put_fp_word(uint8_t *p, uint32_t host) {
    const uint32_t be = (host << 16) | (host >> 16);
    p[0] = uint8_t(be >> 24); p[1] = uint8_t(be >> 16); p[2] = uint8_t(be >> 8); p[3] = uint8_t(be);
}

static bool open_archive(Psarc &archive, int argc, char **argv, std::string &error) {
    if (argc == 3 && !std::strcmp(argv[1], "--memory")) {
        std::FILE *f = std::fopen(argv[2], "rb");
        std::vector<uint8_t> bytes;
        if (f) {
            int c;
            while ((c = std::fgetc(f)) != EOF) bytes.push_back(uint8_t(c));
            std::fclose(f);
        }
        return archive.open_memory(std::move(bytes), error);
    }
    return archive.open_file(argv[1], error);
}

int main(int argc, char **argv) {
    if (argc == 1) {
        // Two instructions, the first with an inline constant (a type-2 source in word 1).
        uint8_t fp[48] = {};
        put_fp_word(fp + 4, 2u);
        put_fp_word(fp + 32, 1u);  // END on the instruction after the constant
        CHECK(fragment_program_size(fp, sizeof fp) == 48);
        CHECK(fragment_program_size(fp, 47) == 0);  // cut short: no end within the bound
        uint8_t none[32] = {};
        CHECK(fragment_program_size(none, sizeof none) == 0);
        // A type table without shader effects is skipped; one with them, or a header that does not
        // parse, is read.
        uint8_t block[0x40] = {};
        block[7] = 0x30; block[0x2f] = 1;
        const uint8_t other[8] = {0x12, 0x34, 0x56, 0x78, 0, 0, 0, 3};
        std::memcpy(block + 0x30, other, 8);
        CHECK(!resource_block_may_hold_effects(block, sizeof block));
        const uint8_t effects[8] = {0xb6, 0x60, 0x91, 0xf2, 0, 0, 0, 2};
        std::memcpy(block + 0x30, effects, 8);
        CHECK(resource_block_may_hold_effects(block, sizeof block));
        block[0x2f] = 0xff; block[0x2e] = 0xff;
        CHECK(resource_block_may_hold_effects(block, sizeof block));
        // Duplicates go, the first of each stays.
        std::vector<Program> ps(3);
        ps[0].ucode = {1, 2}; ps[1].ucode = {1, 2}; ps[2].ucode = {1, 2}; ps[2].control = 0x40;
        unique_programs(ps);
        CHECK(ps.size() == 2 && ps[1].control == 0x40);
        std::printf(failures ? "[shader_programs] FAIL\n" : "[shader_programs] PASS\n");
        return failures ? 1 : 0;
    }
    Psarc archive;
    std::string error;
    if (!open_archive(archive, argc, argv, error)) { std::printf("error %s\n", error.c_str()); return 1; }
    std::vector<Program> programs;
    ScanStats stats;
    if (!scan_archive(archive, true, programs, stats, error)) { std::printf("error %s\n", error.c_str()); return 1; }
    for (const auto &p : programs) {
        std::printf("%s %08x ", p.vertex ? "vp" : "fp", p.control);
        for (uint8_t b : p.ucode) std::printf("%02x", b);
        std::printf("\n");
    }
    std::printf("stats members %zu blocks %zu effects %zu skipped %zu techniques %zu loose %zu vp %zu fp %zu\n",
                stats.members, stats.effect_blocks, stats.effects, stats.effects_skipped, stats.techniques,
                stats.loose_files, stats.vertex, stats.fragment);
    return 0;
}
