#pragma once
// String dump and replacement store for the MCLA string table bridge
// (string_table.cpp).
//
// Vendored from the ReXGlue fork's rex::system::StringDumper so larecomp builds
// against the stock SDK, which has no rex/system/string_dumper.h. Same hash
// (FNV-1a over context, a NUL, then the text), same files, same TOML schema, so
// dumps and replacement files written by either one work with the other:
//
//   <strings folder>/dump/strings.toml          what was seen
//   <strings folder>/replace/*.toml             what to show instead
//
// On the fork the SDK owns one of these inside KernelState and its kernel string
// exports (sprintf, lstrcpyA...) feed it too; string_table.cpp keeps using that
// instance there, because two dumpers flushing the same strings.toml would race
// and drop entries. This class is the stock-SDK half.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>

// Defined by the fork's SDK when it has its own dumper, by string_dumper.cpp
// otherwise. Same names either way, so a toml written for one works with both.
REXCVAR_DECLARE(bool, string_dump_enabled);
REXCVAR_DECLARE(bool, string_replace_enabled);
REXCVAR_DECLARE(std::string, string_folder);
REXCVAR_DECLARE(std::string, string_language);

namespace mc::strings {

class StringDumper {
 public:
  static constexpr size_t kAutoFlushThreshold = 64;

  explicit StringDumper(std::filesystem::path strings_dir);

  StringDumper(const StringDumper&) = delete;
  StringDumper& operator=(const StringDumper&) = delete;

  // Rebuilds the hash -> replacement index from replace/*.toml.
  void Rescan();

  // Records a string seen in the game. Thread-safe.
  void DumpString(std::string_view context, std::string_view original,
                  uint32_t guest_address = 0);

  // Appends every pending string to dump/strings.toml.
  void FlushDump();

  // The replacement for this string, or empty when there is none.
  [[nodiscard]] std::string FindReplacement(uint64_t string_id) const;
  [[nodiscard]] std::string FindReplacement(std::string_view context,
                                            std::string_view original) const;

  static uint64_t HashString(std::string_view context, std::string_view text);

  std::filesystem::path dump_dir() const { return strings_dir_ / "dump"; }
  std::filesystem::path replace_dir() const { return strings_dir_ / "replace"; }

 private:
  struct Entry {
    uint64_t id = 0;
    std::string context;
    std::string original;
    uint32_t guest_address = 0;
  };

  bool LoadReplacementFile(const std::filesystem::path& path);
  static std::string EscapeToml(std::string_view input);

  std::filesystem::path strings_dir_;
  std::unordered_map<uint64_t, std::string> replacements_;

  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> pending_dumps_;
  std::unordered_set<uint64_t> flushed_hashes_;
};

// The folder the fork's runtime picks: the string_folder cvar, or <game
// data>/strings when that is empty.
std::filesystem::path DefaultStringsDir(const std::filesystem::path& game_data_root);

}  // namespace mc::strings
