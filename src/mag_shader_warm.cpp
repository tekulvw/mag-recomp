// MagShaderWarm: translate MAG's shaders into the player's SPIR-V cache before the first session
// (docs/shader-warm.md).
//
// The draw engine translates each RSX program pair to SPIR-V with glslang the first time a draw
// needs it, about 10 ms a shader, on the thread that walks the command buffer; the files in
// PS3RECOMP_SPIRV_CACHE (src/mag_shader_cache.h) spare later sessions. This tool, built with MAG
// from the same runtime, finds the programs in the player's own game files
// (src/mag_shader_programs.h), describes the draw states a first session is likely to use, and
// has the runtime translate each one (rsx_shader_prewarm.h) through the engine's own text
// builders, so the files are the ones the engine will look for. Several processes share the work.
//
// Everything it writes is derived from the player's game files and stays in the player's cache
// folder; it is never shipped.
//
//   MagShaderWarm --game <PS3_GAME folder> [--cache <folder>] [--jobs N] [--dry-run]
//
// Without --cache the folder is chosen as MAG chooses it (choose_shader_cache): a
// PS3RECOMP_SPIRV_CACHE the player set, else %LOCALAPPDATA%\MAG\cache\shaders; MAG_SHADER_CACHE=0
// skips the warm-up. It prints one summary line, "[shader-warm] ...", and exits 0 unless it could
// not run at all.
#include "mag_shader_cache.h"
#include "mag_shader_programs.h"

#include "rsx_fp_decompiler.h"
#include "rsx_shader_prewarm.h"
#include "rsx_shader_spirv.h"
#include "rsx_vp_decompiler.h"
#include "sdata_decrypt.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;
using mag::shaders::Program;

