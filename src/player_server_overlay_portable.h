#pragma once
#include <filesystem>
#include <string>

namespace mag {
struct ServerChoice { std::string server_address; int tcp_port = 0, udp_port = 0; };
enum class OverlayStatus { NotNeeded, Ready, ArchiveError, Error };
struct OverlayResult { OverlayStatus status; std::filesystem::path root; std::string message; };

// The portable counterpart of the macOS server overlay. With a server chosen, builds (or reuses)
// <owned_root>/vfs: a view of source_root whose settings archive appends the connection override
// to the retail settings member, identified by expected_member_sha256. Every other file is a hard
// link to the original, so the view costs no copy and removing it never touches the game files.
// ArchiveError means the game's own archive is missing, unreadable or not the retail version.
OverlayResult create_server_overlay(const std::filesystem::path &source_root, const std::filesystem::path &owned_root,
                                    const ServerChoice &choice, const std::string &expected_member_sha256);

// The Lua appended to the retail settings member for this server (shared with tests).
std::string server_override_text(const std::string &ipv4, int tcp_port, int udp_port);
}
