#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mag {
// Lowercase hexadecimal SHA-256 (FIPS 180-4), for builds without a system crypto library.
std::string sha256_hex(const uint8_t *data, size_t size);
}
