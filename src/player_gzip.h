#pragma once
// A small gzip (RFC 1952 around RFC 1951) compressor for the performance and error reports
// (src/player_reports.h). The packaged supervisors build without zlib, so this is self-contained:
// LZ77 over a 32 KiB window with fixed Huffman codes. Log text compresses about four to six times;
// the output is a standard gzip member any decoder reads.
#include <cstddef>
#include <cstdint>
#include <string>

namespace mag {
// The CRC-32 (IEEE 802.3, reflected) gzip carries.
uint32_t crc32(const void *data, size_t size, uint32_t crc = 0);
// One gzip member holding `size` bytes (empty input gives a valid, empty member).
std::string gzip_compress(const void *data, size_t size);
inline std::string gzip_compress(const std::string &data) { return gzip_compress(data.data(), data.size()); }
// Standard base64 with padding (RFC 4648), for the report's encoded log blobs.
std::string base64_encode(const std::string &data);
}  // namespace mag
