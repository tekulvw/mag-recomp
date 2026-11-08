#include "player_server_overlay_portable.h"
#include "player_archive.h"
#include "player_sha256.h"
#include "sdata_decrypt.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace fs = std::filesystem;

namespace {
const char *const archive_path = "PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat";
const char *const settings_member = "/run/mag/ps3/settings/maglocalsettings.lua";
const uint64_t archive_limit = 512ull * 1024 * 1024;

struct ArchiveProblem : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
void require_archive(bool ok, const std::string &message) { if (!ok) throw ArchiveProblem(message); }

std::vector<uint8_t> read_all(const fs::path &path, uint64_t limit) {
    std::error_code error;
    require(fs::is_regular_file(fs::symlink_status(path, error)), "Not an ordinary file: " + path.string());
    auto size = fs::file_size(path, error);
    require(!error && size <= limit, "File is unreadable or too large: " + path.string());
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(size);
    require(in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size)) && in.gcount() == std::streamsize(size),
            "Cannot read " + path.string());
    return bytes;
}

void write_new(const fs::path &path, const std::string &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    out.close();
    require(bool(out), "Cannot write " + path.string());
}

std::string sha(const std::vector<uint8_t> &bytes) { return mag::sha256_hex(bytes.data(), bytes.size()); }

std::string ipv4(const std::string &host) {
#ifdef _WIN32
    WSADATA wsa;
    require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Networking is unavailable.");
    struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
#endif
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) == 1) return host;
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    require(getaddrinfo(host.c_str(), nullptr, &hints, &found) == 0 && found,
            "The server hostname could not be resolved to IPv4: " + host);
    address = reinterpret_cast<sockaddr_in *>(found->ai_addr)->sin_addr;
    char text[INET_ADDRSTRLEN] = {};
    const char *converted = inet_ntop(AF_INET, &address, text, sizeof text);
    freeaddrinfo(found);
    require(converted != nullptr, "The server address could not be converted to IPv4.");
    return text;
}

std::string identity(const fs::path &source, const mag::ServerChoice &choice, const std::string &member) {
    return "source=" + source.generic_string() + "\nserver_address=" + choice.server_address +
           "\ntcp_port=" + std::to_string(choice.tcp_port) + "\nudp_port=" + std::to_string(choice.udp_port) +
           "\nmember_sha256=" + member + "\nconnection_profile=community-no-ticket-v1\n";
}

std::string read_text(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error))) return {};
    auto bytes = read_all(path, 65536);
    return std::string(bytes.begin(), bytes.end());
}

// A directory tree of real directories and hard links: removing it never follows into the game files.
void mirror(const fs::path &source, const fs::path &target, const fs::path &skip) {
    for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator(); ++it) {
        const auto relative = it->path().lexically_relative(source);
        const auto destination = target / relative;
        const auto status = it->symlink_status();
        if (fs::is_directory(status)) { fs::create_directory(destination); continue; }
        if (relative == skip) continue;
        require(fs::is_regular_file(status), "The game folder holds an unsupported entry: " + relative.generic_string());
        std::error_code error;
        fs::create_hard_link(it->path(), destination, error);
        require(!error, "The private game view could not be linked (" + error.message() +
                        "). Keep MAG and its game files on the same drive.");
    }
}
}

std::string mag::server_override_text(const std::string &host, int tcp_port, int udp_port) {
    std::string text = "\n-- MAG player connection override\nMAGClientNetConfig = MAGClientNetConfig or {}\n"
                       "MAGClientNetConfig.EnableNetwork = true\nMAGClientNetConfig.MAPSServerAddr = \"" + host + "\"\n"
                       "MAGClientNetConfig.EnableServerOnClient = false\nMAGClientNetConfig.UseTestClient = false\n"
                       "MAGClientNetConfig.UseNPTicket = false\n";
    if (tcp_port) text += "MAGClientNetConfig.MAPSServerTCPPort = " + std::to_string(tcp_port) + "\n";
    if (udp_port) text += "MAGClientNetConfig.MAPSServerUDPPort = " + std::to_string(udp_port) + "\n";
    return text;
}

