#pragma once
// How a launch on a server signs in, shared by the macOS and portable players.
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mag {
using EnvironmentList = std::vector<std::pair<std::string, std::string>>;

// What a launch on a server exports once its connection overlay is ready.
//  * No ticket file: the community no-ticket profile. The existing retail compatibility hook
//    restores the configured UseNPTicket=0, and the launcher signs in with the player's name.
//  * A ticket file (opt-in): the portal ticket profile. The game keeps the NP ticket setting its
//    own init turns on, asks for a ticket as on a PS3, and the runtime's local provider returns the
//    file's exact bytes (PS3RECOMP_NP_TICKET_FILE). The name stays the online ID (PS3_NP_ONLINE_ID,
//    exported with the player name), which the provider requires the game to ask for.
inline EnvironmentList server_sign_in_environment(const std::string &player_name, const std::string &ticket_file) {
    if (ticket_file.empty())
        return {{"PS3_NET_ONLINE", "1"}, {"MAG_LOCAL_NO_TICKET", "1"}, {"MAG_LOCAL_ACCOUNT_NAME", player_name}};
    return {{"PS3_NET_ONLINE", "1"}, {"PS3RECOMP_NP_LOCAL_TICKET", "1"}, {"PS3RECOMP_NP_TICKET_FILE", ticket_file}};
}

// The optional ticket_file setting: empty (no ticket), or an absolute path in UTF-8 of at most
// 1024 bytes without control characters. The file itself is read only when the game signs in.
inline bool check_ticket_file(const std::string &path, std::string &error) {
    if (path.empty()) return true;
    bool ok = path.size() <= 1024;
    for (unsigned char c : path) ok = ok && c >= 0x20 && c != 0x7f;
    if (ok) ok = std::filesystem::path(std::u8string(path.begin(), path.end())).is_absolute();
    if (!ok) error = "The ticket file must be an absolute path of at most 1024 bytes.";
    return ok;
}
}
