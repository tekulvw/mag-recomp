// Synthetic driver for the portable player settings, overlay, decoder and digest.
#include "player_inflate.h"
#include "player_settings_portable.h"
#include "player_settings_runtime.h"
#include "player_server_overlay_portable.h"
#include "player_sha256.h"
#include "player_sign_in.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

// The settings window (player_settings_session.cpp) is not part of this driver: a stand-in reports
// when the launch would open it, and may play the player saving a file.
extern "C" int mag_player_settings_setup_wanted(int settings_exist) {
    const char *skip = std::getenv("MAG_SKIP_SETUP");  // as player_settings_session.cpp reads it
    return !(skip && *skip && std::string(skip) != "0") && !settings_exist;
}
extern "C" int mag_player_settings_setup(void) {
    std::cerr << "[driver] settings window opened\n";
    const char *saved = std::getenv("MAG_TEST_WINDOW_SAVES");
    if (!saved) return 0;
    std::ofstream(std::getenv("MAG_PLAYER_SETTINGS_PATH")) << saved;
    return 1;
}

static std::vector<uint8_t> load(const char *path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    std::string mode = argv[1];
    if (mode == "inflate" && argc == 4) {  // inflate <zlib file> <expected size>: decoded bytes on stdout
        auto in = load(argv[2]); std::vector<uint8_t> out(std::stoul(argv[3]));
        if (!mag::zlib_decode(in.data(), in.size(), out.data(), out.size())) return 1;
        std::cout.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
        return 0;
    }
    if (mode == "sha256" && argc == 3) { auto in = load(argv[2]); std::cout << mag::sha256_hex(in.data(), in.size()) << '\n'; return 0; }
    if (mode == "parse" && argc == 3) {
        auto in = load(argv[2]); mag::PortableSettings settings; std::string error;
        if (!mag::parse_portable_settings(std::string(in.begin(), in.end()), settings, error)) { std::cerr << error << '\n'; return 1; }
        std::cout << mag::format_portable_settings(settings); return 0;
    }
    if (mode == "overlay" && argc == 6) {  // overlay <vfs> <owned> <server> <member sha>
        auto result = mag::create_server_overlay(argv[2], argv[3], {argv[4], 54321, 0}, argv[5]);
        std::cout << int(result.status) << '\n' << result.root.string() << '\n' << result.message << '\n';
        return result.status == mag::OverlayStatus::Ready || result.status == mag::OverlayStatus::NotNeeded ? 0 : 1;
    }
    if (mode == "sign-in" && argc == 4) {  // sign-in <player name> <ticket file or "">: the server profile
        for (const auto &[name, value] : mag::server_sign_in_environment(argv[2], argv[3])) std::cout << name << '=' << value << '\n';
        return 0;
    }
    if (mode == "prepare") {
        int result = mag_player_settings_prepare();
        for (const char *name : {"PS3_VFS_ROOT", "PS3_NET_ONLINE", "MAG_LOCAL_NO_TICKET", "PS3_NP_ONLINE_ID", "PS3RECOMP_OSK_INITIAL_TEXT", "MAG_LOCAL_ACCOUNT_NAME",
                                 "PS3RECOMP_NP_LOCAL_TICKET", "PS3RECOMP_NP_TICKET_FILE", "PS3RECOMP_VK_FULLSCREEN",
                                 "MAG_PLAYER_OUTPUT_SIZE"}) {
            const char *value = std::getenv(name);
            std::cout << name << '=' << (value ? value : "") << '\n';
        }
        return result == 1 ? 0 : 10 + result;
    }
    return 2;
}
