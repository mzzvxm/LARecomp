#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the stamp (section 3.1 of shaderpack/PIPELINE-PLAN.md)
// ===========================================================================
// mcla_shaders.stamp is a small key=value text file next to the packs, written LAST, after the
// packs were moved into place. A pack is used only while every line still matches what is true
// now, and the check is stat-only (a few file stats, no hashing, no DLL loaded) unless a size or
// time moved:
//
//   stamp_format, builder_version     the port's own rules (flags, masks, checks, layout): bump
//                                     kBuilderVersion by hand when they change
//   pack_version                      the reader's contract (pack_format.h)
//   generator_revision                the translator's commit (xenos_recomp::Revision())
//   shader_common_xxh3                the translator's embedded shader_common.h: forgetting to bump
//                                     a revision cannot go unnoticed
//   dxc_version (information)         what the compiler said about itself
//   dxcompiler_size/_mtime/_xxh3,
//   dxil_size/_mtime/_xxh3            a changed DLL changes blobs; size and time are compared at
//                                     every start, the hash only when those moved
//   formats                           what the pack carries (Format mask): a pack with only DXIL does
//                                     not satisfy a Vulkan start
//   xex_size/_mtime, rpf_size/_mtime  the game files the pack was built from
//   rpf_shaders_xxh3,                 the containers found in them (containers.h fingerprints): lets a
//   xex_containers_xxh3               copied game folder (new times, same content) skip the rebuild
//   pack_size, pack_xxh3              integrity and diagnosis (pack_xxh3 is what the renderer logs)
//   larecomp_version (information)    deliberately not a trigger: the packs are rebuilt when their
//                                     inputs change, not on every release
//
// The HDR variant step adds lines of its own (hdr_*), see builder.h.
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::shaderpack {

// Hand bumped: the stamp's layout, and the builder's rules.
inline constexpr uint32_t kStampFormat = 1;
inline constexpr uint32_t kBuilderVersion = 1;

// The names of the HDR step's lines the builder itself writes and checks.
inline constexpr const char* kStampHdrFormats = "hdr_formats";           // the formats the HDR pack carries
inline constexpr const char* kStampHdrMainPackXxh3 = "hdr_main_pack_xxh3";  // the main pack it was built beside
inline constexpr const char* kStampHdrPackSize = "hdr_pack_size";         // size of mcla_shaders_hdr.v1.pack
inline constexpr const char* kStampHdrPrefix = "hdr_";

struct StampLine {
  std::string key;
  std::string value;
};

class Stamp {
 public:
  // Replaces the line with this key, or appends it. Values are single line text.
  void Set(std::string_view key, std::string value);
  void Set(std::string_view key, uint64_t value);
  void SetHex(std::string_view key, uint64_t value);  // 16 hex digits
  void RemovePrefix(std::string_view prefix);

  const std::string* Get(std::string_view key) const;
  // False when the line is missing or is not a number (decimal / 16 hex digits).
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

// A file's size and last write time (microseconds since the file clock's epoch: only ever compared
// with itself on the same machine).
struct FileStat {
  bool exists = false;
  uint64_t size = 0;
  int64_t mtime_us = 0;
};
FileStat StatFile(const std::filesystem::path& path);
// XXH3-64 of a whole file.
bool HashFile(const std::filesystem::path& path, uint64_t* xxh3);

// Everything the main stamp records, gathered by the builder.
struct StampInputs {
  uint32_t formats = 0;
  FileStat xex;
  FileStat rpf;
  uint64_t rpf_fingerprint = 0;
  uint64_t image_fingerprint = 0;
  FileStat dxc_compiler;  // exists == false: the file was not there, no lines
  FileStat dxc_dil;
  uint64_t dxc_compiler_xxh3 = 0;
  uint64_t dxc_dil_xxh3 = 0;
  std::string dxc_version;
  uint64_t pack_size = 0;
  uint64_t pack_xxh3 = 0;
  std::string app_version;
};
Stamp MakeMainStamp(const StampInputs& inputs);

// The file names of the compiler and the signing library inside the dxc folder (platform
// dependent; the same ones dxc.cpp loads).
std::string DxcCompilerFileName();
std::string DxcDilFileName();

}  // namespace larecomp::shaderpack
