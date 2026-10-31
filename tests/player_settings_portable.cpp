#include "player_settings_portable.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
#ifdef MAG_TEST_PORTABLE_OVERLAY
  if (argc >= 5) {
    mag::PlayerSettings settings;
    settings.server_address = "127.0.0.1";
    settings.tcp_port = 54321;
    settings.udp_port = 54322;
    if (argc == 6)
      settings.server_address = "";
    try {
      std::cout << mag::create_server_overlay(argv[1], argv[2], settings,
                                              argv[3])
                       .string()
                << '\n';
      return 0;
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
      return 1;
    }
  }
#endif
  assert(argc == 2);
  std::filesystem::path root = argv[1], file = root / "settings.json";
  bool exists = true;
  auto settings = mag::load_player_settings(file, exists);
  assert(!exists && settings.server_address == "play.magtest.online" &&
         settings.render_scale == 100);
  settings.server_address = " Example.COM ";
  settings.render_scale = 200;
  settings.tcp_port = 12345;
  mag::save_player_settings(file, settings);
  auto loaded = mag::load_player_settings(file, exists);
  assert(exists && loaded.server_address == "example.com" &&
         loaded.render_scale == 200 && loaded.tcp_port == 12345);
  std::ifstream read(file);
  std::string text((std::istreambuf_iterator<char>(read)), {});
  auto field = text.find("  \"render_scale\": 200,\n");
  assert(field != std::string::npos);
  text.erase(field, std::string("  \"render_scale\": 200,\n").size());
  {
    std::ofstream out(file);
    out << text;
  }
  loaded = mag::load_player_settings(file, exists);
  assert(loaded.render_scale == 100 && loaded.tcp_port == 12345);
  {
    std::ifstream in(file);
    std::string unchanged((std::istreambuf_iterator<char>(in)), {});
    assert(unchanged == text);
  }
  for (const std::string bad :
       {"127.2.3.999", "http://example.com", "bad..example", "-bad.example",
        "1.2.3", "a\\\"b"}) {
    settings.server_address = bad;
    bool rejected = false;
    try {
      mag::save_player_settings(file, settings);
    } catch (...) {
      rejected = true;
    }
    assert(rejected);
  }
  settings.server_address = "127.0.0.1";
  for (int scale : {0, 99, 101, 400}) {
    settings.render_scale = scale;
    bool rejected = false;
    try {
      mag::save_player_settings(file, settings);
    } catch (...) {
      rejected = true;
    }
    assert(rejected);
  }
  for (std::string bad : std::vector<std::string>{
           "{}", "{\"schema_version\":true}", text + "garbage",
           text.substr(0, text.size() - 2), std::string(65537, ' ')}) {
    {
      std::ofstream out(file);
      out << bad;
    }
    bool rejected = false;
    try {
      mag::load_player_settings(file, exists);
    } catch (...) {
      rejected = true;
    }
    assert(rejected);
  }
  settings = mag::PlayerSettings{};
  mag::save_player_settings(file, settings);
#ifndef _WIN32
  auto link = root / "link.json";
  std::filesystem::create_symlink(file, link);
  bool rejected = false;
  try {
    mag::load_player_settings(link, exists);
  } catch (...) {
    rejected = true;
  }
  assert(rejected);
  rejected = false;
  try {
    mag::save_player_settings(link, settings);
  } catch (...) {
    rejected = true;
  }
  assert(rejected);
#endif
  std::cout << "portable settings persistence checks passed\n";
}
