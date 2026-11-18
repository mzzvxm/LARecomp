#ifndef REXGLUE_HAS_XEO3_TARGET
// See string_dumper.h. Ported from the fork's src/system/string_dumper.cpp with
// two changes: logging goes through the larecomp category, and every TOML file
// is read through the path object instead of path.string() -- on Windows that
// string is in the ANSI code page, so a strings folder under an accented
// directory could not be opened (the bug the fork fixed in cvar.cpp and
// achievement_manager.cpp but never here).

#include "string_dumper.h"

#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "logging.h"

// The fork's SDK defines these four next to its own dumper; the stock SDK has
// none of them. Same names, defaults and help text as the fork, so a toml set up
// for one keeps working on the other.
#if !__has_include(<rex/system/string_dumper.h>)
REXCVAR_DEFINE_BOOL(string_dump_enabled, false, "System/String Replacement",
                    "Capture encountered strings to TOML for translation/modding")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(string_replace_enabled, false, "System/String Replacement",
                    "Inject replacement strings from disk when available")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(string_folder, "", "System/String Replacement",
                      "Override the strings folder (empty = <game data>/strings)")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(string_language, "", "System/String Replacement",
                      "Language code for string replacements (e.g. pt_BR, es_ES)")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

namespace mc::strings {

namespace {

// For log lines: UTF-8, whatever the path holds.
std::string PathUtf8(const std::filesystem::path& p) {
  const std::u8string u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

// The whole file, opened through the path object; false if it cannot be read.
bool ReadWholeFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

bool IsTomlExtension(const std::filesystem::path& p) {
  std::string ext = PathUtf8(p.extension());
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return ext == ".toml";
}

}  // namespace

uint64_t StringDumper::HashString(std::string_view context, std::string_view text) {
  constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
  constexpr uint64_t kFnvPrime = 1099511628211ULL;
  uint64_t hash = kFnvOffset;
  for (char c : context) {
    hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
    hash *= kFnvPrime;
  }
  // The separator, so ("ab", "cd") and ("a", "bcd") do not collide. XOR with
  // zero is a no-op; the multiply is what the fork's hash has, so it stays.
  hash *= kFnvPrime;
  for (char c : text) {
    hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
    hash *= kFnvPrime;
  }
  return hash;
}

StringDumper::StringDumper(std::filesystem::path strings_dir)
    : strings_dir_(std::move(strings_dir)) {
  Rescan();
}

void StringDumper::Rescan() {
  replacements_.clear();
  std::error_code ec;
  if (!std::filesystem::exists(replace_dir(), ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(replace_dir(), ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file() && IsTomlExtension(entry.path())) {
      LoadReplacementFile(entry.path());
    }
  }
  if (!replacements_.empty()) {
    MC_INFO("StringDumper: {} replacement(s) indexed from {}", replacements_.size(),
            PathUtf8(replace_dir()));
  }
}

bool StringDumper::LoadReplacementFile(const std::filesystem::path& path) {
  std::string content;
  if (!ReadWholeFile(path, content)) {
    MC_WARN("StringDumper: cannot read {}", PathUtf8(path));
    return false;
  }
  try {
    auto table = toml::parse(content, PathUtf8(path));
    const auto* entries = table["strings"].as_array();
    if (!entries) {
      return false;
    }
    const std::string active_lang = REXCVAR_GET(string_language);
    size_t loaded = 0;
    for (const auto& node : *entries) {
      const auto* entry = node.as_table();
      if (!entry) {
        continue;
      }
      auto context = (*entry)["context"].value<std::string>();
      auto original = (*entry)["original"].value<std::string>();
      if (!context || !original) {
        continue;
      }
      const uint64_t id = HashString(*context, *original);
      // A plain "text" field replaces in every language.
      if (auto text = (*entry)["text"].value<std::string>()) {
        replacements_[id] = *text;
        ++loaded;
        continue;
      }
      // Otherwise a sub-table per language code, picked by string_language.
      if (!active_lang.empty()) {
        if (const auto* lang_table = (*entry)[active_lang].as_table()) {
          if (auto text = (*lang_table)["text"].value<std::string>()) {
            replacements_[id] = *text;
            ++loaded;
          }
        }
      }
    }
    if (loaded > 0) {
      MC_INFO("StringDumper: loaded {} replacement(s) from {}", loaded,
              PathUtf8(path.filename()));
    }
    return true;
  } catch (const toml::parse_error& error) {
    MC_WARN("StringDumper: failed to parse {}: {}", PathUtf8(path), error.what());
    return false;
  }
}

void StringDumper::DumpString(std::string_view context, std::string_view original,
                              uint32_t guest_address) {
  if (original.empty()) {
    return;
  }
  const uint64_t id = HashString(context, original);
  bool should_flush = false;
  {
    std::lock_guard lock(mutex_);
    if (pending_dumps_.contains(id) || flushed_hashes_.contains(id)) {
      return;
    }
    Entry entry;
    entry.id = id;
    entry.context = std::string(context);
    entry.original = std::string(original);
    entry.guest_address = guest_address;
    pending_dumps_.emplace(id, std::move(entry));
    should_flush = pending_dumps_.size() >= kAutoFlushThreshold;
  }
  if (should_flush) {
    FlushDump();
  }
}

void StringDumper::FlushDump() {
  std::unordered_map<uint64_t, Entry> to_flush;
  {
    std::lock_guard lock(mutex_);
    if (pending_dumps_.empty()) {
      return;
    }
    to_flush.swap(pending_dumps_);
  }

  std::error_code ec;
  std::filesystem::create_directories(dump_dir(), ec);
  const auto dump_path = dump_dir() / "strings.toml";

  // What the file already holds, so a string seen in an earlier session is not
  // written twice.
  std::unordered_set<uint64_t> existing_ids;
  std::string existing_content;
  if (ReadWholeFile(dump_path, existing_content)) {
    try {
      auto table = toml::parse(existing_content, PathUtf8(dump_path));
      if (const auto* entries = table["strings"].as_array()) {
        for (const auto& node : *entries) {
          if (const auto* entry = node.as_table()) {
            auto ctx = (*entry)["context"].value<std::string>();
            auto orig = (*entry)["original"].value<std::string>();
            if (ctx && orig) {
              existing_ids.insert(HashString(*ctx, *orig));
            }
          }
        }
      }
    } catch (const toml::parse_error&) {
      // Unparseable: keep the bytes, append after them, exactly like the fork.
    }
  }

  std::string new_content;
  size_t appended = 0;
  if (existing_content.empty()) {
    new_content =
        "# String dump - managed by ReXGlue runtime\n"
        "# Edit strings in the replace/ folder, not here.\n\n";
  }
  for (const auto& [id, entry] : to_flush) {
    if (!existing_ids.contains(id)) {
      new_content += fmt::format(
          "[[strings]]\n"
          "hash = \"{:016x}\"\n"
          "context = \"{}\"\n"
          "original = \"{}\"\n",
          entry.id, EscapeToml(entry.context), EscapeToml(entry.original));
      if (entry.guest_address != 0) {
        new_content += fmt::format("address = \"0x{:08X}\"\n", entry.guest_address);
      }
      new_content += "\n";
      ++appended;
    }
    std::lock_guard lock(mutex_);
    flushed_hashes_.insert(id);
  }
  if (appended == 0) {
    return;
  }

  // Written to a temporary and renamed over, so a crash mid-write cannot leave
  // half a file behind.
  std::filesystem::path tmp_path = dump_path;
  tmp_path += ".tmp";
  {
    std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      MC_WARN("StringDumper: cannot write {}", PathUtf8(tmp_path));
      return;
    }
    f << existing_content << new_content;
  }
  std::filesystem::remove(dump_path, ec);
  ec.clear();
  std::filesystem::rename(tmp_path, dump_path, ec);
  if (ec) {
    MC_WARN("StringDumper: rename failed: {}", ec.message());
    return;
  }
  MC_INFO("StringDumper: flushed {} new string(s) to {}", appended, PathUtf8(dump_path));
}

std::string StringDumper::FindReplacement(uint64_t string_id) const {
  auto it = replacements_.find(string_id);
  return it != replacements_.end() ? it->second : std::string();
}

std::string StringDumper::FindReplacement(std::string_view context,
                                          std::string_view original) const {
  return FindReplacement(HashString(context, original));
}

std::string StringDumper::EscapeToml(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '\\': result += "\\\\"; break;
      case '"':  result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:   result += c; break;
    }
  }
  return result;
}

std::filesystem::path DefaultStringsDir(const std::filesystem::path& game_data_root) {
  const std::string configured = REXCVAR_GET(string_folder);
  if (!configured.empty()) {
    // The cvar store holds UTF-8.
    return std::filesystem::path(std::u8string(configured.begin(), configured.end()));
  }
  return game_data_root / "strings";
}

}  // namespace mc::strings

#endif  // REXGLUE_HAS_XEO3_TARGET
