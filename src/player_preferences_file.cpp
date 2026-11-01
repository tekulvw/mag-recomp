#include "player_preferences_file.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace mag {
namespace { [[noreturn]] void invalid(const char* message) {throw std::runtime_error(message);} }
std::optional<std::string> read_player_preferences_file(const std::filesystem::path &path, size_t limit) {
  std::string data;
  char bytes[4096];
#ifdef _WIN32
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND ||
        GetLastError() == ERROR_PATH_NOT_FOUND) {
      return std::nullopt;
    }
    invalid("Settings could not be opened. Check file permissions.");
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      info.nFileSizeHigh || info.nFileSizeLow > limit) {
    CloseHandle(file);
    invalid("The settings file is invalid or too large.");
  }
  DWORD count;
  BOOL ok;
  while ((ok = ReadFile(file, bytes, sizeof bytes, &count, nullptr)) && count) {
    data.append(bytes, count);
    if (data.size() > limit) {
      CloseHandle(file);
      invalid("The settings file is too large.");
    }
  }
  CloseHandle(file);
  if (!ok)
    invalid("The settings file could not be read.");
#else
  int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (file < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    invalid("Settings could not be opened. Check file permissions.");
  }
  struct stat info{};
  if (fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || uint64_t(info.st_size) > limit) {
    close(file);
    invalid("The settings file is invalid or too large.");
  }
  ssize_t count;
  while ((count = read(file, bytes, sizeof bytes)) != 0) {
    if (count < 0) {
      if (errno == EINTR)
        continue;
      close(file);
      invalid("The settings file could not be read.");
    }
    data.append(bytes, (size_t)count);
    if (data.size() > limit) {
      close(file);
      invalid("The settings file is too large.");
    }
  }
  close(file);
#endif
  return data;
}

void write_player_preferences_file(const std::filesystem::path &path, const std::string &data) {
  auto parent = path.parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  auto status = std::filesystem::symlink_status(path);
  if (std::filesystem::exists(status) &&
      !std::filesystem::is_regular_file(status))
    invalid("Settings must be an ordinary file.");
#ifdef _WIN32
  std::filesystem::path temporary;
  HANDLE file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 16 && file == INVALID_HANDLE_VALUE;
       ++attempt) {
    temporary = path;
    temporary += L".next." + std::to_wstring(std::random_device{}());
    file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (file == INVALID_HANDLE_VALUE)
    invalid("Cannot create the settings file. Check folder permissions.");
  DWORD wrote = 0;
  bool ok = WriteFile(file, data.data(), (DWORD)data.size(), &wrote, nullptr) &&
            wrote == data.size() && FlushFileBuffers(file);
  if (!CloseHandle(file))
    ok = false;
  if (ok && !MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    ok = false;
  if (!ok) {
    DeleteFileW(temporary.c_str());
    invalid("Settings could not be saved. Your previous settings were kept.");
  }
#else
  auto pattern = path.string() + ".next.XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  int file = mkstemp(name.data());
  if (file < 0)
    invalid("Cannot create the settings file. Check folder permissions.");
  size_t left = data.size();
  const char *next = data.data();
  bool ok = true;
  while (left) {
    ssize_t n = write(file, next, left);
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
  if (ok && rename(name.data(), path.c_str()))
    ok = false;
  if (!ok) {
    unlink(name.data());
    invalid("Settings could not be saved. Your previous settings were kept.");
  }
#endif
}
}
