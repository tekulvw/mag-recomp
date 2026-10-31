#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mag {
std::vector<uint8_t> read_psarc_member(const std::vector<uint8_t>& source, const std::string& member);
struct PsarcReplacement { std::vector<uint8_t> bytes, original; };
PsarcReplacement rewrite_psarc_member(const std::vector<uint8_t>& source, const std::string& member,
                                     const std::vector<uint8_t>& replacement);
}