namespace {

/* What each program is warmed for, besides its neutral state. The choices follow what a first
 * MAG session draws (docs/shader-warm.md): the ambient cube map on unit 14 for programs that sample
 * it, sRGB export, and an sRGB material texture on unit 0. */
enum : uint32_t { kCube14 = 1u, kSrgbExport = 2u, kGamma0 = 4u };

struct Item {
    uint32_t program = 0;  /* index into the program list */
    uint32_t variant = 0;
    uint64_t name = 0;     /* the cache name its text has */
};

constexpr const char *kManifest = "shader-warm.txt";
constexpr const char *kTag = "[shader-warm]";

rsx_fp_text_state fragment_state(const Program &p, uint32_t variant) {
    rsx_fp_text_state st;
    std::memset(&st, 0, sizeof st);
    st.ctrl = p.control;
    if (variant & kCube14) st.cube_mask = 1u << 14;
    if (variant & kSrgbExport) st.srgb_output = 1;
    if (variant & kGamma0) st.gamma_masks[0] = 7;  // sRGB decode of R, G and B
    return st;
}

std::vector<uint32_t> fragment_variants(const Program &p) {
    const uint32_t units = rsx_fp_texture_units(p.ucode.data(), uint32_t(p.ucode.size()));
    std::vector<uint32_t> cube = {0}, srgb = {0}, gamma = {0};
    if (units & (1u << 14)) cube.push_back(kCube14);
    // An A8R8G8B8 target; sRGB export does not apply to 32-bit exports.
    if (rsx_fp_srgb_output_enabled(1, p.control, 0x08)) srgb.push_back(kSrgbExport);
    if (units & 1u) gamma.push_back(kGamma0);
    std::vector<uint32_t> out;
    for (uint32_t c : cube) for (uint32_t s : srgb) for (uint32_t g : gamma) out.push_back(c | s | g);
    return out;
}

/* The cache name of a program's text in a variant, or 0 when it does not decompile. */
uint64_t text_name(const Program &p, uint32_t variant, std::vector<char> &buffer) {
    const uint32_t size = uint32_t(p.ucode.size());
    if (p.vertex) {
        const uint32_t bytes = rsx_vp_program_size_instrs(p.ucode.data(), size) * 16u;
        if (rsx_vp_build_text(p.ucode.data(), bytes, 0, 0, 0xFFFFu, buffer.data(), uint32_t(buffer.size())) <= 0) return 0;
        return rsx_hlsl_to_spirv_cache_name(buffer.data(), RSX_SHADER_STAGE_VERTEX);
    }
    const rsx_fp_text_state st = fragment_state(p, variant);
    if (rsx_fp_build_text(p.ucode.data(), size, &st, buffer.data(), uint32_t(buffer.size()), nullptr) <= 0) return 0;
    return rsx_hlsl_to_spirv_cache_name(buffer.data(), RSX_SHADER_STAGE_FRAGMENT);
}

std::string hex_name(uint64_t name) {
    char s[24];
    std::snprintf(s, sizeof s, "%016llx", (unsigned long long)name);
    return s;
}

bool read_file(const fs::path &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

/* ---- the work file a parent hands its workers ----------------------------------------------- */

void put32(std::vector<uint8_t> &b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void put64(std::vector<uint8_t> &b, uint64_t v) { put32(b, uint32_t(v)); put32(b, uint32_t(v >> 32)); }
uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t(get32(p + 4)) << 32); }

constexpr char kWorkMagic[8] = {'M', 'A', 'G', 'S', 'H', 'W', '1', '\n'};

bool write_work(const fs::path &path, const std::vector<Program> &programs, const std::vector<Item> &items) {
    std::vector<uint8_t> b(kWorkMagic, kWorkMagic + 8);
    put32(b, uint32_t(programs.size()));
    for (const auto &p : programs) {
        b.push_back(p.vertex ? 1 : 0);
        put32(b, p.control);
        put32(b, uint32_t(p.ucode.size()));
        b.insert(b.end(), p.ucode.begin(), p.ucode.end());
    }
    put32(b, uint32_t(items.size()));
    for (const auto &it : items) { put32(b, it.program); put32(b, it.variant); put64(b, it.name); }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
    return bool(f);
}

bool read_work(const fs::path &path, std::vector<Program> &programs, std::vector<Item> &items) {
    std::vector<uint8_t> b;
    if (!read_file(path, b) || b.size() < 12 || std::memcmp(b.data(), kWorkMagic, 8)) return false;
    size_t at = 8;
    auto need = [&](size_t n) { return at + n <= b.size(); };
    const uint32_t np = get32(&b[at]);
    at += 4;
    for (uint32_t i = 0; i < np; ++i) {
        if (!need(9)) return false;
        Program p;
        p.vertex = b[at] != 0;
        p.control = get32(&b[at + 1]);
        const uint32_t n = get32(&b[at + 5]);
        at += 9;
        if (!need(n)) return false;
        p.ucode.assign(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + n));
        at += n;
        programs.push_back(std::move(p));
    }
    if (!need(4)) return false;
    const uint32_t ni = get32(&b[at]);
    at += 4;
    for (uint32_t i = 0; i < ni; ++i) {
        if (!need(16)) return false;
        Item it{get32(&b[at]), get32(&b[at + 4]), get64(&b[at + 8])};
        at += 16;
        if (it.program >= programs.size()) return false;
        items.push_back(it);
    }
    return true;
}

/* ---- worker -------------------------------------------------------------------------------- */

