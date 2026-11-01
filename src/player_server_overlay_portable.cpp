#include "player_archive.h"
#include "player_settings_portable.h"
#include "sdata_decrypt.h"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mag {
namespace {
namespace fs = std::filesystem;
constexpr size_t archiveLimit = 512ull * 1024 * 1024;
const fs::path archivePath = "PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat";
const std::string member = "/run/mag/ps3/settings/maglocalsettings.lua";
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string sha(const std::vector<uint8_t> &bytes) {
  std::array<unsigned char, 32> digest{};
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                      nullptr, 0) >= 0,
          "SHA-256 is unavailable.");
  const bool ok =
      BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
      BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                     (ULONG)bytes.size(), 0) >= 0 &&
      BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) >= 0;
  if (hash)
    BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  require(ok, "SHA-256 failed.");
#elif defined(__APPLE__)
  CC_SHA256(bytes.data(), (CC_LONG)bytes.size(), digest.data());
#else
  unsigned int length = 0;
  require(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length,
                     EVP_sha256(), nullptr) == 1 &&
              length == 32,
          "SHA-256 failed.");
#endif
  const char hex[] = "0123456789abcdef";
  std::string result;
  for (auto value : digest) {
    result += hex[value >> 4];
    result += hex[value & 15];
  }
  return result;
}
std::vector<uint8_t> read(const fs::path &path, size_t limit) {
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 65536> buffer{};
#ifdef _WIN32
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  require(file != INVALID_HANDLE_VALUE,
          "The private game settings file could not be opened.");
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      info.nFileSizeHigh || info.nFileSizeLow > limit) {
    CloseHandle(file);
    throw std::runtime_error(
        "The private game settings file is invalid or too large.");
  }
  DWORD count = 0;
  BOOL ok;
  while ((ok = ReadFile(file, buffer.data(), (DWORD)buffer.size(), &count,
                        nullptr)) &&
         count) {
    if (count > limit - bytes.size()) {
      CloseHandle(file);
      throw std::runtime_error("The private game settings file is too large.");
    }
    bytes.insert(bytes.end(), buffer.data(), buffer.data() + count);
  }
  CloseHandle(file);
  require(ok, "The private game settings file could not be read.");
#else
  int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  require(file >= 0, "The private game settings file could not be opened.");
  struct stat info{};
  if (fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      (uint64_t)info.st_size > limit) {
    close(file);
    throw std::runtime_error(
        "The private game settings file is invalid or too large.");
  }
  ssize_t count;
  while ((count = ::read(file, buffer.data(), buffer.size())) != 0) {
    if (count < 0) {
      if (errno == EINTR)
        continue;
      close(file);
      throw std::runtime_error(
          "The private game settings file could not be read.");
    }
    if ((size_t)count > limit - bytes.size()) {
      close(file);
      throw std::runtime_error("The private game settings file is too large.");
    }
    bytes.insert(bytes.end(), buffer.data(), buffer.data() + count);
  }
  close(file);
#endif
  return bytes;
}
void write(const fs::path &path, const std::vector<uint8_t> &bytes) {
#ifdef _WIN32
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  require(file != INVALID_HANDLE_VALUE,
          "The private game settings file could not be created.");
  DWORD wrote = 0;
  bool ok =
      WriteFile(file, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) &&
      wrote == bytes.size() && FlushFileBuffers(file);
  if (!CloseHandle(file))
    ok = false;
  require(ok, "The private game settings file could not be written.");
#else
  int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  require(file >= 0, "The private game settings file could not be created.");
  size_t left = bytes.size();
  const uint8_t *next = bytes.data();
  bool ok = true;
  while (left) {
    ssize_t n = ::write(file, next, left);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    next += n;
    left -= (size_t)n;
  }
  if (ok && fsync(file))
    ok = false;
  if (close(file))
    ok = false;
  require(ok, "The private game settings file could not be written.");
#endif
}
std::string ipv4(const std::string &host) {
#ifdef _WIN32
  WSADATA data{};
  require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
          "The network address resolver is unavailable.");
  struct Winsock {
    ~Winsock() { WSACleanup(); }
  } winsock;
