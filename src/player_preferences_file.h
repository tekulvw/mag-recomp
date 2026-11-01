#pragma once
#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
namespace mag {
// Shared ordinary-file, bounded-read and atomic replacement implementation.
std::optional<std::string> read_player_preferences_file(const std::filesystem::path&, std::size_t limit=65536);
void write_player_preferences_file(const std::filesystem::path&, const std::string&);
}