mag::OverlayResult mag::create_server_overlay(const fs::path &source_root, const fs::path &owned_root,
                                              const ServerChoice &choice, const std::string &expected_member_sha256) {
    if (choice.server_address.empty()) return {OverlayStatus::NotNeeded, source_root, {}};
    fs::path stage;
    try {
        require(!source_root.empty() && !owned_root.empty(), "The private server overlay location is unavailable.");
        const auto source = fs::absolute(source_root).lexically_normal();
        const auto destination = owned_root / "vfs", marker = owned_root / "server-overlay.txt";
        const auto relative_archive = fs::path(archive_path);
        const auto wanted = identity(source, choice, expected_member_sha256);
        std::error_code error;
        fs::create_directories(owned_root, error);
        require(!error && fs::is_directory(fs::symlink_status(owned_root)), "The private server overlay folder could not be created.");
        const auto existing = fs::symlink_status(destination, error);
        if (fs::exists(existing)) {
            require(fs::is_directory(existing), "The private server overlay is not an ordinary folder.");
            const auto text = read_text(marker);
            const auto target = destination / relative_archive;
            if (text.rfind(wanted, 0) == 0 && fs::is_regular_file(fs::symlink_status(target, error)) &&
                text.substr(wanted.size()) == "archive_sha256=" + sha(read_all(target, archive_limit)) + "\n")
                return {OverlayStatus::Ready, destination, {}};
            // A view for other settings, or a changed one: it holds only folders and links, so drop it.
            fs::remove(marker, error);
            fs::remove_all(destination);
        }
        const auto archive_file = source / relative_archive;
        require_archive(fs::is_regular_file(fs::symlink_status(archive_file, error)), "The game settings archive is missing.");
        std::vector<uint8_t> raw;
        try { raw = read_all(archive_file, archive_limit); }
        catch (const std::exception &failure) { throw ArchiveProblem(failure.what()); }
        std::vector<uint8_t> archive;
        if (raw.size() >= 4 && !std::memcmp(raw.data(), "NPD\0", 4)) {
            size_t length = 0;
            uint8_t *plain = sdata_decrypt(raw.data(), raw.size(), &length);
            require_archive(plain && length <= archive_limit, "The game settings archive could not be decoded.");
            archive.assign(plain, plain + length); free(plain);
        } else archive = std::move(raw);
        std::vector<uint8_t> original;
        try { original = mag::read_psarc_member(archive, settings_member); }
        catch (const std::exception &failure) { throw ArchiveProblem(failure.what()); }
        require_archive(sha(original) == expected_member_sha256,
                        "The settings archive does not match the supported retail version. The original was kept.");
        const auto text = server_override_text(ipv4(choice.server_address), choice.tcp_port, choice.udp_port);
        auto replacement = original;
        replacement.insert(replacement.end(), text.begin(), text.end());
        auto changed = mag::rewrite_psarc_member(archive, settings_member, replacement);

        std::random_device random;
        stage = owned_root / (".vfs-" + std::to_string(random()) + std::to_string(random()));
        require(fs::create_directory(stage), "The private server overlay could not be staged.");
        mirror(source, stage, relative_archive);
        write_new(stage / relative_archive, std::string(changed.bytes.begin(), changed.bytes.end()));
        write_new(marker, wanted + "archive_sha256=" + sha(changed.bytes) + "\n");
        fs::rename(stage, destination);
        stage.clear();
        return {OverlayStatus::Ready, destination, {}};
    } catch (const ArchiveProblem &failure) {
        if (!stage.empty()) { std::error_code ignored; fs::remove_all(stage, ignored); }
        return {OverlayStatus::ArchiveError, {}, failure.what()};
    } catch (const std::exception &failure) {
        if (!stage.empty()) { std::error_code ignored; fs::remove_all(stage, ignored); }
        return {OverlayStatus::Error, {}, failure.what()};
    }
}