#endif
  addrinfo hints{}, *result = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  require(getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result,
          "The server hostname could not be resolved to IPv4.");
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(result,
                                                           freeaddrinfo);
  char text[INET_ADDRSTRLEN];
  const auto *address = &((sockaddr_in *)result->ai_addr)->sin_addr;
  require(inet_ntop(AF_INET, address, text, sizeof text) != nullptr,
          "The server address could not be converted to IPv4.");
  return text;
}
void privateDirectory(const fs::path &path) {
  require(fs::create_directory(path),
          "The private server overlay could not be staged.");
#ifndef _WIN32
  require(chmod(path.c_str(), 0700) == 0,
          "The private server overlay permissions could not be set.");
#endif
}
#ifdef _WIN32
// Windows normally denies unprivileged symbolic links. A directory tree of hard
// links needs no Developer Mode and keeps source bytes untouched. Cross-volume
// installations fall back to ordinary private copies, including their cleanup.
void linkView(const fs::path &source, const fs::path &target) {
  const auto status = fs::symlink_status(source);
  const auto attributes = GetFileAttributesW(source.c_str());
  require(attributes != INVALID_FILE_ATTRIBUTES &&
              !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
              !fs::is_symlink(status),
          "The original game view contains an unsupported symbolic link or "
          "reparse point.");
  if (fs::is_directory(status)) {
    privateDirectory(target);
    for (const auto &entry : fs::directory_iterator(source))
      linkView(entry.path(), target / entry.path().filename());
  } else if (fs::is_regular_file(status)) {
    std::error_code error;
    fs::create_hard_link(source, target, error);
    if (error)
      require(fs::copy_file(source, target, fs::copy_options::none),
              "The private game view could not be copied.");
  } else
    throw std::runtime_error(
        "The original game view contains an unsupported file type.");
}
#else
void linkView(const fs::path &source, const fs::path &target) {
  fs::create_symlink(source, target);
}
#endif
std::vector<uint8_t> asBytes(const std::string &text) {
  return {text.begin(), text.end()};
}
std::string identity(const fs::path &source, const PlayerSettings &s,
                     const std::string &expected) {
  auto utf8 = source.generic_u8string();
  std::string path(utf8.begin(), utf8.end());
  return std::to_string(path.size()) + ":" + path + "\n" + s.server_address +
         "\n" + std::to_string(s.tcp_port) + "\n" + std::to_string(s.udp_port) +
         "\n" + expected + "\ncommunity-no-ticket-v1\n";
}
} // namespace