int run_worker(const fs::path &work, size_t first, size_t step, const fs::path &result) {
    std::vector<Program> programs;
    std::vector<Item> items;
    if (!read_work(work, programs, items) || !step) return 2;
    size_t translated = 0, on_disk = 0, failed = 0, renamed = 0;
    std::string first_failure;
    for (size_t i = first; i < items.size(); i += step) {
        const Item &it = items[i];
        const Program &p = programs[it.program];
        rsx_shader_prewarm_result r;
        int rc;
        if (p.vertex) {
            rc = rsx_shader_prewarm_vertex(p.ucode.data(), uint32_t(p.ucode.size()), 0, 0, &r);
        } else {
            const rsx_fp_text_state st = fragment_state(p, it.variant);
            rc = rsx_shader_prewarm_fragment(p.ucode.data(), uint32_t(p.ucode.size()), &st, &r);
        }
        if (rc == RSX_PREWARM_FAILED) {
            if (first_failure.empty()) first_failure = r.log;
            failed++;
            continue;
        }
        if (r.name != it.name) renamed++;  // the parent's name and the warm-up's disagree: a bug, reported
        (rc == RSX_PREWARM_TRANSLATED ? translated : on_disk)++;
    }
    std::ofstream f(result, std::ios::trunc);
    f << translated << ' ' << on_disk << ' ' << failed << ' ' << renamed << '\n' << first_failure << '\n';
    return f ? 0 : 2;
}

/* ---- parent -------------------------------------------------------------------------------- */

#ifdef _WIN32
std::wstring quoted(const std::wstring &arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { slashes++; continue; }
        out.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(2 * slashes, L'\\');
    return out + L"\"";
}
#endif

/* Runs `jobs` workers over the work file and waits for them. False when one could not start. */
bool run_workers(const std::string &self, const fs::path &work, size_t jobs, std::string &error) {
#ifdef _WIN32
    (void)self;
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]));
    if (!n || n >= sizeof exe / sizeof exe[0]) { error = "cannot find this program's path"; return false; }
    std::vector<HANDLE> running;
    for (size_t k = 0; k < jobs; ++k) {
        std::wstring command = quoted(exe) + L" --worker " + quoted(work.wstring()) + L" " +
                               std::to_wstring(k) + L" " + std::to_wstring(jobs);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        // Below normal: the computer stays usable while the shaders translate.
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                            nullptr, nullptr, &si, &pi)) {
            error = "cannot start a worker (error " + std::to_string(GetLastError()) + ")";
            break;
        }
        CloseHandle(pi.hThread);
        running.push_back(pi.hProcess);
    }
    for (HANDLE h : running) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    return error.empty();
#else
    std::vector<pid_t> running;
    const std::string w = work.string(), step = std::to_string(jobs);
    for (size_t k = 0; k < jobs; ++k) {
        const std::string first = std::to_string(k);
        std::vector<char *> argv = {const_cast<char *>(self.c_str()), const_cast<char *>("--worker"),
                                    const_cast<char *>(w.c_str()), const_cast<char *>(first.c_str()),
                                    const_cast<char *>(step.c_str()), nullptr};
        pid_t pid = 0;
        if (posix_spawn(&pid, self.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
            error = "cannot start a worker";
            break;
        }
        running.push_back(pid);
    }
    for (pid_t pid : running) { int status = 0; waitpid(pid, &status, 0); }
    return error.empty();
#endif
}

struct Options {
    std::string game, cache, self;
    size_t jobs = 0;
    bool dry_run = false;
};

/* The folder, as MAG's engine would choose it, or empty with the reason in `why`. */
std::string cache_folder(const Options &o, std::string &why) {
    if (!o.cache.empty()) { why = "shader cache: " + o.cache + " (--cache)"; return o.cache; }
    auto make_dir = [](const std::string &dir) {
        std::error_code error;
        fs::create_directories(dir, error);
        return fs::is_directory(dir, error);
    };
    const char *explicit_cache = std::getenv("PS3RECOMP_SPIRV_CACHE");
    const mag::ShaderCacheChoice c = mag::choose_shader_cache(explicit_cache, std::getenv("MAG_SHADER_CACHE"),
                                                              std::getenv("LOCALAPPDATA"), make_dir);
    why = c.why;
    if (!c.dir.empty()) return c.dir;
    if (explicit_cache && *explicit_cache) return explicit_cache;  // the engine uses it as it is
    return std::string();
}

