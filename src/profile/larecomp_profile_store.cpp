// larecomp - Save profiles on disk
#include "larecomp_profile_store.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace larecomp::profiles {

namespace {

// UserProfile::kMaxNameLength in the SDK fork.
constexpr size_t kMaxNameLength = 15;

constexpr char kMarkerName[] = ".profile";
constexpr char kSavedGameType[] = "00000001";

bool IsHexName(std::string_view s, size_t digits) {
  return s.size() == digits &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string Lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// True when `dir` holds at least one saved-game package
// (<dir>/<title id>/00000001/<package>/<file>), and records the newest file time.
bool ScanSaves(const fs::path& dir, Profile& profile) {
  bool found = false;
  std::error_code ec;
  for (fs::directory_iterator title(dir, ec), end; !ec && title != end; title.increment(ec)) {
    std::error_code e;
    if (!title->is_directory(e) || !IsHexName(Utf8FromPath(title->path().filename()), 8)) {
      continue;
    }
    for (fs::directory_iterator package(title->path() / kSavedGameType, e), package_end;
         !e && package != package_end; package.increment(e)) {
      std::error_code e2;
      if (!package->is_directory(e2)) {
        continue;
      }
      for (fs::directory_iterator file(package->path(), e2), file_end; !e2 && file != file_end;
           file.increment(e2)) {
        std::error_code e3;
        if (!file->is_regular_file(e3)) {
          continue;
        }
        const auto written = file->last_write_time(e3);
        if (e3) {
          continue;
        }
        found = true;
        profile.last_played = std::max(profile.last_played, written);
      }
    }
  }
  return found;
}

// The key of a flat `key = value` line, or empty for anything else.
std::string LineKey(std::string_view line) {
  if (line.starts_with("\xEF\xBB\xBF")) {
    line.remove_prefix(3);
  }
  const size_t p = line.find_first_not_of(" \t");
  if (p == std::string_view::npos || line[p] == '#' || line[p] == '[') {
    return {};
  }
  size_t e = p;
  while (e < line.size() && line[e] != '=' && line[e] != ' ' && line[e] != '\t') {
    ++e;
  }
  const size_t q = line.find_first_not_of(" \t", e);
  if (e == p || q == std::string_view::npos || line[q] != '=') {
    return {};
  }
  return std::string(line.substr(p, e - p));
}

bool ReadWholeFile(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

}  // namespace

fs::path PathFromUtf8(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string Utf8FromPath(const fs::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string SanitizeName(std::string_view raw) {
  std::string name;
  name.reserve(kMaxNameLength);
  for (char c : raw) {
    if (name.size() >= kMaxNameLength) {
      break;
    }
    const auto uc = static_cast<unsigned char>(c);
    // Control characters, and whatever cannot live in a path component on any host.
    if (uc < 0x20 || uc == 0x7F || std::strchr("<>:\"/\\|?*", c) != nullptr) {
      continue;
    }
    name.push_back(c);
  }
  // Trailing dots and spaces are not addressable on Windows.
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
    name.pop_back();
  }
  while (!name.empty() && name.front() == ' ') {
    name.erase(name.begin());
  }
  static const char* kDeviceNames[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3",
                                       "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1",
                                       "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8",
                                       "LPT9"};
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  for (const char* device : kDeviceNames) {
    if (upper == device) {
      name.push_back('_');
      break;
    }
  }
  return name;
}

bool IsReservedName(std::string_view name) {
  if (name.empty() || name.front() == '.') {
    return true;
  }
  const std::string lower = Lower(name);
  if (lower == "cache" || lower == "achievements" || lower == "ratemyride") {
    return true;
  }
  // 16 digits: the hex XUID directories, shared content included. 8 digits: a
  // title id, which the pre-profile layout filed straight under the root.
  return IsHexName(name, 16) || IsHexName(name, 8);
}

bool SameName(std::string_view a, std::string_view b) {
  return Lower(a) == Lower(b);
}

std::vector<Profile> Enumerate(const fs::path& root, Sanitizer sanitize) {
  std::vector<Profile> found;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (!it->is_directory(e)) {
      continue;
    }
    std::string name = Utf8FromPath(it->path().filename());
    if (IsReservedName(name) || sanitize(name) != name) {
      continue;
    }
    Profile profile;
    profile.name = std::move(name);
    if (!ScanSaves(it->path(), profile)) {
      continue;
    }
    profile.has_save = true;
    found.push_back(std::move(profile));
  }
  std::sort(found.begin(), found.end(), [](const Profile& a, const Profile& b) {
    return a.last_played != b.last_played ? a.last_played > b.last_played : a.name < b.name;
  });
  return found;
}

std::string ReadMarker(const fs::path& root) {
  std::ifstream in(root / kMarkerName, std::ios::binary);
  if (!in) {
    return {};
  }
  // Same reading as the content manager: the first 255 bytes, minus a trailing newline.
  char buffer[256] = {};
  in.read(buffer, sizeof(buffer) - 1);
  std::string name(buffer, static_cast<size_t>(in.gcount()));
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
    name.pop_back();
  }
  return name;
}

bool WriteMarker(const fs::path& root, std::string_view name) {
  std::error_code ec;
  fs::create_directories(root, ec);
  std::ofstream out(root / kMarkerName, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(name.data(), static_cast<std::streamsize>(name.size()));
  return static_cast<bool>(out);
}

std::string QuoteTomlString(std::string_view value) {
  std::string result = "\"";
  for (char c : value) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\t': result += "\\t"; break;
      case '\n': result += "\\n"; break;
      case '\f': result += "\\f"; break;
      case '\r': result += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          static constexpr char kHex[] = "0123456789ABCDEF";
          const auto byte = static_cast<unsigned char>(c);
          result += "\\u00";
          result += kHex[(byte >> 4) & 0xF];
          result += kHex[byte & 0xF];
        } else {
          result += c;
        }
        break;
    }
  }
  result += '"';
  return result;
}

