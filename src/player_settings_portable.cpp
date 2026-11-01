#include "player_settings_portable.h"
#include "retail_network_policy.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string_view>
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
namespace {
[[noreturn]] void invalid(const char *message) {
  throw std::runtime_error(message);
}
struct Value {
  bool string;
  std::string text;
};
// Settings are a flat object. Reject duplicate/unknown keys, non-integral
// numbers, nested values and invalid escapes rather than quietly coercing
// preferences.
struct Parser {
  std::string_view text;
  size_t at = 0;
  void space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' ||
                                text[at] == '\n' || text[at] == '\r'))
      ++at;
  }
  bool take(char c) {
    space();
    if (at < text.size() && text[at] == c) {
      ++at;
      return true;
    }
    return false;
  }
  void need(char c) {
    if (!take(c))
      invalid("The settings file is damaged. Review defaults and Apply to "
              "replace it.");
  }
  std::string string() {
    need('"');
    std::string out;
    while (at < text.size()) {
      unsigned char c = text[at++];
      if (c == '"')
        return out;
      if (c < 32)
        invalid("Invalid settings string.");
      if (c == '\\') {
        if (at == text.size())
          invalid("Invalid settings string.");
        c = text[at++];
        if (c == 'u') {
          unsigned value = 0;
          for (int i = 0; i < 4; ++i) {
            if (at == text.size())
              invalid("Invalid settings escape.");
            char h = text[at++];
            unsigned n = h >= '0' && h <= '9'   ? h - '0'
                         : h >= 'a' && h <= 'f' ? h - 'a' + 10
                         : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                : 16;
            if (n == 16)
              invalid("Invalid settings escape.");
            value = value * 16 + n;
          }
          // All supported preference values are ASCII.
          if (value > 127 || value < 32)
            invalid("Settings values must use ASCII characters.");
          c = (unsigned char)value;
        } else
          switch (c) {
          case '"':
          case '\\':
          case '/':
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'n':
            c = '\n';
            break;
          case 'r':
            c = '\r';
            break;
          case 't':
            c = '\t';
            break;
          default:
            invalid("Invalid settings escape.");
          }
      }
      out += (char)c;
    }
    invalid("Unterminated settings string.");
  }
  std::map<std::string, Value> object() {
    need('{');
    std::map<std::string, Value> values;
    if (!take('}'))
      for (;;) {
        auto key = string();
        need(':');
        space();
        Value value{};
        if (at < text.size() && text[at] == '"') {
          value.string = true;
          value.text = string();
        } else {
          size_t start = at;
          if (at < text.size() && text[at] == '-')
            ++at;
          if (at == text.size() || text[at] < '0' || text[at] > '9')
            invalid("Settings numbers must be integers.");
          if (text[at] == '0')
            ++at;
          else
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
              ++at;
          value.text = std::string(text.substr(start, at - start));
        }
        if (!values.emplace(key, value).second)
          invalid("Settings contain duplicate fields.");
        if (take('}'))
          break;
        need(',');
      }
    space();
    if (at != text.size())
      invalid("Unexpected data follows the settings object.");
    return values;
  }
};
std::string quote(const std::string &text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out + '"';
}
std::string encode(const PlayerSettings &s) {
  return "{\n  \"schema_version\": 1,\n  \"server_address\": " +
         quote(s.server_address) +
         ",\n  \"local_account_name\": " + quote(s.local_account_name) +
         ",\n  \"tcp_port\": " + std::to_string(s.tcp_port) +
         ",\n  \"udp_port\": " + std::to_string(s.udp_port) +
         ",\n  \"display_mode\": " + quote(s.display_mode) +
         ",\n  \"output_width\": " + std::to_string(s.output_width) +
         ",\n  \"output_height\": " + std::to_string(s.output_height) +
         ",\n  \"render_scale\": " + std::to_string(s.render_scale) +
         ",\n  \"anti_aliasing\": " + quote(s.anti_aliasing) + "\n}\n";
}
PlayerSettings decode(const std::string &text) {
  auto values = Parser{text}.object();
  PlayerSettings s;
  if (!values.count("render_scale"))
    values["render_scale"] = {false, "100"};
  if (!values.count("local_account_name"))
    values["local_account_name"] = {true, ""};
  if (values.size() != 10)
    invalid("Settings contain missing or unsupported fields.");
  auto str = [&](const char *key) {
    auto it = values.find(key);
    if (it == values.end() || !it->second.string)
      invalid("Missing or invalid settings string.");
    return it->second.text;
  };
  auto number = [&](const char *key) {
    auto it = values.find(key);
    if (it == values.end() || it->second.string)
      invalid("Missing or invalid settings integer.");
    int n = 0;
    const auto &v = it->second.text;
    auto result = std::from_chars(v.data(), v.data() + v.size(), n);
    if (result.ec != std::errc() || result.ptr != v.data() + v.size())
      invalid("Invalid settings integer.");
    return n;
  };
  s.schema_version = number("schema_version");
  s.server_address = str("server_address");
  s.local_account_name = str("local_account_name");
  s.tcp_port = number("tcp_port");
  s.udp_port = number("udp_port");
  s.display_mode = str("display_mode");
  s.output_width = number("output_width");
  s.output_height = number("output_height");
  s.render_scale = number("render_scale");
  s.anti_aliasing = str("anti_aliasing");
  return validate_player_settings(s);
}
} // namespace