bool find_programs(const fs::path &game, std::vector<Program> &programs, mag::shaders::ScanStats &stats, std::string &error) {
    const fs::path base = game / "USRDIR/run/mag/ps3";
    std::vector<uint8_t> sealed;
    if (!read_file(base / "game.psarc.sdat", sealed)) { error = "cannot read " + (base / "game.psarc.sdat").string(); return false; }
    std::vector<uint8_t> bytes;
    if (sealed.size() >= 4 && !std::memcmp(sealed.data(), "PSAR", 4)) {
        bytes = std::move(sealed);  // already an archive (the synthetic tests' game folders)
    } else {
        size_t plain_size = 0;
        uint8_t *plain = sdata_decrypt(sealed.data(), sealed.size(), &plain_size);
        if (!plain) { error = "game.psarc.sdat did not decrypt"; return false; }
        bytes.assign(plain, plain + plain_size);
        std::free(plain);
        sealed.clear();
    }
    mag::shaders::Psarc game_archive;
    if (!game_archive.open_memory(std::move(bytes), error) ||
        !mag::shaders::scan_archive(game_archive, true, programs, stats, error)) {
        error = "game.psarc.sdat: " + error;
        return false;
    }
    mag::shaders::Psarc main_archive;
    if (!main_archive.open_file((base / "main.psarc").string(), error) ||
        !mag::shaders::scan_archive(main_archive, false, programs, stats, error)) {
        error = "main.psarc: " + error;
        return false;
    }
    mag::shaders::unique_programs(programs);
    return true;
}

/* Files the runtime would never read again: another translator's, and names the previous warm-up
 * wrote that this one does not (an older runtime's text). Files the engine wrote itself stay. */
size_t prune(const fs::path &dir, const std::set<uint64_t> &names) {
    size_t removed = 0;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(dir, error)) {
        const std::string file = entry.path().filename().string();
        if (file.size() != 20 || file.compare(16, 4, ".spv") != 0) continue;
        if (rsx_hlsl_to_spirv_cache_file_current(entry.path().string().c_str()) == 0 && fs::remove(entry.path(), error))
            removed++;
    }
    std::ifstream previous(dir / kManifest);
    std::string line;
    while (std::getline(previous, line)) {
        if (line.size() != 16 || line.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        if (names.count(std::strtoull(line.c_str(), nullptr, 16))) continue;
        const fs::path file = dir / (line + ".spv");
        if (fs::exists(file, error) && fs::remove(file, error)) removed++;
    }
    return removed;
}

unsigned long this_process() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

