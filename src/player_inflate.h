#pragma once
#include <cstddef>
#include <cstdint>

namespace mag {
// Decodes one zlib stream (RFC 1950 around RFC 1951 data) into exactly `expected` bytes.
// Returns false for any malformed, truncated or differently sized stream, or a bad checksum.
// Used where zlib itself is not part of the build (the portable player settings).
bool zlib_decode(const uint8_t *source, size_t size, uint8_t *output, size_t expected);
}
