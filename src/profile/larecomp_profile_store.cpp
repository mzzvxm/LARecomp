// larecomp - Save profiles on disk
#include "larecomp_profile_store.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

constexpr std::string_view kBom = "\xEF\xBB\xBF";

// One line of the file and the line break that ended it ("" on the last line).
struct Line {
  std::string_view text;
  std::string_view end;
};

std::vector<Line> SplitLines(std::string_view text) {
  std::vector<Line> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) {
      lines.push_back({text.substr(pos), {}});
      break;
    }
    const size_t cr = (nl > pos && text[nl - 1] == '\r') ? 1 : 0;
    lines.push_back({text.substr(pos, nl - cr - pos), text.substr(nl - cr, cr + 1)});
    pos = nl + 1;
  }
  return lines;
}

bool IsBlank(std::string_view line) {
  return line.find_first_not_of(" \t") == std::string_view::npos;
}

// A [table] or [[array of tables]] header.
bool IsTableLine(std::string_view line) {
  if (line.starts_with(kBom)) {
    line.remove_prefix(kBom.size());
  }
  const size_t p = line.find_first_not_of(" \t");
  return p != std::string_view::npos && line[p] == '[';
}

// A `key = value` line: the key (a quoted one without its quotes) and where the
// value starts in the line. The key is empty for anything else.
struct KeyLine {
  std::string key;
  size_t value = 0;
};

KeyLine ParseKeyLine(std::string_view line) {
  const size_t bom = line.starts_with(kBom) ? kBom.size() : 0;
  const size_t p = line.find_first_not_of(" \t", bom);
  if (p == std::string_view::npos || line[p] == '#' || line[p] == '[') {
    return {};
  }
  std::string key;
  size_t e = p;
  if (line[p] == '"' || line[p] == '\'') {
    const size_t close = line.find(line[p], p + 1);
    if (close == std::string_view::npos) {
      return {};
    }
    key = std::string(line.substr(p + 1, close - p - 1));
    e = close + 1;
  } else {
    while (e < line.size() && line[e] != '=' && line[e] != ' ' && line[e] != '\t') {
      ++e;
    }
    key = std::string(line.substr(p, e - p));
  }
  const size_t q = line.find_first_not_of(" \t", e);
  if (key.empty() || q == std::string_view::npos || line[q] != '=') {
    return {};
  }
  return {std::move(key), std::min(line.find_first_not_of(" \t", q + 1), line.size())};
}

// Where the single-line value that starts at `pos` ends: after its closing
// quote for a string, else at the first space or '#'.
size_t ValueEnd(std::string_view line, size_t pos) {
  if (pos >= line.size()) {
    return line.size();
  }
  const char c = line[pos];
  size_t i = pos;
  if (c == '"' || c == '\'') {
    for (++i; i < line.size() && line[i] != c; i += (c == '"' && line[i] == '\\') ? 2 : 1) {
    }
    return std::min(i + 1, line.size());
  }
  while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '#') {
    ++i;
  }
  return i;
}

// How many lines the statement on lines[i] takes: one, unless its value is a
// multi-line string (""" or ''') that does not close on that line. The lines of
// such a string are never taken for keys.
size_t StatementLines(const std::vector<Line>& lines, size_t i, size_t value) {
  const std::string_view first = lines[i].text;
  const std::string_view open = first.substr(std::min(value, first.size()), 3);
  if (open != "\"\"\"" && open != "'''") {
    return 1;
  }
  if (first.find(open, value + 3) != std::string_view::npos) {
    return 1;
  }
  for (size_t j = i + 1; j < lines.size(); ++j) {
    if (lines[j].text.find(open) != std::string_view::npos) {
      return j - i + 1;
    }
  }
  return lines.size() - i;
}

// Moves `from` over `to` in one step.
bool MoveOver(const fs::path& from, const fs::path& to) {
#if defined(_WIN32)
  // The data must be on the disk before the name moves, or a power cut could
  // leave the new name on an empty file.
  HANDLE handle = CreateFileW(from.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(handle);
    CloseHandle(handle);
  }
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         0;
#else
  std::error_code ec;
  fs::rename(from, to, ec);
  return !ec;
#endif
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
  // The settings panel and the pause menu save from different threads; both
  // read the file, change it and write it back, and share one temporary name.
  static std::mutex edit_mutex;
  std::lock_guard<std::mutex> lock(edit_mutex);

  std::string text;
  const bool existed = ReadWholeFile(config, text);
  std::error_code ec;
  if (!existed && fs::exists(config, ec)) {
    return false;  // there, but it cannot be read: do not replace what we have not seen
  }
  // Keep the file's own line ending; a new file gets the one the SDK writes on Windows.
  const std::string eol = (!existed || text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";

  const std::vector<Line> lines = SplitLines(text);
  std::vector<bool> applied(edits.size(), false);
  std::string out;
  // Cvars are top-level keys. Past the first [table] nothing is read or added,
  // and a new key goes in right after the last line above that table.
  bool in_table = false;
  size_t top_end = 0;
  size_t insert_at = std::string::npos;
  for (size_t i = 0; i < lines.size();) {
    if (!in_table && IsTableLine(lines[i].text)) {
      in_table = true;
      insert_at = top_end;
    }
    KeyLine parsed;
    if (!in_table) {
      parsed = ParseKeyLine(lines[i].text);
    }
    size_t hit = edits.size();
    for (size_t k = 0; k < edits.size() && !parsed.key.empty(); ++k) {
      if (edits[k].key == parsed.key) {
        hit = k;
        break;
      }
    }
    const size_t count = parsed.key.empty() ? 1 : StatementLines(lines, i, parsed.value);
    if (hit == edits.size()) {
      for (size_t j = i; j < i + count; ++j) {  // not ours: byte for byte
        out.append(lines[j].text).append(lines[j].end);
        if (!in_table && !IsBlank(lines[j].text)) {
          top_end = out.size();
        }
      }
      i += count;
      continue;
    }
    if (!applied[hit]) {
      applied[hit] = true;
      if (edits[hit].value) {
        const std::string_view first = lines[i].text;
        // What follows the old value stays when it is a comment.
        std::string_view comment;
        if (count == 1) {
          const size_t end = ValueEnd(first, parsed.value);
          const size_t c = first.find_first_not_of(" \t", end);
          if (c != std::string_view::npos && first[c] == '#') {
            comment = first.substr(end);
          }
        }
        // The key part is kept as the player typed it (spacing, quotes, BOM).
        out.append(first.substr(0, parsed.value))
            .append(*edits[hit].value)
            .append(comment)
            .append(lines[i + count - 1].end);
        top_end = out.size();
      }
    }
    // else: a second line for a key already handled; TOML forbids the duplicate.
    i += count;
  }
  std::string added;
  for (size_t i = 0; i < edits.size(); ++i) {
    if (!applied[i] && edits[i].value && !edits[i].update_only) {
      added.append(edits[i].key).append(" = ").append(*edits[i].value).append(eol);
    }
  }
  if (!added.empty()) {
    if (insert_at != std::string::npos) {
      out.insert(insert_at, added);
    } else {
      if (out.empty() && !existed) {
        out.append("# Auto-generated cvar configuration").append(eol);
      }
      if (!out.empty() && out.back() != '\n') {
        out.append(eol);
      }
      out.append(added);
    }
  }

  if (out == text) {
    return true;  // nothing to change; a missing file stays missing
  }

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
  if (MoveOver(temp, config)) {
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