fs::path create_server_overlay(const fs::path &sourceRoot,
                               const fs::path &ownedRoot,
                               const PlayerSettings &rawSettings,
                               const std::string &expected) {
  const auto settings = validate_player_settings(rawSettings);
  if (settings.server_address.empty())
    return sourceRoot;
  require(!sourceRoot.empty() && !ownedRoot.empty(),
          "The private server overlay location is unavailable.");
  require(expected.size() == 64 && expected.find_first_not_of(
                                       "0123456789abcdef") == std::string::npos,
          "The supported settings member identity is invalid.");
  auto source = fs::canonical(sourceRoot);
  fs::create_directories(ownedRoot);
  require(fs::is_directory(fs::symlink_status(ownedRoot)),
          "The private server overlay folder must be an ordinary directory.");
  auto owned = fs::canonical(ownedRoot);
  auto relative = owned.lexically_relative(source);
  require(relative.empty() || *relative.begin() == "..",
          "The private overlay must be outside the original game folder.");
#ifndef _WIN32
  require(chmod(owned.c_str(), 0700) == 0,
          "The private server overlay permissions could not be set.");
#endif
  const auto destination = owned / "vfs",
             markerPath = owned / "server-overlay.identity";
  const auto prefix = std::string("MAG server overlay v1\n") +
                      sha(asBytes(identity(source, settings, expected))) + "\n";
  if (fs::exists(fs::symlink_status(destination))) {
    require(fs::is_directory(fs::symlink_status(destination)),
            "The existing private overlay is not an ordinary directory.");
    const auto marker = read(markerPath, 4096);
    const std::string text(marker.begin(), marker.end());
    require(
        text.size() == prefix.size() + 65 &&
            text.compare(0, prefix.size(), prefix) == 0 && text.back() == '\n',
        "The existing private server overlay belongs to different settings.");
    require(text.substr(prefix.size(), 64) ==
                sha(read(destination / archivePath, archiveLimit)),
            "The private server overlay changed unexpectedly. Relaunch MAG.");
    return destination;
  }
  auto archive = read(source / archivePath, archiveLimit);
  if (archive.size() >= 4 && !std::memcmp(archive.data(), "NPD\0", 4)) {
    size_t length = 0;
    std::unique_ptr<uint8_t, decltype(&free)> plain(
        sdata_decrypt(archive.data(), archive.size(), &length), free);
    require(plain && length <= archiveLimit,
            "The supported SDATA settings archive could not be decoded.");
    archive.assign(plain.get(), plain.get() + length);
  }
  auto original = read_psarc_member(archive, member);
  require(sha(original) == expected,
          "The settings archive does not match the supported retail version. "
          "The original was kept.");
  const std::string extra =
      "\n-- MAG player connection override\nMAGClientNetConfig = "
      "MAGClientNetConfig or {}\n"
      "MAGClientNetConfig.EnableNetwork = "
      "true\nMAGClientNetConfig.MAPSServerAddr = \"" +
      ipv4(settings.server_address) +
      "\"\n"
      "MAGClientNetConfig.EnableServerOnClient = "
      "false\nMAGClientNetConfig.UseTestClient = "
      "false\nMAGClientNetConfig.UseNPTicket = false\n" +
      (settings.tcp_port ? "MAGClientNetConfig.MAPSServerTCPPort = " +
                               std::to_string(settings.tcp_port) + "\n"
                         : "") +
      (settings.udp_port ? "MAGClientNetConfig.MAPSServerUDPPort = " +
                               std::to_string(settings.udp_port) + "\n"
                         : "");
  original.insert(original.end(), extra.begin(), extra.end());
  auto changed = rewrite_psarc_member(archive, member, original);
  fs::path stage;
  for (int attempt = 0; attempt < 16; ++attempt) {
    auto candidate = owned / (".vfs-" + std::to_string(std::random_device{}()));
    if (!fs::exists(fs::symlink_status(candidate))) {
      privateDirectory(candidate);
      stage = candidate;
      break;
    }
  }
  require(!stage.empty(), "The private server overlay could not be staged.");
  try {
    auto sourceDir = source, targetDir = stage;
    for (auto part = archivePath.begin(); part != archivePath.end(); ++part) {
      for (const auto &child : fs::directory_iterator(sourceDir))
        if (child.path().filename() != *part)
          linkView(child.path(), targetDir / child.path().filename());
      if (std::next(part) != archivePath.end()) {
        sourceDir /= *part;
        targetDir /= *part;
        privateDirectory(targetDir);
      }
    }
    write(stage / archivePath, changed.bytes);
    // A separate owned directory avoids colliding with any original VFS
    // filename, and atomic replacement never follows an existing marker link.
    auto markerStage = stage;
    markerStage += ".marker";
    privateDirectory(markerStage);
    try {
      write(markerStage / "identity",
            asBytes(prefix + sha(changed.bytes) + "\n"));
#ifdef _WIN32
      require(
          MoveFileExW((markerStage / "identity").c_str(), markerPath.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
          "The private server overlay marker could not be saved.");
#else
      fs::rename(markerStage / "identity", markerPath);
#endif
      fs::remove(markerStage);
    } catch (...) {
      std::error_code ignored;
      fs::remove_all(markerStage, ignored);
      throw;
    }
    fs::rename(stage, destination);
    return destination;
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(stage, ignored);
    throw;
  }
}
} // namespace mag