PlayerSettings validate_player_settings(PlayerSettings s) {
  if (s.schema_version != 1)
    invalid("This settings version is not supported.");
  if (!s.local_account_name.empty() &&
      (s.local_account_name.find('\0') != std::string::npos ||
       !mag_local_account_name_valid(s.local_account_name.c_str())))
    invalid("Use 3 to 15 letters, digits, hyphens or underscores for the community account name.");
  auto whitespace = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
  };
  auto begin = std::find_if_not(s.server_address.begin(),
                                s.server_address.end(), whitespace);
  auto end = std::find_if_not(s.server_address.rbegin(),
                              s.server_address.rend(), whitespace)
                 .base();
  s.server_address = begin < end ? std::string(begin, end) : std::string();
  for (char &c : s.server_address)
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
  const auto &host = s.server_address;
  if (host.size() > 253)
    invalid("The server address is too long.");
  if (!host.empty()) {
    bool numeric = true;
    size_t start = 0;
    unsigned labels = 0;
    for (char c : host) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '.'))
        invalid("Use a hostname or IPv4 address, without a URL, path or port.");
      if (c != '.' && (c < '0' || c > '9'))
        numeric = false;
    }
    for (;;) {
      size_t endLabel = host.find('.', start);
      if (endLabel == std::string::npos)
        endLabel = host.size();
      size_t length = endLabel - start;
      if (!length || length > 63 || host[start] == '-' ||
          host[endLabel - 1] == '-')
        invalid("Enter a valid server hostname.");
      if (numeric) {
        unsigned n = 0;
        auto parsed =
            std::from_chars(host.data() + start, host.data() + endLabel, n);
        if (length > 3 || parsed.ec != std::errc() || n > 255 ||
            (length > 1 && host[start] == '0'))
          invalid("Enter a valid IPv4 address.");
      }
      ++labels;
      if (endLabel == host.size())
        break;
      start = endLabel + 1;
    }
    if (numeric && labels != 4)
      invalid("Enter a valid IPv4 address.");
  }
  if (s.tcp_port < 0 || s.tcp_port > 65535 || s.udp_port < 0 ||
      s.udp_port > 65535)
    invalid(
        "Ports must be between 1 and 65535, or blank for the game default.");
  if (s.display_mode != "windowed" && s.display_mode != "borderless")
    invalid("Choose windowed or borderless fullscreen.");
  if (!((s.output_width == 1280 && s.output_height == 720) ||
        (s.output_width == 1920 && s.output_height == 1080) ||
        (s.output_width == 2560 && s.output_height == 1440) ||
        (s.output_width == 3840 && s.output_height == 2160)))
    invalid("Choose a supported output size.");
  if (s.render_scale != 100 && s.render_scale != 150 && s.render_scale != 200 &&
      s.render_scale != 300)
    invalid("Choose a supported rendering resolution.");
  if (s.anti_aliasing != "off" && s.anti_aliasing != "fxaa")
    invalid("Choose Off or FXAA for anti-aliasing.");
  return s;
}

PlayerSettings load_player_settings(const std::filesystem::path &path,
                                    bool &exists) {
  exists = true;
  std::string data;
  char bytes[4096];
#ifdef _WIN32
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND ||
        GetLastError() == ERROR_PATH_NOT_FOUND) {
      exists = false;
      return {};
    }
    invalid("Settings could not be opened. Check file permissions.");
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      info.nFileSizeHigh || info.nFileSizeLow > 65536) {
    CloseHandle(file);
    invalid("The settings file is invalid or too large.");
  }
  DWORD count;
  BOOL ok;
  while ((ok = ReadFile(file, bytes, sizeof bytes, &count, nullptr)) && count) {
    data.append(bytes, count);
    if (data.size() > 65536) {
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
      exists = false;
      return {};
    }
    invalid("Settings could not be opened. Check file permissions.");
  }
  struct stat info{};
  if (fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_size > 65536) {
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
    if (data.size() > 65536) {
      close(file);
      invalid("The settings file is too large.");
    }
  }
  close(file);
#endif
  return decode(data);
}

void save_player_settings(const std::filesystem::path &path,
                          const PlayerSettings &settings) {
  const auto data = encode(validate_player_settings(settings));
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
} // namespace mag
