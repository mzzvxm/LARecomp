#pragma once
// ===========================================================================
// LARecomp city pack -- the stamp
// ===========================================================================
// mcla_city.stamp is a small key=value text file next to the pack, written LAST (after the pack was moved into place), the way the shader packs'
// stamp is (src/shaderpack/stamp.h). The pack is used while every line still matches what is true now; the check is stat only (a few file stats,
// nothing hashed, nothing decompressed):
//
//   stamp_format, builder_version, pack_format_version   this library's own rules (pack_format.h): bump the numbers there by hand
//   source_size, source_mtime_us                         xarchive_cache.rpf, the game file the pack was built from
//   city_overlay                                         what xarchive_mods.rpf (the mod loader's overlay: a modded sector is built from it) holds of the CITY: a hash of
//                                                        the files under resources/city/sc in it (0: no mod archive, or none of those). A mod archive that changes for
//                                                        another reason (a car, a song, a string table) does not make the pack stale: the first start after the launcher's
//                                                        setup would otherwise rebuild it for nothing
//   mods_size, mods_mtime_us                             the mod archive's size and time: information only, never compared
//   drop_mask, positions                                 what the builder was asked for (a pack with decals in it is not the default's)
//   pack_size, pack_hash                                 integrity and diagnosis (the hash of the table and the name heap)
//   sectors, hi_triangles, lod_triangles, lamps          information
//   app_version                                          information, deliberately not a trigger: the pack is rebuilt when its inputs change, not on every release
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::citypack {

inline constexpr uint32_t kStampFormat = 2;

struct StampLine {
  std::string key;
  std::string value;
};

class Stamp {
 public:
  void Set(std::string_view key, std::string value);
  void Set(std::string_view key, uint64_t value);
  void SetHex(std::string_view key, uint64_t value);  // 16 hex digits
  const std::string* Get(std::string_view key) const;
  bool GetU64(std::string_view key, uint64_t* value) const;
  bool GetHex(std::string_view key, uint64_t* value) const;
  const std::vector<StampLine>& lines() const { return lines_; }
  bool empty() const { return lines_.empty(); }

  std::string ToText() const;
  static Stamp Parse(std::string_view text);
  static bool Read(const std::filesystem::path& path, Stamp* out, std::string* error);
  // Writes <path>.tmp and moves it into place.
  bool Write(const std::filesystem::path& path, std::string* error) const;

 private:
  std::vector<StampLine> lines_;
};

// A file's size and last write time (microseconds since the file clock's epoch: only ever compared with itself on the same machine).
struct FileStat {
  bool exists = false;
  uint64_t size = 0;
  int64_t mtime_us = 0;
};
FileStat StatFile(const std::filesystem::path& path);

// What the builder says it built from and with.
struct StampInputs {
  FileStat source;
  FileStat mods;               // information only (the stamp keeps it, the check does not compare it)
  uint64_t city_overlay = 0;   // CityOverlayHash of the mod archive (builder.h)
  uint32_t drop_mask = 0;
  uint32_t positions = 0;
  uint64_t pack_size = 0;
  uint64_t pack_hash = 0;
  uint32_t sectors = 0;
  uint64_t hi_triangles = 0;
  uint64_t lod_triangles = 0;
  uint32_t lamps = 0;          // the lamps section's lights (information)
  std::string app_version;
};
Stamp MakeStamp(const StampInputs& inputs);

struct StampCheck {
  bool current = false;
  std::string why;  // when not current: the first line that did not match, in plain words
};
// Is the pack in `out_dir` current for these inputs? `expect` carries source, city_overlay, drop_mask and positions (the others are ignored).
StampCheck CheckStamp(const std::filesystem::path& out_dir, const StampInputs& expect);

}  // namespace larecomp::citypack
