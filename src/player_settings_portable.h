#pragma once
#include <filesystem>
#include <string>

namespace mag {
struct PlayerSettings {
  int schema_version = 1;
  std::string server_address = "play.magtest.online";
  std::string local_account_name;
  int tcp_port = 0, udp_port = 0;
  std::string display_mode = "windowed";
  int output_width = 1280, output_height = 720, render_scale = 100;
  std::string anti_aliasing = "off";
};
// All failures throw std::runtime_error; loading missing preferences returns
// defaults.
PlayerSettings validate_player_settings(PlayerSettings settings);
PlayerSettings load_player_settings(const std::filesystem::path &path,
                                    bool &exists);
void save_player_settings(const std::filesystem::path &path,
                          const PlayerSettings &settings);
std::filesystem::path
create_server_overlay(const std::filesystem::path &sourceRoot,
                      const std::filesystem::path &ownedRoot,
                      const PlayerSettings &settings,
                      const std::string &expectedMemberSHA256);
} // namespace mag