bool EditConfig(const fs::path& config, const std::vector<ConfigEdit>& edits) {
  std::string text;
  const bool existed = ReadWholeFile(config, text);
  // Keep the file's own line ending; a new file gets the one the SDK writes on Windows.
  const std::string eol = (!existed || text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";

  std::vector<bool> applied(edits.size(), false);
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    const bool has_end = nl != std::string::npos;
    std::string_view line(text.data() + pos, (has_end ? nl : text.size()) - pos);
    std::string_view end = has_end ? std::string_view("\n") : std::string_view();
    if (has_end && line.ends_with('\r')) {
      line.remove_suffix(1);
      end = "\r\n";
    }
    pos = has_end ? nl + 1 : text.size();

    const std::string key = LineKey(line);
    size_t hit = edits.size();
    if (!key.empty()) {
      for (size_t i = 0; i < edits.size(); ++i) {
        if (edits[i].key == key) {
          hit = i;
          break;
        }
      }
    }
    if (hit == edits.size()) {
      out.append(line).append(end);  // not ours: byte for byte
      continue;
    }
    if (applied[hit]) {
      continue;  // a second line for a key already handled: TOML forbids the duplicate
    }
    applied[hit] = true;
    if (edits[hit].value) {
      out.append(key).append(" = ").append(*edits[hit].value).append(end);
    }
  }
  for (size_t i = 0; i < edits.size(); ++i) {
    if (applied[i] || !edits[i].value) {
      continue;
    }
    if (out.empty() && !existed) {
      out.append("# Auto-generated cvar configuration").append(eol);
    }
    if (!out.empty() && out.back() != '\n') {
      out.append(eol);
    }
    out.append(edits[i].key).append(" = ").append(*edits[i].value).append(eol);
  }

  if (out == text) {
    return true;  // nothing to change; a missing file stays missing
  }

  std::error_code ec;
  if (!config.parent_path().empty()) {
    fs::create_directories(config.parent_path(), ec);
  }
  // Written beside the file and moved over it, so a crash mid-write cannot leave half a config.
  fs::path temp = config;
  temp += ".tmp";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    if (!o) {
      return false;
    }
    o.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!o) {
      o.close();
      fs::remove(temp, ec);
      return false;
    }
  }
  fs::rename(temp, config, ec);
  if (!ec) {
    return true;
  }
  // Something holds the config open (an editor without share-delete): write through it instead.
  fs::remove(temp, ec);
  std::ofstream direct(config, std::ios::binary | std::ios::trunc);
  if (!direct) {
    return false;
  }
  direct.write(out.data(), static_cast<std::streamsize>(out.size()));
  return static_cast<bool>(direct);
}

}  // namespace larecomp::profiles