int run(const Options &o) {
    const auto t0 = std::chrono::steady_clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    std::string why;
    const std::string dir = cache_folder(o, why);
    if (dir.empty()) {
        std::printf("%s skipped: %s\n", kTag, why.c_str());
        return 0;
    }
    std::error_code fs_error;
    if (!fs::is_directory(dir, fs_error)) {
        std::printf("%s skipped: %s is not a folder\n", kTag, dir.c_str());
        return 0;
    }
    set_env("PS3RECOMP_SPIRV_CACHE", dir);  // the runtime's translator, here and in the workers

    std::vector<Program> programs;
    mag::shaders::ScanStats stats;
    std::string error;
    if (!find_programs(o.game, programs, stats, error)) {
        std::printf("%s failed: %s\n", kTag, error.c_str());
        return 1;
    }
    size_t vertex = 0;
    for (const auto &p : programs) vertex += p.vertex;
    const double found_at = seconds();

    // Every text once; those already in the cache need no worker.
    std::vector<char> buffer(RSX_FP_HLSL_BUFFER_SIZE);
    std::vector<Item> todo;
    std::set<uint64_t> names;
    size_t present = 0, undecodable = 0;
    for (uint32_t i = 0; i < programs.size(); ++i) {
        const auto variants = programs[i].vertex ? std::vector<uint32_t>{0} : fragment_variants(programs[i]);
        for (uint32_t v : variants) {
            const uint64_t name = text_name(programs[i], v, buffer);
            if (!name) { undecodable++; continue; }
            if (!names.insert(name).second) continue;
            const fs::path file = fs::path(dir) / (hex_name(name) + ".spv");
            if (rsx_hlsl_to_spirv_cache_file_current(file.string().c_str()) == 1) { present++; continue; }
            todo.push_back({i, v, name});
        }
    }
    if (o.dry_run) {
        for (uint64_t n : names) std::printf("%s.spv\n", hex_name(n).c_str());
        std::printf("%s dry run: %zu programs (%zu vertex, %zu fragment), %zu texts, %zu already cached, %zu to translate\n",
                    kTag, programs.size(), vertex, programs.size() - vertex, names.size(), present, todo.size());
        return 0;
    }

    size_t translated = 0, on_disk = 0, failed = 0, renamed = 0, jobs = 0;
    std::string first_failure;
    if (!todo.empty()) {
        const size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
        jobs = o.jobs ? o.jobs : std::min<size_t>(6, cores > 2 ? cores - 1 : 1);
        jobs = std::max<size_t>(1, std::min(jobs, (todo.size() + 31) / 32));
        const fs::path work = fs::path(dir) / ("shader-warm-work-" + std::to_string(this_process()) + ".bin");
        // The work file holds game-derived programs: it lives in the player's cache folder for the
        // workers to read and is removed when they are done.
        if (!write_work(work, programs, todo)) {
            std::printf("%s failed: cannot write %s\n", kTag, work.string().c_str());
            return 1;
        }
        const bool started = run_workers(o.self, work, jobs, error);
        for (size_t k = 0; k < jobs; ++k) {
            const fs::path result = fs::path(work.string() + "." + std::to_string(k));
            {
                std::ifstream f(result);
                size_t t = 0, d = 0, x = 0, r = 0;
                if (f >> t >> d >> x >> r) {
                    translated += t; on_disk += d; failed += x; renamed += r;
                    std::string line;
                    std::getline(f, line);
                    if (std::getline(f, line) && first_failure.empty()) first_failure = line;
                } else {
                    failed += (todo.size() + jobs - 1 - k) / jobs;  // that worker's share
                }
            }
            fs::remove(result, fs_error);
        }
        fs::remove(work, fs_error);
        if (!started) std::printf("%s warning: %s\n", kTag, error.c_str());
    }
    const size_t pruned = prune(dir, names);
    {
        std::ofstream manifest(fs::path(dir) / kManifest, std::ios::trunc);
        for (uint64_t n : names) manifest << hex_name(n) << '\n';
    }
    std::printf("%s %zu programs (%zu vertex, %zu fragment) found in %.1f s; %zu texts: %zu translated, %zu already cached, "
                "%zu failed; %zu stale files removed; %.1f s with %zu processes; %s\n",
                kTag, programs.size(), vertex, programs.size() - vertex, found_at, names.size(), translated,
                present + on_disk, failed + undecodable, pruned, seconds(), jobs, dir.c_str());
    if (renamed) std::printf("%s warning: %zu texts were named differently by the runtime\n", kTag, renamed);
    if (!first_failure.empty()) std::printf("%s first failure: %s\n", kTag, first_failure.c_str());
    if (stats.effects_skipped) std::printf("%s note: %zu shader effects could not be read\n", kTag, stats.effects_skipped);
    return 0;
}

int usage() {
    std::fprintf(stderr, "usage: MagShaderWarm --game <PS3_GAME folder> [--cache <folder>] [--jobs N] [--dry-run]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 5 && !std::strcmp(argv[1], "--worker")) {
        const size_t first = std::strtoul(argv[3], nullptr, 10), step = std::strtoul(argv[4], nullptr, 10);
        return run_worker(argv[2], first, step, std::string(argv[2]) + "." + argv[3]);
    }
    Options o;
    o.self = argv[0];
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) o.game = argv[++i];
        else if (a == "--cache" && i + 1 < argc) o.cache = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) o.jobs = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--dry-run") o.dry_run = true;
        else return usage();
    }
    if (o.game.empty()) return usage();
    return run(o);
}
