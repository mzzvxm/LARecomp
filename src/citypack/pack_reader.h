#pragma once
// ===========================================================================
// LARecomp city pack -- the reader (the streamer's and the tool's)
// ===========================================================================
// Open reads the header and the sector table (about 70 KB) and checks the table's hash; ReadMesh reads and decodes ONE blob (a sector's hi detail or its
// far lod mesh) and may be called from any thread (each call opens the file itself). Nothing is kept open, so the pack can be replaced while the game
// runs (the next Open sees the new one).
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "citypack/lamp_table.h"
#include "citypack/pack_format.h"

namespace larecomp::citypack {

class PackReader {
 public:
  // False (with `error`) for a file that is missing, not a city pack, another format version, or whose table is damaged.
  bool Open(const std::filesystem::path& file, std::string* error);
  bool is_open() const { return open_; }
  const std::filesystem::path& path() const { return path_; }

  const PackHeader& header() const { return header_; }
  uint32_t sector_count() const { return uint32_t(sectors_.size()); }
  const SectorRecord& sector(uint32_t index) const { return sectors_[index]; }
  const std::vector<SectorRecord>& sectors() const { return sectors_; }
  std::string_view name(uint32_t index) const;

  // The raw bytes of a blob (one read), and the decoded mesh. A sector without that mesh: false with an empty `error`.
  bool ReadBlob(uint32_t sector, MeshKind kind, std::vector<uint8_t>* out, std::string* error) const;
  bool ReadMesh(uint32_t sector, MeshKind kind, MeshData* out, std::string* error) const;

  // The lamps section (lamp_table.h): one read, checked against the header's hash and decoded. A pack without one (builder version 1, or lights.xlm was not readable when it was built):
  // false with an empty `error`; any other false is a damaged section.
  bool has_lamps() const { return open_ && header_.has_lamps(); }
  bool ReadLamps(LampTable* out, std::string* error) const;

 private:
  bool open_ = false;
  std::filesystem::path path_;
  PackHeader header_;
  std::vector<SectorRecord> sectors_;
  std::string names_;
};

// Every blob of the pack decoded and compared with its record (counts, bounds): the tool's `validate`. Returns the number of problems (the first few are
// in `problems`); 0 = the pack is whole.
uint32_t ValidatePack(const PackReader& reader, std::vector<std::string>* problems);

}  // namespace larecomp::citypack
