#pragma once
#include <filesystem>
#include <string>

namespace mag {
// np_ticket: the portal ticket profile (player_sign_in.h). Both phases then keep the game's NP
// ticket setting on, so the engine requests a ticket as the launcher does; otherwise the
// community no-ticket profile turns it off.
struct ServerChoice { std::string server_address; int tcp_port = 0, udp_port = 0; bool np_ticket = false; };
enum class OverlayStatus { NotNeeded, Ready, ArchiveError, Error };
struct OverlayResult { OverlayStatus status; std::filesystem::path root; std::string message; };

// The portable counterpart of the macOS server overlay. With a server chosen, builds (or reuses)
// <owned_root>/overlay: a folder with the game root's layout that holds only the settings archive,
// rewritten so the retail settings member (identified by expected_member_sha256) ends with the
// connection override. The runtime reads that one file from the overlay instead of the game root
// (PS3_VFS_OVERLAY, docs/player-settings.md), so the game files are never mirrored or changed.
// Ready returns the overlay folder in root; NotNeeded (no server) returns an empty root.
// ArchiveError means the game's own archive is missing, unreadable or not the retail version.
OverlayResult create_server_overlay(const std::filesystem::path &source_root, const std::filesystem::path &owned_root,
                                    const ServerChoice &choice, const std::string &expected_member_sha256);

// Earlier versions kept a whole view of the game folder in <owned_root>/vfs: real folders, hard
// links to the game files and the rewritten archive (and .vfs-* stages a stopped launch left).
// Removes each such tree once every file in it is shown to be the game file at the same place
// under source_root (a hard link), the view's own archive, or a file with no other name (what the
// game wrote into the view, or a copy); anything else (a symbolic link, a link to some other file,
// the game folder itself) leaves that tree in place. Returns what it did for the log, or "" when
// there was no old view.
std::string remove_legacy_server_view(const std::filesystem::path &source_root, const std::filesystem::path &owned_root);

// The Lua appended to the retail settings member for this server (shared with tests).
std::string server_override_text(const std::string &ipv4, int tcp_port, int udp_port, bool np_ticket = false);
}
